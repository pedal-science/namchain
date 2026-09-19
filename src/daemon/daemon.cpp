// namchaind: the reference daemon (plan.md §8.2) and the control thread of docs/rig-pipeline.md.
//
// Platform note: the control transport is a Unix SOCK_SEQPACKET socket plus signalfd, which is the
// Linux spelling of "block in one poll set". On a platform without signalfd the class is declared
// but not defined, and the daemon is not built (CMake builds it on Linux only).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#if defined(__linux__)

#include "namchain/daemon/daemon.h"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <json.hpp>

#include "namchain/graph/rig.h"
#include "namchain/pcm.h"
#include "namchain/rt/scheduling.h"
#include "namchain/rt_check.h"
#include "namchain/version.h"

namespace namchain
{
namespace daemon
{
namespace
{

/// Every plan gets its own arena; 1 MiB is what the soak compiles any sane rig out of, and a rig
/// that needs more gets "arena exhausted" rather than a silently truncated graph.
constexpr std::size_t kArenaBytes = 1u << 20;

/// One SEQPACKET message. A request names a rig, it does not carry one, so this is generous; a
/// message that fills it is refused rather than truncated into something that parses.
constexpr std::size_t kMaxMessageBytes = 256u * 1024;

/// The protocol version this daemon speaks. Incremented when the request/response shape changes.
constexpr int kProtocolVersion = 1;

/// A local control socket does not need many peers, and an unbounded accept loop is how a daemon
/// runs out of descriptors. Past the cap the connection is closed immediately rather than queued.
constexpr std::size_t kMaxClients = 16;

void delete_plan_slot(void* object)
{
  delete static_cast<PlanSlot*>(object);
}

/// mlockall(MCL_CURRENT), never MCL_FUTURE (§14.10). The soft limit is raised to the hard one first,
/// because on a host whose administrator has already granted a large LimitMEMLOCK the soft limit is
/// the only thing in the way.
bool lock_process_memory()
{
  rlimit limit{};
  if (::getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_cur != limit.rlim_max)
  {
    limit.rlim_cur = limit.rlim_max;
    ::setrlimit(RLIMIT_MEMLOCK, &limit); // best effort: the hard limit is the administrator's call
  }
  return ::mlockall(MCL_CURRENT) == 0;
}

std::string ok_response(const std::string& id, nlohmann::json body)
{
  body["ok"] = true;
  if (!id.empty())
    body["id"] = id;
  return body.dump();
}

std::string error_response(const std::string& id, const std::string& code, const std::string& message)
{
  nlohmann::json body;
  body["ok"] = false;
  if (!id.empty())
    body["id"] = id;
  body["error"] = {{"code", code}, {"message", message}};
  return body.dump();
}

} // namespace

const char* device_state_name(DeviceState state)
{
  switch (state)
  {
  case DeviceState::Closed:
    return "closed";
  case DeviceState::Running:
    return "running";
  case DeviceState::Lost:
    return "lost";
  }
  return "unknown";
}

Daemon::Daemon(Config config) : config_(std::move(config)) {}

Daemon::~Daemon()
{
  stop();
}

void Daemon::request_stop()
{
  stop_requested_.store(true);
}

bool Daemon::setup_signals(std::string& error)
{
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGHUP);

  // Blocked before the render thread exists, so every thread inherits the block and the signalfd is
  // the only reader. Blocking after the thread started would leave a window where the default
  // disposition killed the process. The old mask is kept and restored by stop().
  if (::pthread_sigmask(SIG_BLOCK, &mask, &previous_mask_) != 0)
  {
    error = std::string("pthread_sigmask: ") + std::strerror(errno);
    return false;
  }
  mask_saved_ = true;
  const int fd = ::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
  if (fd < 0)
  {
    error = std::string("signalfd: ") + std::strerror(errno);
    return false;
  }
  signal_fd_ = fd;
  return true;
}

bool Daemon::bind_socket(std::string& error)
{
  const std::string& path = config_.socket_path;
  if (path.empty())
  {
    error = "the socket path is empty";
    return false;
  }

  // RuntimeDirectory= creates this in the unit; a hand-run needs it too, and creating it here makes
  // the socket path the only thing an operator has to get right.
  std::error_code ignored;
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent, ignored);

