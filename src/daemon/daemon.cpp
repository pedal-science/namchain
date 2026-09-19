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
#include "namchain/graph/patch.h"
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
///
/// 2 added `rig.apply` (a whole document in memory), the document in `rig.get`, the per-chain array in
/// `engine.status`, a `chain` on `rig.patch` and `engine.mute`, and `features`. A version-1 client is
/// still served: every change is additive, and unknown fields are ignored by both sides, so the
/// negotiation accepts anything from 1 up to this (docs/ui-integration-plan.md §1.4).
constexpr int kProtocolVersion = 2;
constexpr int kOldestProtocolVersion = 1;

/// What this daemon can be relied on to do, so a client degrades rather than guesses. Grows as the
/// slices land (docs/ui-integration-plan.md §1.4): a client that does not see "stereo" does not offer
/// a pan control, and one that sees "schema" knows to build its inspector from `engine.schema`.
nlohmann::json protocol_features()
{
  return nlohmann::json::array({"multi_chain", "documents", "rig_apply", "rig_get_document",
                                "structural_crossfade", "smooth_params", "bypass", "guard",
                                "per_chain_mute"});
}

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

bool Daemon::push_slot(uint32_t chain, uint32_t chain_count, PlanSlot* slot)
{
  PlanCommand command;
  command.kind = PlanCommand::Kind::PublishPlan;
  command.slot = slot;
  command.chain = chain;
  command.chain_count = chain_count;
  if (commands_.push(command))
    return true;
  // A full command ring means the render thread is not draining, which is an engine bug rather than
  // a load condition. Free the plan here rather than leaking it.
  delete slot;
  if (config_.verbose)
    std::fprintf(stderr, "[namchaind] publication ring is full; plan dropped\n");
  return false;
}

int Daemon::find_chain(const std::string& chain_id) const
{
  for (std::size_t i = 0; i < current_document_.chains.size(); ++i)
  {
    if (current_document_.chains[i].id == chain_id)
      return static_cast<int>(i);
  }
  return -1;
}

int Daemon::find_chain_with_node(const std::string& node) const
{
  int found = -1;
  for (std::size_t i = 0; i < published_slots_.size(); ++i)
  {
    const PlanSlot* slot = published_slots_[i];
    if (slot == nullptr || slot->plan == nullptr)
      continue;
    const std::vector<std::string>& ids = slot->plan->node_ids();
    if (std::find(ids.begin(), ids.end(), node) == ids.end())
      continue;
    if (found >= 0)
      return -2; // more than one chain holds it, and guessing would edit an arbitrary one
    found = static_cast<int>(i);
  }
  return found;
}

bool Daemon::publish_param(const std::string& chain_id, const std::string& node, const std::string& param,
                           double value, std::string& error)
{
  int index = -1;
  if (!chain_id.empty())
  {
    index = find_chain(chain_id);
    if (index < 0)
    {
      error = "no chain '" + chain_id + "'";
      return false;
    }
  }
  else
  {
    index = find_chain_with_node(node);
    if (index == -2)
    {
      error = "node '" + node + "' is in more than one chain; name the chain";
      return false;
    }
    if (index < 0)
    {
      error = "no chain holds a node '" + node + "'";
      return false;
    }
  }

  PlanSlot* slot = published_slots_[static_cast<std::size_t>(index)];
  if (slot == nullptr || slot->plan == nullptr)
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
  if (!slot->plan->resolve_param(node, param, node_index, param_id))
  {
    error = "node '" + node + "' has no settable param '" + param + "'";
    return false;
  }

  PlanCommand command;
  command.kind = PlanCommand::Kind::SetParam;
  command.chain = static_cast<uint32_t>(index);
  command.generation = slot->generation;
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
  graph::Document document;
  if (!graph::load_document_json(rig_path, document, error))
    return false;
  return publish_document(document, rig_path, "file", std::filesystem::path(rig_path).parent_path().string(), error);
}