  const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0)
  {
    error = std::string("socket: ") + std::strerror(errno);
    return false;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path))
  {
    ::close(fd);
    error = "socket path is too long: " + path;
    return false;
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

  // A node left by a previous run is stale, and bind() would refuse it. Removing it before binding
  // is what makes `systemctl restart` work.
  ::unlink(path.c_str());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
  {
    error = "bind " + path + ": " + std::strerror(errno);
    ::close(fd);
    return false;
  }
  if (::chmod(path.c_str(), static_cast<mode_t>(config_.socket_mode)) != 0)
  {
    error = "chmod " + path + ": " + std::strerror(errno);
    ::close(fd);
    ::unlink(path.c_str());
    return false;
  }
  if (::listen(fd, 8) != 0)
  {
    error = "listen " + path + ": " + std::strerror(errno);
    ::close(fd);
    ::unlink(path.c_str());
    return false;
  }
  listen_fd_ = fd;
  return true;
}

void Daemon::close_socket()
{
  for (const int client : clients_)
    ::close(client);
  clients_.clear();
  // The path is unlinked only when this process bound it: a failed bind must not remove a socket a
  // previous (or another) daemon is still serving.
  if (listen_fd_ >= 0)
  {
    ::close(listen_fd_);
    listen_fd_ = -1;
    if (!config_.socket_path.empty())
      ::unlink(config_.socket_path.c_str());
  }
}

bool Daemon::push_slot(PlanSlot* slot)
{
  PlanCommand command;
  command.kind = PlanCommand::Kind::PublishPlan;
  command.slot = slot;
  if (commands_.push(command))
    return true;
  // A full command ring means the render thread is not draining, which is an engine bug rather than
  // a load condition. Free the plan here rather than leaking it.
  delete slot;
  if (config_.verbose)
    std::fprintf(stderr, "[namchaind] publication ring is full; plan dropped\n");
  return false;
}

bool Daemon::publish_param(const std::string& node, const std::string& param, double value, std::string& error)
{
  if (last_published_ == nullptr || last_published_->plan == nullptr)
  {
    error = "no plan is loaded; rig.load first";
    return false;
  }
  if (!std::isfinite(value))
  {
    error = "'" + node + "." + param + "' is not finite";
    return false;
  }

  // resolve_param reads immutable plan structure only, so doing it on the control thread while the
  // render thread runs the same plan is safe. The *write* goes on the ring and happens on the render
  // thread; that is the whole point of the command.
  std::size_t node_index = 0;
  uint32_t param_id = 0;
  if (!last_published_->plan->resolve_param(node, param, node_index, param_id))
  {
    error = "node '" + node + "' has no settable param '" + param + "'";
    return false;
  }

  PlanCommand command;
  command.kind = PlanCommand::Kind::SetParam;
  command.generation = last_published_->generation;
  command.node = static_cast<uint32_t>(node_index);
  command.param = param_id;
  command.value = static_cast<float>(value);
  if (!commands_.push(command))
  {
    error = "the command ring is full; the render thread is not consuming edits";
    return false;
  }
  return true;
}

bool Daemon::load_rig(const std::string& rig_path, std::string& error)
{
  graph::Graph graph;
  if (!graph::load_graph_json(rig_path, graph, error))
    return false;

  render::CompileOptions options;
  options.max_quantum = static_cast<int>(config_.period_size);
  options.sample_rate = static_cast<double>(config_.sample_rate);

  auto slot = std::make_unique<PlanSlot>(kArenaBytes);
  slot->plan = graph::RenderPlan::compile(graph, options, slot->arena, error);
  if (slot->plan == nullptr)
    return false;

  // The first process() call on a freshly built node is tens of times its steady-state cost, and a
  // stall that long can drain the device ring. It is control-thread work, before publication (§3.3).
  slot->plan->prewarm();
  slot->rig = rig_path;
  const uint64_t generation = generation_ + 1;
  slot->generation = generation;

  PlanSlot* raw = slot.get();
  const int buffers = slot->plan->buffer_count();
  if (!push_slot(slot.release()))
  {
    error = "the publication ring is full; the render thread is not consuming plans";
    return false;
  }
  generation_ = generation;
  current_rig_ = rig_path;
  last_published_ = raw;

  if (config_.verbose)
  {
    std::fprintf(stderr, "[namchaind] published %s (generation %llu, %d buffers)\n", rig_path.c_str(),
                 static_cast<unsigned long long>(generation_), buffers);
  }
  return true;
}

bool Daemon::start(std::string& error)
{
  if (started_)
  {
    error = "the daemon is already started";
    return false;
  }
  if (config_.sample_rate == 0 || config_.period_size == 0 || config_.periods == 0)
  {
    error = "sample_rate, period_size and periods must all be non-zero";
    return false;
  }
  if (config_.capture_channels == 0 || config_.playback_channels == 0)
  {
    error = "capture_channels and playback_channels must both be non-zero";
    return false;
  }
  if (config_.rt_priority < 0 || config_.rt_priority > 99)
  {
    error = "rt_priority must be in [0, 99]";
    return false;
  }
  if (config_.output_queue_floor_periods > config_.output_queue_periods)
  {
    error = "output_queue_floor_periods cannot exceed output_queue_periods";
    return false;
  }
  if (config_.output_queue_periods != 0 && config_.output_queue_periods >= config_.periods)
  {
    // A cap at or above the ring's capacity can never bind, so it is a configuration mistake rather
    // than a no-op: the ring itself becomes the limit and the number means nothing.
    error = "output_queue_periods must be less than periods";
    return false;
  }

  if (!setup_signals(error))
    return false;
  if (!bind_socket(error))
    return false;

  // The startup rig is compiled before the clock exists, so the first plan is there when the first
  // block runs (rig-pipeline.md §5.1 item 2). A failure here is fatal: the operator named a rig.
  if (!config_.rig.empty())
  {
    std::string path;
    if (!resolve_startup_rig(config_, config_.rig, path, error))
      return false;
    if (!load_rig(path, error))
      return false;
  }

  running_.store(true);
  render_thread_ = std::thread([this] { render_loop(); });
  started_ = true;
  return true;
}