bool Daemon::publish_document(const graph::Document& document, const std::string& path, const std::string& source,
                              const std::string& base_dir, std::string& error)
{
  const graph::Validation validation = graph::validate(document);
  if (!validation.ok)
  {
    error = validation.message;
    return false;
  }
  if (document.chains.size() > static_cast<std::size_t>(kMaxChains))
  {
    error = "a document may hold at most " + std::to_string(kMaxChains) + " chains, this one has " +
            std::to_string(document.chains.size());
    return false;
  }

  // Relative model/IR paths resolve against the base the document arrived with, on a *copy*: the
  // document the engine remembers and hands back is the one the client wrote, so applying what
  // `rig.get` returned is a fixed point rather than a rewrite of every relative path.
  graph::Document resolved = document;
  graph::resolve_relative_paths(resolved, base_dir);

  render::CompileOptions options;
  options.max_quantum = static_cast<int>(config_.period_size);
  options.sample_rate = static_cast<double>(config_.sample_rate);

  // Which chains actually changed. An unchanged chain keeps the plan it has: republishing it would
  // recompile it -- reloading every model on this thread -- and crossfade it, for an edit somebody
  // made to a different chain.
  std::vector<bool> changed(document.chains.size(), true);
  std::size_t changed_count = document.chains.size();
  if (document.chains.size() == current_document_.chains.size())
  {
    changed_count = 0;
    for (std::size_t i = 0; i < document.chains.size(); ++i)
    {
      const graph::Chain& before = current_document_.chains[i];
      const graph::Chain& after = document.chains[i];
      changed[i] = before.id != after.id || before.name != after.name ||
                   before.input_channel != after.input_channel ||
                   before.output_channel != after.output_channel || !graph::same_graph(before.graph, after.graph);
      if (changed[i])
        ++changed_count;
    }
  }

  if (changed_count == 0 && published_slots_.size() == document.chains.size())
  {
    // Nothing about the running document would change. Where it came from is still worth recording --
    // it is what a later patch republishes under -- but the generation does not advance, because no
    // new plan exists and a client that polls for adoption should not see one.
    current_rig_ = path;
    current_source_ = source;
    current_base_dir_ = base_dir;
    current_name_ = document.name;
    return true;
  }

  // Room for every chain's command before any of them is pushed: a document publish is all-or-nothing,
  // and half a document installed is the one outcome worse than a rejected one.
  if (commands_.size() + changed_count > commands_.capacity())
  {
    error = "the command ring is full; the render thread is not consuming plans";
    return false;
  }

  const uint64_t generation = generation_ + 1;
  std::vector<std::unique_ptr<PlanSlot>> built;
  for (const graph::Chain& chain : document.chains)
  {
    if (chain.input_channel >= static_cast<int>(config_.capture_channels))
    {
      error = "chain '" + chain.id + "' reads capture channel " + std::to_string(chain.input_channel) +
              ", but capture_channels is " + std::to_string(config_.capture_channels);
      return false;
    }
    if (chain.output_channel >= static_cast<int>(config_.playback_channels))
    {
      error = "chain '" + chain.id + "' writes playback channel " + std::to_string(chain.output_channel) +
              ", but playback_channels is " + std::to_string(config_.playback_channels);
      return false;
    }
  }
  for (std::size_t i = 0; i < document.chains.size(); ++i)
  {
    if (!changed[i])
      continue;
    const graph::Chain& chain = resolved.chains[i];
    auto slot = std::make_unique<PlanSlot>(kArenaBytes);
    slot->plan = graph::RenderPlan::compile(chain.graph, options, slot->arena, error);
    if (slot->plan == nullptr)
    {
      error = "chain '" + chain.id + "': " + error;
      return false;
    }
    // The first process() call on a freshly built node is tens of times its steady-state cost, and a
    // stall that long can drain the device ring. It is control-thread work, before publication (§3.3).
    slot->plan->prewarm();
    slot->input_channel = chain.input_channel;
    slot->output_channel = chain.output_channel;
    slot->generation = generation;
    built.push_back(std::move(slot));
  }

  // Every plan exists, is prewarmed and is bound before any of them becomes visible. The pushes cannot
  // fail: the room for all of them was checked above and this thread is the only producer.
  std::vector<PlanSlot*> raw;
  raw.reserve(document.chains.size());
  std::size_t next_built = 0;
  for (std::size_t i = 0; i < document.chains.size(); ++i)
  {
    if (!changed[i])
    {
      raw.push_back(published_slots_[i]); // still owned by the render thread or the reclaimer
      continue;
    }
    PlanSlot* slot = built[next_built++].release();
    if (!push_slot(static_cast<uint32_t>(i), static_cast<uint32_t>(document.chains.size()), slot))
    {
      error = "the publication ring is full; the render thread is not consuming plans";
      return false;
    }
    raw.push_back(slot);
  }

  // Only now does anything about the running engine change. A failed compile, a chain whose channels
  // are not available, or a full ring leaves the previous document, plans and generation as they were.
  generation_ = generation;
  current_rig_ = path;
  current_source_ = source;
  current_base_dir_ = base_dir;
  current_name_ = document.name;
  current_document_ = document;
  published_slots_ = std::move(raw);
  last_published_ = &current_document_;

  if (config_.verbose)
  {
    const std::string what = path.empty() ? (document.name.empty() ? "(applied document)" : document.name) : path;
    std::fprintf(stderr, "[namchaind] published %s (generation %llu, %zu chain%s, %zu replanned)\n", what.c_str(),
                 static_cast<unsigned long long>(generation_), document.chains.size(),
                 document.chains.size() == 1 ? "" : "s", built.size());
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
  if (!(config_.fade_ms >= 0.0))
  {
    error = "fade_ms must be >= 0";
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

  for (int i = 0; i < kMaxChains; ++i)
  {
    for (PlanSlot** slot : {&chains_[i].current, &chains_[i].next, &chains_[i].pending})
    {
      if (*slot == nullptr)
        continue;
      delete *slot;
      *slot = nullptr;
    }
  }
  chain_count_ = 0;
  transitioning_chain_ = -1;
  transition_active_.store(false);
  audible_generation_.store(0);
  target_generation_.store(0);
  adopted_generation_.store(0);
  failed_nodes_.store(0);
  guard_trips_.store(0);
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
  backend.input_path = config_.capture_path;
  backend.output_path = config_.playback_path;

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
  // A chain may read any channel the *configuration* promised, and each chain's channel was checked
  // against that at publish time. A device that gives fewer would make those reads read past the end
  // of a frame, so it is refused rather than silently remapped (the same argument as the period size).
  if (info.capture_channels < config_.capture_channels)
  {
    error = "device gave " + std::to_string(info.capture_channels) + " capture channels, configured for " +
            std::to_string(config_.capture_channels);
    close_device();
    return false;
  }
  if (info.playback_channels < config_.playback_channels)
  {
    error = "device gave " + std::to_string(info.playback_channels) + " playback channels, configured for " +
            std::to_string(config_.playback_channels);
    close_device();
    return false;
  }

  // Sized before the stream starts, and before mlockall covers them.
  device_finished_ = false;
  capture_buffer_.assign(static_cast<std::size_t>(config_.period_size) * info.capture_bytes_per_frame, 0);
  playback_buffer_.assign(static_cast<std::size_t>(config_.period_size) * info.playback_bytes_per_frame, 0);
  bus_.assign(static_cast<std::size_t>(config_.period_size) * info.playback_channels, 0.0f);
  for (ChainRuntime& chain : chains_)
  {
    chain.in.assign(config_.period_size, 0.0f);
    chain.out.assign(config_.period_size, 0.0f);
    chain.old.assign(config_.period_size, 0.0f);
    chain.incoming.assign(config_.period_size, 0.0f);
  }

  active_rate_.store(info.sample_rate);
  active_period_.store(info.period_size);
  active_capture_channels_.store(info.capture_channels);
  active_playback_channels_.store(info.playback_channels);
  return true;
}

void Daemon::finish_device(std::string& /*error*/)
{
  if (device_ == nullptr || device_finished_)
    return;
  device_finished_ = true;
  std::string reason;
  if (!device_->finish(reason) && config_.verbose)
    std::fprintf(stderr, "[namchaind] finishing the device failed: %s\n", reason.c_str());
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

void Daemon::start_transition(int index, PlanSlot* next)
{
  ChainRuntime& chain = chains_[index];
  chain.next = next;
  unsigned rate = active_rate_.load();
  if (rate == 0)
    rate = config_.sample_rate;
  const int fade = static_cast<int>(std::llround(config_.fade_ms * static_cast<double>(rate) / 1000.0));
  chain.crossfade.start(fade);
  chain.target_generation.store(next->generation);
  // "Adopted" means the render thread has taken the plan up, which for a structural change is the
  // moment it starts fading in, not the moment the fade closes.
  chain.adopted_generation.store(next->generation);
  chain.transition_active.store(true);
  transitioning_chain_ = index;
  transitions_started_.fetch_add(1);
  update_aggregate_state();
}

void Daemon::finish_transition(int index)
{
  ChainRuntime& chain = chains_[index];
  if (chain.current != nullptr)
    reclaimer_.retire(chain.current, &delete_plan_slot);
  chain.current = chain.next;
  chain.next = nullptr;
  chain.transition_active.store(false);
  if (chain.current != nullptr)
    chain.audible_generation.store(chain.current->generation);
  if (transitioning_chain_ == index)
    transitioning_chain_ = -1;

  // A publish that arrived while this fade was in flight starts now, from the plan that has just
  // become audible. Nothing is lost and nothing nests: one structural transition at a time, so a
  // document apply works through its chains one after another rather than all at once.
  start_queued_transition();
  update_aggregate_state();
}

void Daemon::start_queued_transition()
{
  if (transitioning_chain_ >= 0)
    return;
  for (int i = 0; i < chain_count_; ++i)
  {
    ChainRuntime& chain = chains_[i];
    if (chain.pending == nullptr)
      continue;
    PlanSlot* pending = chain.pending;
    chain.pending = nullptr;
    start_transition(i, pending);
    return;
  }
  update_aggregate_state();
}

void Daemon::drop_chain(int index)
{
  ChainRuntime& chain = chains_[index];
  if (chain.current == nullptr && chain.next == nullptr && chain.pending == nullptr)
    return;

  // A chain the new document does not have stops now. Its plans are retired rather than faded: a
  // removal is not a transition from one plan to another, and fading it would mean inventing a
  // fade-to-silence path that nothing else needs (docs/ui-integration-plan.md §1.1). A client that
  // wants the removal to be gradual mutes the chain first, which is a ramp.
  if (chain.next != nullptr)
  {
    reclaimer_.retire(chain.next, &delete_plan_slot);
    chain.next = nullptr;
  }
  if (chain.pending != nullptr)
  {
    reclaimer_.retire(chain.pending, &delete_plan_slot);
    chain.pending = nullptr;
  }
  if (chain.current != nullptr)
  {
    reclaimer_.retire(chain.current, &delete_plan_slot);
    chain.current = nullptr;
  }
  chain.transition_active.store(false);
  chain.audible_generation.store(0);
  chain.target_generation.store(0);
  chain.adopted_generation.store(0);
  if (transitioning_chain_ == index)
    transitioning_chain_ = -1;
  start_queued_transition();
}

void Daemon::update_aggregate_state()
{
  // The document-level fields answer one question each: is anything still moving, and what is the
  // device actually hearing. `audible_generation` is therefore the document generation only once
  // every chain has settled at it, and the previous one while a staggered apply is still working.
  bool moving = false;
  unsigned fading = 0;
  for (int i = 0; i < chain_count_; ++i)
  {
    if (chains_[i].transition_active.load())
      ++fading;
    if (chains_[i].transition_active.load() || chains_[i].pending != nullptr)
      moving = true;
  }
  if (fading > max_concurrent_transitions_.load())
    max_concurrent_transitions_.store(fading);
  transition_active_.store(moving);
  audible_generation_.store(moving ? previous_generation_ : rendered_generation_);
  target_generation_.store(rendered_generation_);

  // "Adopted" is the document the render thread has taken up: every chain's command has been
  // processed, whether it started a fade or is waiting its turn.
  adopted_generation_.store(rendered_generation_);
}

void Daemon::drain_commands()
{
  bool published = false;
  PlanCommand command;
  while (commands_.pop(command))
  {
    if (command.kind == PlanCommand::Kind::SetParam)
    {
      // The command names the chain and the generation it was resolved against. During a transition
      // two plans are live for that chain, so the edit goes to whichever of them it names; a plan a
      // structural publish has already superseded is counted as stale rather than applied to the
      // wrong chain.
      if (command.chain >= static_cast<uint32_t>(kMaxChains))
      {
        params_stale_.fetch_add(1);
        continue;
      }
      ChainRuntime& chain = chains_[command.chain];
      graph::RenderPlan* plan = nullptr;
      if (chain.current != nullptr && chain.current->generation == command.generation)
        plan = chain.current->plan.get();
      else if (chain.next != nullptr && chain.next->generation == command.generation)
        plan = chain.next->plan.get();
      else if (chain.pending != nullptr && chain.pending->generation == command.generation)
        plan = chain.pending->plan.get();

      if (plan != nullptr && plan->set_param(command.node, command.param, command.value))
        params_applied_.fetch_add(1);
      else
        params_stale_.fetch_add(1); // an unknown param is a control-side bug; counted, not fatal
      continue;
    }

    PlanSlot* next = command.slot;
    if (next == nullptr || command.chain >= static_cast<uint32_t>(kMaxChains))
      continue;

    // The document's shape comes from the commands themselves, so the render thread never reads a
    // count the control thread could move under it mid-drain. A slot from a generation this thread has
    // not seen is a new document: remember where the last settled one was, because
    // `audible_generation` reports that until every chain has moved (see update_aggregate_state).
    if (next->generation != rendered_generation_)
    {
      previous_generation_ = rendered_generation_;
      rendered_generation_ = next->generation;
    }
    if (command.chain_count > 0 && command.chain_count <= static_cast<uint32_t>(kMaxChains))
      chain_count_ = static_cast<int>(command.chain_count);
    published = true;

    const int index = static_cast<int>(command.chain);
    ChainRuntime& chain = chains_[index];

    if (chain.current == nullptr)
    {
      // Nothing to fade from: adopt it. This is the startup rig, and the first appearance of a chain
      // added by a document -- the device has not produced a block, or the chain is new.
      chain.next = nullptr;
      chain.current = next;
      chain.audible_generation.store(next->generation);
      chain.adopted_generation.store(next->generation);
      chain.target_generation.store(next->generation);
      chain.transition_active.store(false);
      continue;
    }

    if (chain.next != nullptr || transitioning_chain_ >= 0)
    {
      // This chain is fading, or another one is and this daemon fades one chain at a time. A burst
      // coalesces to the newest request (the superseded plan is retired here) rather than nesting
      // fades, which is what rig-pipeline.md §5.3 asks for -- and staggering is what keeps the peak
      // cost of a document apply at two of the largest chain instead of two of the whole document,
      // with no admission control to lean on.
      if (chain.pending != nullptr)
        reclaimer_.retire(chain.pending, &delete_plan_slot);
      chain.pending = next;
      chain.target_generation.store(next->generation);
      continue;
    }

    start_transition(index, next);
  }

  // A document with fewer chains than the last one: the chains that are gone stop now. Done after the
  // drain rather than per command, so a document is never half-dropped while its own commands are
  // still arriving.
  if (published)
  {
    for (int i = chain_count_; i < kMaxChains; ++i)
      drop_chain(i);
  }
  update_aggregate_state();
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
      finish_device(error);
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

      const std::size_t width = info.bytes_per_sample;
      const int frames = static_cast<int>(info.period_size);
      std::fill(bus_.begin(), bus_.end(), 0.0f);

      for (int c = 0; c < chain_count_; ++c)
      {
        ChainRuntime& chain = chains_[c];
        if (chain.current == nullptr)
          continue;

        // Which channel a chain reads is the *incoming* plan's business when a transition is running:
        // a document that moved a chain to another input channel means the new plan hears the new
        // channel from the moment it starts fading in, rather than a fade between two inputs.
        const PlanSlot* source = chain.next != nullptr ? chain.next : chain.current;
        const unsigned channel = static_cast<unsigned>(source->input_channel);
        for (unsigned i = 0; i < info.period_size; ++i)
          chain.in[i] = pcm_decode(capture_buffer_.data() +
                                     (static_cast<std::size_t>(i) * info.capture_channels + channel) * width,
                                   info.format);

        if (chain.next != nullptr)
        {
          // Dual-render: both plans run on the same input, and their chain outputs are equal-power
          // crossfaded. Both keep running for the whole fade (a chain that stops mid-transition
          // freezes its state and would step on the way out).
          chain.current->plan->execute(chain.in.data(), chain.old.data(), frames);
          chain.next->plan->execute(chain.in.data(), chain.incoming.data(), frames);
          chain.crossfade.mix(chain.old.data(), chain.incoming.data(), chain.out.data(), frames);
          // mix() has finished reading old, so the outgoing plan can be retired here; the control
          // thread frees it after the reclaimer's grace window.
          if (!chain.crossfade.active())
            finish_transition(c);
        }
        else
        {
          chain.current->plan->execute(chain.in.data(), chain.out.data(), frames);
        }

        // Mute is a zeroing of the *emitted* copy, applied after execute so the chain keeps running:
        // a muted chain whose inference is skipped freezes state and clicks on unmute (G14). It is
        // also why this is a skip rather than a reason not to execute at all.
        if (chain.mute.load())
          continue;

        float* destination = bus_.data() + static_cast<std::size_t>(source->output_channel) * info.period_size;
        for (int i = 0; i < frames; ++i)
          destination[i] += chain.out[static_cast<std::size_t>(i)];
      }

      // Global mute, after every chain has run, for the same reason a per-chain mute is a skip.
      if (mute_.load())
        std::fill(bus_.begin(), bus_.end(), 0.0f);

      // One encode for the whole bus: two chains writing one channel are summed, and pcm_encode
      // clamps, so a sum that exceeds full scale saturates rather than wrapping.
      std::fill(playback_buffer_.begin(), playback_buffer_.end(), 0);
      for (unsigned i = 0; i < info.period_size; ++i)
      {
        for (unsigned c = 0; c < info.playback_channels; ++c)
        {
          pcm_encode(playback_buffer_.data() +
                       (static_cast<std::size_t>(i) * info.playback_channels + c) * width,
                     info.format, bus_[static_cast<std::size_t>(c) * info.period_size + i]);
        }
      }
    }

    // Outside the RT scope: the guard's counters are read for a report, not needed to render a block,
    // and the scope is kept to the work that has a deadline. Summed across the audible chains, because
    // "a node is failed" is a fact about the rig rather than about one chain.
    {
      unsigned failed = 0;
      uint64_t trips = 0;
      for (int c = 0; c < chain_count_; ++c)
      {
        if (chains_[c].current == nullptr)
          continue;
        failed += chains_[c].current->plan->failed_node_count();
        trips += chains_[c].current->plan->guard_trips();
        chains_[c].failed_nodes.store(static_cast<unsigned>(chains_[c].current->plan->failed_node_count()));
        chains_[c].guard_trips.store(chains_[c].current->plan->guard_trips());
      }
      failed_nodes_.store(failed);
      guard_trips_.store(trips);
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

  // Give the backend its chance to write what it accumulated (a `file` backend's output WAV) before
  // the object is destroyed. A device backend has nothing to finish, and says so by doing nothing.
  finish_device(error);
  close_device();
}

// ---------------------------------------------------------------------------
// Control loop and protocol
// ---------------------------------------------------------------------------

std::string Daemon::handle_request(const std::string& request)
{
  // One shape for the per-chain array wherever it appears, so a client can read the same entry from
  // `engine.status` and from `rig.get` and not learn two vocabularies for one thing.
  auto chains_json = [this]() {
    nlohmann::json list = nlohmann::json::array();
    for (std::size_t i = 0; i < current_document_.chains.size() && i < static_cast<std::size_t>(kMaxChains); ++i)
    {
      const graph::Chain& chain = current_document_.chains[i];
      const ChainRuntime& runtime = chains_[i];
      nlohmann::json entry;
      entry["id"] = chain.id;
      entry["name"] = chain.name;
      entry["input_channel"] = chain.input_channel;
      entry["output_channel"] = chain.output_channel;
      entry["generation"] = published_slots_[i] == nullptr ? 0 : published_slots_[i]->generation;
      entry["audible_generation"] = runtime.audible_generation.load();
      entry["adopted_generation"] = runtime.adopted_generation.load();
      entry["target_generation"] = runtime.target_generation.load();
      entry["transition_active"] = runtime.transition_active.load();
      entry["muted"] = runtime.mute.load();
      entry["failed_nodes"] = runtime.failed_nodes.load();
      entry["guard_trips"] = runtime.guard_trips.load();
      list.push_back(std::move(entry));
    }
    return list;
  };

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
    if (protocol < kOldestProtocolVersion || protocol > kProtocolVersion)
    {
      return error_response(id, "unsupported_protocol",
                            "namchaind speaks protocol " + std::to_string(kOldestProtocolVersion) + " to " +
                              std::to_string(kProtocolVersion) + ", the client asked for " +
                              std::to_string(protocol));
    }
    nlohmann::json body;
    body["protocol"] = kProtocolVersion;
    body["role"] = "namchaind";
    body["version"] = NAMCHAIN_VERSION_STRING;
    body["features"] = protocol_features();
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
    status["rig_source"] = current_source_;
    status["rig_name"] = current_name_;
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
    status["failed_nodes"] = failed_nodes_.load();
    status["guard_trips"] = guard_trips_.load();
    // Two honest labels: parameter edits are ramped inside the node (no recompile, no step); a
    // structural reload is dual-rendered and equal-power crossfaded for fade_ms.
    status["param_edits"] = "ramped";
    status["structural_transitions"] = "crossfade";
    status["transition_active"] = transition_active_.load();
    status["audible_generation"] = audible_generation_.load();
    status["target_generation"] = target_generation_.load();
    status["transitions_started"] = transitions_started_.load();
    status["max_concurrent_transitions"] = max_concurrent_transitions_.load();
    status["fade_ms"] = config_.fade_ms;

    // One entry per chain, in the document's order. This is where a client learns what its document
    // became: the chain ids it owns, the channels it was bound to, and how far each chain has got
    // through its own transition. The document-level fields above are derived from these, so a client
    // can read either the whole picture or one chain's.
    status["chains"] = chains_json();
    status["max_chains"] = kMaxChains;
    status["features"] = protocol_features();
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
    nlohmann::json body;
    if (doc.contains("chain"))
    {
      // Per-chain mute: the chain keeps running and keeps its state warm, and only its contribution
      // to the output bus is dropped (G14). Resolved here, so a chain that does not exist is an error
      // rather than a silently ignored mute.
      if (!doc["chain"].is_string())
        return error_response(id, "bad_request", "'chain' must be a string");
      const int index = find_chain(doc["chain"].get<std::string>());
      if (index < 0)
        return error_response(id, "unknown_chain", "no chain '" + doc["chain"].get<std::string>() + "'");
      chains_[static_cast<std::size_t>(index)].mute.store(muted);
      body["mute"] = muted;
      body["chain"] = doc["chain"];
      return ok_response(id, body);
    }
    mute_.store(muted);
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
  if (method == "rig.apply")
  {
    // A whole document, in memory. This is the path a UI takes: it owns the document, and a
    // structural edit is one publish rather than a sequence of graph ops it would have to get right
    // (docs/ui-integration-plan.md §1.1). The document is parsed by the same parser a rig file goes
    // through, so a document means the same thing however it arrived.
    if (!doc.contains("document") || !doc["document"].is_object())
      return error_response(id, "bad_request", "rig.apply needs an object 'document'");
    graph::Document document;
    std::string error;
    if (!graph::parse_document_json(doc["document"].dump(), document, error))
      return error_response(id, "bad_request", error);

    // Relative model/IR paths resolve against the rigs root, which is the same trust boundary a
    // rig.load name lives behind: a socket client may name a rig but may not point the daemon at an
    // arbitrary directory. Absolute paths in a document are the operator's business, and slice C
    // revisits that rule for a network client (docs/ui-integration-plan.md §3.8).
    if (!publish_document(document, /*path=*/"", "applied", config_.rigs_root, error))
      return error_response(id, "compile_failed", error);
    nlohmann::json body;
    body["name"] = document.name;
    body["generation"] = generation_;
    body["chains"] = document.chains.size();
    return ok_response(id, body);
  }
  if (method == "rig.get")
  {
    nlohmann::json body;
    body["rig"] = current_rig_;
    body["source"] = current_source_;
    body["generation"] = generation_;
    body["loaded"] = current_document_.chains.empty() ? false : true;
    if (!current_document_.chains.empty())
    {
      // The engine's own parse of what it is running, so a client can see the ids it minted and the
      // element/role of every node. A diagnostic, not a document store (§1.1).
      body["document"] = nlohmann::json::parse(graph::document_to_json(current_document_));
      body["chains"] = chains_json();
    }
    return ok_response(id, body);
  }
  if (method == "rig.patch")
  {
    if (!doc.contains("ops") || !doc["ops"].is_array())
      return error_response(id, "bad_request", "rig.patch needs an 'ops' array");
    if (current_document_.chains.empty() || published_slots_.empty())
      return error_response(id, "patch_rejected", "no plan is loaded; rig.load first");

    // Which chain this patch is against. With one chain it is obvious; with several it must be said,
    // because guessing would edit an arbitrary one of them (docs/ui-integration-plan.md §1.3).
    std::size_t chain_index = 0;
    if (doc.contains("chain"))
    {
      if (!doc["chain"].is_string())
        return error_response(id, "bad_request", "'chain' must be a string");
      const int found = find_chain(doc["chain"].get<std::string>());
      if (found < 0)
        return error_response(id, "unknown_chain", "no chain '" + doc["chain"].get<std::string>() + "'");
      chain_index = static_cast<std::size_t>(found);
    }
    else if (current_document_.chains.size() > 1)
    {
      return error_response(id, "unknown_chain",
                            "this rig has " + std::to_string(current_document_.chains.size()) +
                              " chains; rig.patch needs a 'chain'");
    }
    const std::string chain_id = current_document_.chains[chain_index].id;
    const PlanSlot* target = published_slots_[chain_index];
    if (target == nullptr || target->plan == nullptr)
      return error_response(id, "patch_rejected", "no plan is loaded; rig.load first");

    // Route by op class (§5.3). If every op is a `set_param` the live plan can dispatch, the whole
    // patch is the smooth class: a vtable dispatch on the render thread, no compile, no fade. Any
    // other op means the graph changes, so the whole patch is structural and all-or-nothing: it is
    // applied to a copy and published, which the render thread crossfades into.
    bool smooth = true;
    for (std::size_t i = 0; smooth && i < doc["ops"].size(); ++i)
    {
      const nlohmann::json& op = doc["ops"][i];
      if (!op.is_object() || !op.contains("op") || !op["op"].is_string() ||
          op["op"].get<std::string>() != "set_param")
      {
        smooth = false;
        break;
      }
      if (!op.contains("node") || !op["node"].is_string() || !op.contains("param") ||
          !op["param"].is_string() || !op.contains("value") || !op["value"].is_number())
      {
        // Malformed even for the structural path, but let apply_patch produce the precise message.
        smooth = false;
        break;
      }
      std::size_t node_index = 0;
      uint32_t param_id = 0;
      if (!target->plan->resolve_param(op["node"].get<std::string>(), op["param"].get<std::string>(),
                                       node_index, param_id))
        smooth = false;
    }

    if (smooth)
    {
      unsigned applied = 0;
      for (const nlohmann::json& op : doc["ops"])
      {
        std::string error;
        if (!publish_param(chain_id, op["node"].get<std::string>(), op["param"].get<std::string>(),
                           op["value"].get<double>(), error))
        {
          return error_response(id, "patch_rejected", "ops[" + std::to_string(applied) + "]: " + error);
        }
        ++applied;
      }
      nlohmann::json body;
      body["kind"] = "smooth";
      body["applied"] = applied;
      body["chain"] = chain_id;
      body["generation"] = generation_;
      return ok_response(id, body);
    }

    // The structural class. Only the named chain's graph changes, so only that chain is replanned and
    // crossfaded: publish_document compares each chain and leaves the others' plans alone.
    graph::Document patched = current_document_;
    graph::PatchContext context;
    // A model path in a patch is relative to the rig being patched, the same rule a rig's own model
    // paths follow, so the two agree about where models live.
    context.base_dir = current_base_dir_;
    std::string error;
    if (!graph::apply_patch(patched.chains[chain_index].graph, doc["ops"].dump(), context, error))
      return error_response(id, "patch_rejected", error);
    if (!publish_document(patched, current_rig_, current_source_, current_base_dir_, error))
      return error_response(id, "compile_failed", error);

    nlohmann::json body;
    body["kind"] = "structural";
    body["applied"] = static_cast<unsigned>(doc["ops"].size());
    body["chain"] = chain_id;
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