void Daemon::stop()
{
  stop_requested_.store(true);
  running_.store(false);
  if (render_thread_.joinable())
    render_thread_.join();

  // Anything published but never adopted (a plan loaded before the render thread drained it) is
  // still owned by the command ring. Free it here; the loop has stopped, so nothing else can.
  PlanCommand command;
  while (commands_.pop(command))
    if (command.kind == PlanCommand::Kind::PublishPlan)
      delete command.slot;

  if (current_slot_ != nullptr)
  {
    delete current_slot_;
    current_slot_ = nullptr;
  }
  last_published_ = nullptr;
  close_socket();
  if (signal_fd_ >= 0)
  {
    ::close(signal_fd_);
    signal_fd_ = -1;
  }
  if (mask_saved_)
  {
    ::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
    mask_saved_ = false;
  }
  reclaimer_.collect();
  started_ = false;
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------

void Daemon::set_device_state(DeviceState state)
{
  const int prior = device_state_.exchange(static_cast<int>(state));
  if (prior == static_cast<int>(state))
    return;
  if (state == DeviceState::Lost)
    device_lost_.fetch_add(1);
  else if (state == DeviceState::Running && prior == static_cast<int>(DeviceState::Lost))
    device_restored_.fetch_add(1);
}

bool Daemon::open_device(std::string& error)
{
  BackendConfig backend;
  backend.sample_rate = config_.sample_rate;
  backend.period_size = config_.period_size;
  backend.periods = config_.periods;
  backend.capture_channels = config_.capture_channels;
  backend.playback_channels = config_.playback_channels;
  backend.device = config_.device;
  backend.require_exact_period = true; // §9.5: a silently rounded quantum is not acceptable
  backend.output_queue_periods = config_.output_queue_periods;
  backend.output_queue_floor_periods = config_.output_queue_floor_periods;

  device_ = open_backend(backend, config_.backend, error);
  if (device_ == nullptr)
    return false;

  const BackendInfo& info = device_->info();
  // The plan was compiled for the configured quantum and rate. A backend that got something else
  // would run frames past every node's buffer, so refuse it rather than adopt it.
  if (info.period_size != config_.period_size || info.sample_rate != config_.sample_rate)
  {
    error = "device gave " + std::to_string(info.period_size) + " frames at " +
            std::to_string(info.sample_rate) + " Hz, configured for " + std::to_string(config_.period_size) +
            " at " + std::to_string(config_.sample_rate) + " Hz";
    close_device();
    return false;
  }
  if (info.capture_bytes_per_frame == 0 || info.playback_bytes_per_frame == 0)
  {
    error = "device reports a zero bytes-per-frame";
    close_device();
    return false;
  }

  // Sized before the stream starts, and before mlockall covers them.
  capture_buffer_.assign(static_cast<std::size_t>(config_.period_size) * info.capture_bytes_per_frame, 0);
  playback_buffer_.assign(static_cast<std::size_t>(config_.period_size) * info.playback_bytes_per_frame, 0);
  mono_in_.assign(config_.period_size, 0.0f);
  mono_out_.assign(config_.period_size, 0.0f);

  active_rate_.store(info.sample_rate);
  active_period_.store(info.period_size);
  active_capture_channels_.store(info.capture_channels);
  active_playback_channels_.store(info.playback_channels);
  return true;
}

void Daemon::close_device()
{
  if (device_ == nullptr)
    return;
  if (device_started_)
  {
    device_->stop();
    device_started_ = false;
  }
  device_.reset();
}

void Daemon::fail_device(const std::string& reason)
{
  if (config_.verbose)
    std::fprintf(stderr, "[namchaind] device: %s\n", reason.c_str());
  render_errors_.fetch_add(1);
  close_device();
  set_device_state(DeviceState::Lost);
}

void Daemon::drain_commands()
{
  PlanCommand command;
  while (commands_.pop(command))
  {
    if (command.kind == PlanCommand::Kind::SetParam)
    {
      // The command names the generation it was resolved against. A plan swap queued ahead of it is
      // processed first (the ring is FIFO), so the only way to be stale is a patch that lost a race
      // with a structural reload -- in which case dropping it is the intended coalescing, not a bug.
      if (current_slot_ == nullptr || current_slot_->generation != command.generation)
      {
        params_stale_.fetch_add(1);
        continue;
      }
      if (current_slot_->plan->set_param(command.node, command.param, command.value))
        params_applied_.fetch_add(1);
      else
        params_stale_.fetch_add(1); // an unknown param is a control-side bug; counted, not fatal
      continue;
    }

    PlanSlot* next = command.slot;
    if (next == nullptr)
      continue;
    PlanSlot* previous = current_slot_;
    current_slot_ = next;
    adopted_generation_.store(next->generation);
    // The old plan may still be referenced by a block this thread has not finished. It is *retired*
    // here and freed by the control thread after the grace window (rig-pipeline.md §6.4); never
    // deleted on this thread.
    if (previous != nullptr)
      reclaimer_.retire(previous, &delete_plan_slot);
  }
}

void Daemon::render_loop()
{
  const rt::SchedulingState scheduling = rt::apply_scheduling(config_.rt_priority, config_.cpu);
  rt_policy_.store(scheduling.policy);
  rt_priority_.store(scheduling.priority);
  rt_realtime_.store(scheduling.realtime);
  if (config_.verbose)
    std::fprintf(stderr, "[namchaind] render thread: %s\n", rt::describe_scheduling(scheduling).c_str());

  std::string error;
  bool memory_locked = false;
  constexpr int kDeviceWaitMs = 1000;

  while (running_.load())
  {
    if (device_ == nullptr)
    {
      if (!open_device(error))
      {
        set_device_state(DeviceState::Lost);
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1u, config_.device_retry_ms)));
        continue;
      }
      if (!device_->start(error))
      {
        close_device();
        set_device_state(DeviceState::Lost);
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1u, config_.device_retry_ms)));
        continue;
      }
      device_started_ = true;
      set_device_state(DeviceState::Running);
      // Last, so that the plan, the model and every stream buffer are covered by MCL_CURRENT.
      if (config_.lock_memory && !memory_locked)
      {
        memory_locked = lock_process_memory();
        memory_locked_.store(memory_locked);
        if (!memory_locked && config_.verbose)
          std::fprintf(stderr, "[namchaind] mlockall(MCL_CURRENT) refused; the run is not pinned\n");
      }
    }

    const BackendInfo& info = device_->info();
    if (info.capture_exhausted)
    {
      // A file backend that ran out of input. Not a device loss; there is simply no more audio.
      close_device();
      set_device_state(DeviceState::Closed);
      break;
    }

    if (!device_->wait_capture(kDeviceWaitMs, error))
    {
      if (!running_.load())
        break;
      fail_device(error);
      continue;
    }
    if (device_->read_capture(capture_buffer_.data(), info.period_size, 1000, error) < 0)
    {
      if (!running_.load())
        break;
      fail_device(error);
      continue;
    }

    {
      // The render path proper: everything here was bound before publication, so it must not touch
      // the allocator, take a lock or make a syscall (namchain/rt_check.h).
      RtScope scope;
      drain_commands();

      graph::RenderPlan* plan = current_slot_ == nullptr ? nullptr : current_slot_->plan.get();
      const std::size_t width = info.bytes_per_sample;
      for (unsigned i = 0; i < info.period_size; ++i)
        mono_in_[i] = pcm_decode(capture_buffer_.data() + static_cast<std::size_t>(i) * info.capture_channels * width,
                                 info.format);

      if (plan != nullptr)
        plan->execute(mono_in_.data(), mono_out_.data(), static_cast<int>(info.period_size));
      else
        std::fill(mono_out_.begin(), mono_out_.end(), 0.0f);

      // Mute is a zeroing of the *emitted* copy, applied after execute so the chain keeps running:
      // a muted slot whose inference is skipped freezes state and clicks on unmute (G14).
      if (mute_.load())
        std::fill(mono_out_.begin(), mono_out_.end(), 0.0f);

      std::fill(playback_buffer_.begin(), playback_buffer_.end(), 0);
      for (unsigned i = 0; i < info.period_size; ++i)
        pcm_encode(playback_buffer_.data() + static_cast<std::size_t>(i) * info.playback_channels * width,
                   info.format, mono_out_[i]);
    }

    if (device_->try_write_playback(playback_buffer_.data(), info.period_size, error) < 0)
    {
      if (!running_.load())
        break;
      fail_device(error);
      continue;
    }

    reclaimer_.advance_epoch();
    blocks_.fetch_add(1);
    xruns_.store(device_->driver_xruns());
  }

  close_device();
}

// ---------------------------------------------------------------------------
// Control loop and protocol
// ---------------------------------------------------------------------------

std::string Daemon::handle_request(const std::string& request)
{
  nlohmann::json doc;
  try
  {
    doc = nlohmann::json::parse(request);
  }
  catch (const std::exception& e)
  {
    return error_response("", "bad_json", std::string("not valid JSON: ") + e.what());
  }
  if (!doc.is_object())
    return error_response("", "bad_request", "the request must be a JSON object");

  std::string id;
  if (doc.contains("id"))
  {
    if (!doc["id"].is_string())
      return error_response("", "bad_request", "'id' must be a string");
    id = doc["id"].get<std::string>();
  }
  if (!doc.contains("method") || !doc["method"].is_string())
    return error_response(id, "bad_request", "the request needs a string 'method'");
  const std::string method = doc["method"].get<std::string>();

  // --- engine.hello: negotiate the protocol version -----------------------
  if (method == "engine.hello")
  {
    int protocol = kProtocolVersion;
    if (doc.contains("protocol"))
    {
      if (!doc["protocol"].is_number_integer())
        return error_response(id, "bad_request", "'protocol' must be an integer");
      protocol = doc["protocol"].get<int>();
    }
    if (protocol != kProtocolVersion)
    {
      return error_response(id, "unsupported_protocol",
                            "namchaind speaks protocol " + std::to_string(kProtocolVersion) + ", the client asked for " +
                              std::to_string(protocol));
    }
    nlohmann::json body;
    body["protocol"] = kProtocolVersion;
    body["role"] = "namchaind";
    body["version"] = NAMCHAIN_VERSION_STRING;
    return ok_response(id, body);
  }

  // --- engine.status ------------------------------------------------------
  if (method == "engine.status")
  {
    nlohmann::json status;
    status["role"] = "namchaind";
    status["version"] = NAMCHAIN_VERSION_STRING;
    status["protocol"] = kProtocolVersion;
    status["state"] = device_state_name(device_state());
    status["backend"] = backend_kind_name(config_.backend);
    status["device"] = config_.device;
    status["sample_rate"] = active_rate_.load() == 0 ? config_.sample_rate : active_rate_.load();
    status["period_size"] = active_period_.load() == 0 ? config_.period_size : active_period_.load();
    status["capture_channels"] = active_capture_channels_.load();
    status["playback_channels"] = active_playback_channels_.load();
    status["rig"] = current_rig_;
    status["generation"] = generation_;
    status["adopted_generation"] = adopted_generation_.load();
    status["blocks"] = blocks_.load();
    status["xruns"] = xruns_.load();
    status["device_lost"] = device_lost_.load();
    status["device_restored"] = device_restored_.load();
    status["render_errors"] = render_errors_.load();
    status["mute"] = mute_.load();
    status["memory_locked"] = memory_locked_.load();
    status["realtime"] = rt_realtime_.load();
    status["rt_policy"] = rt_policy_.load();
    status["rt_priority"] = rt_priority_.load();
    status["retired_pending"] = static_cast<std::uint64_t>(reclaimer_.pending());
    status["params_applied"] = params_applied_.load();
    status["params_stale"] = params_stale_.load();
    // Two honest labels: parameter edits are ramped inside the node (no recompile, no step); a
    // structural reload is still a cold swap, because the crossfade is not built.
    status["param_edits"] = "ramped";
    status["structural_transitions"] = "cold-swap";
    nlohmann::json body;
    body["status"] = status;
    return ok_response(id, body);
  }

  // --- engine.mute / engine.panic ----------------------------------------
  if (method == "engine.mute")
  {
    bool muted = true;
    if (doc.contains("mute"))
    {
      if (!doc["mute"].is_boolean())
        return error_response(id, "bad_request", "'mute' must be a boolean");
      muted = doc["mute"].get<bool>();
    }
    mute_.store(muted);
    nlohmann::json body;
    body["mute"] = muted;
    return ok_response(id, body);
  }
  if (method == "engine.panic")
  {
    // The other route to silence, and the only one that is unconditional. There is no transition
    // machinery to abort yet (M2), so panic is mute plus a marker in the response.
    mute_.store(true);
    nlohmann::json body;
    body["mute"] = true;
    body["panic"] = true;
    return ok_response(id, body);
  }

  // --- rig.load / rig.get -------------------------------------------------
  if (method == "rig.load")
  {
    if (!doc.contains("rig") || !doc["rig"].is_string())
      return error_response(id, "bad_request", "rig.load needs a string 'rig'");
    const std::string name = doc["rig"].get<std::string>();
    std::string path;
    std::string error;
    if (!resolve_rig_name(config_, name, path, error))
      return error_response(id, "rig_not_found", error);
    if (!load_rig(path, error))
      return error_response(id, "compile_failed", error);
    nlohmann::json body;
    body["rig"] = path;
    body["generation"] = generation_;
    return ok_response(id, body);
  }
  if (method == "rig.get")
  {
    nlohmann::json body;
    body["rig"] = current_rig_;
    body["generation"] = generation_;
    body["loaded"] = current_rig_.empty() ? false : true;
    return ok_response(id, body);
  }
  if (method == "rig.patch")
  {
    // The `smooth` op class of §5.3: each op is a vtable dispatch on the render thread, no compile
    // and no fade. The response says how many were accepted; adoption is asynchronous, so a caller
    // that wants to observe the rendered result polls engine.status (params_applied).
    if (!doc.contains("ops") || !doc["ops"].is_array())
      return error_response(id, "bad_request", "rig.patch needs an 'ops' array");
    unsigned applied = 0;
    for (std::size_t i = 0; i < doc["ops"].size(); ++i)
    {
      const std::string where = "ops[" + std::to_string(i) + "]";
      const nlohmann::json& op = doc["ops"][i];
      if (!op.is_object())
        return error_response(id, "bad_request", where + " must be an object");
      if (op.contains("op") && (!op["op"].is_string() || op["op"].get<std::string>() != "set_param"))
        return error_response(id, "unsupported_op",
                              where + ": only 'set_param' is implemented");
      if (!op.contains("node") || !op["node"].is_string() || !op.contains("param") ||
          !op["param"].is_string() || !op.contains("value") || !op["value"].is_number())
      {
        return error_response(id, "bad_request", where + " needs string 'node' and 'param' and a numeric 'value'");
      }
      std::string error;
      if (!publish_param(op["node"].get<std::string>(), op["param"].get<std::string>(),
                         op["value"].get<double>(), error))
      {
        return error_response(id, "patch_rejected", where + ": " + error);
      }
      ++applied;
    }
    nlohmann::json body;
    body["applied"] = applied;
    body["generation"] = generation_;
    return ok_response(id, body);
  }

  return error_response(id, "unknown_method", "unknown method '" + method + "'");
}

int Daemon::run()
{
  if (!started_)
  {
    std::fprintf(stderr, "[namchaind] run() called before start()\n");
    return 1;
  }

  std::vector<char> message(kMaxMessageBytes);
  while (!stop_requested_.load())
  {
    std::vector<pollfd> fds;
    fds.push_back(pollfd{listen_fd_, POLLIN, 0});
    fds.push_back(pollfd{signal_fd_, POLLIN, 0});
    for (const int client : clients_)
      fds.push_back(pollfd{client, POLLIN, 0});

    const int timeout = std::max(1, config_.reclamation_timeout_ms);
    const int ready = ::poll(fds.data(), fds.size(), timeout);
    if (ready < 0)
    {
      if (errno == EINTR)
        continue;
      std::fprintf(stderr, "[namchaind] poll: %s\n", std::strerror(errno));
      stop();
      return 1;
    }

    // A client can vanish at any point in the loop below; collect the dead fds and close them after
    // the walk, so the index arithmetic stays honest.
    std::vector<int> dead;

    if ((fds[0].revents & POLLIN) != 0 && listen_fd_ >= 0)
    {
      const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (client >= 0)
      {
        if (clients_.size() < kMaxClients)
          clients_.push_back(client);
        else
          ::close(client);
      }
    }

    if ((fds[1].revents & POLLIN) != 0)
    {
      signalfd_siginfo info{};
      while (::read(signal_fd_, &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info)))
      {
        if (info.ssi_signo == SIGTERM || info.ssi_signo == SIGINT)
          stop_requested_.store(true);
        else if (info.ssi_signo == SIGHUP)
        {
          std::string path;
          std::string error;
          if (!config_.rig.empty() && resolve_startup_rig(config_, config_.rig, path, error) &&
              load_rig(path, error))
          {
            if (config_.verbose)
              std::fprintf(stderr, "[namchaind] SIGHUP: reloaded %s\n", path.c_str());
          }
          else
          {
            std::fprintf(stderr, "[namchaind] SIGHUP reload failed: %s\n", error.c_str());
          }
        }
      }
    }
    if (stop_requested_.load())
      break;

    for (std::size_t i = 0; i < clients_.size(); ++i)
    {
      const int client = clients_[i];
      const short revents = fds[2 + i].revents;
      if ((revents & (POLLIN | POLLHUP | POLLERR)) == 0)
        continue;

      const ssize_t got = ::recv(client, message.data(), message.size(), 0);
      if (got <= 0)
      {
        dead.push_back(client);
        continue;
      }

      std::string reply;
      if (static_cast<std::size_t>(got) == message.size())
        reply = error_response("", "message_too_large", "the request filled the message buffer");
      else
        reply = handle_request(std::string(message.data(), static_cast<std::size_t>(got)));

      // A peer that went away between recv and send is dead, not an error worth reporting.
      if (::send(client, reply.data(), reply.size(), MSG_NOSIGNAL) < 0)
        dead.push_back(client);
    }

    for (const int client : dead)
    {
      ::close(client);
      clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
    }

    // The reclamation window of rig-pipeline.md §5.3: this is the "somebody" that frees a plan once
    // its grace has closed, and the reason the poll timeout is neither zero nor infinite.
    reclaimer_.collect();
  }

  stop();
  return 0;
}

} // namespace daemon
} // namespace namchain

#endif // __linux__
