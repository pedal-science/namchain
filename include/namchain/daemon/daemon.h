#pragma once

// namchaind: the reference daemon (plan.md §8.2), which is also the control thread sketched in
// docs/rig-pipeline.md §5.1/§5.3.
//
// The ownership split is the point of the design, so it is visible in the type:
//   * the render thread owns the live PlanSlot pointer, the backend and the stream buffers;
//   * the control thread owns compilation, publication, the socket server and reclamation;
//   * the only things that cross are POD commands on a lock-free ring (control -> render) and the
//     reclaimer's retire FIFO (render -> control).
#include <signal.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "namchain/daemon/config.h"
#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"
#include "namchain/io/backend.h"
#include "namchain/rt/arena.h"
#include "namchain/rt/crossfade.h"
#include "namchain/rt/reclaimer.h"
#include "namchain/rt/spsc_ring.h"

namespace namchain
{
namespace daemon
{

/// Where the render thread thinks the device is. Reported rather than inferred, because "the daemon
/// is up" and "the interface is up" are different claims and an appliance has to be able to tell
/// them apart.
enum class DeviceState
{
  Closed,  ///< No device: never opened, or closed after a failure and waiting to retry.
  Running, ///< Open and streaming.
  Lost,    ///< Was running and failed, or could not be opened. The render thread is retrying.
};

const char* device_state_name(DeviceState state);

/// \brief One published chain plan and the arena its buffers live in.
///
/// Owns both, so retiring a slot frees the plan and every intermediate buffer together. The control
/// thread builds it and pushes a pointer; the render thread swaps it in; the control thread frees it
/// once the reclaimer's grace window has closed (rig-pipeline.md §6.4). A plan is never freed on the
/// render thread and never published before it is complete.
///
/// One slot is one **chain**, not one document: a document with two chains publishes two slots, in the
/// same generation, and each fades into its own independently
/// (docs/ui-integration-plan.md §1.1).
struct PlanSlot
{
  explicit PlanSlot(std::size_t arena_bytes) : arena(arena_bytes) {}

  rt::Arena arena;
  std::unique_ptr<graph::RenderPlan> plan;
  /// The device channels this chain reads and writes. Carried in the slot rather than in a
  /// control-side table because the render thread needs them every block and may not read anything
  /// that the control thread can move underneath it.
  int input_channel = 0;
  int output_channel = 0;
  uint64_t generation = 0;
};

/// \brief The most chains one document may hold.
///
/// A fixed bound rather than a vector, because the render thread's per-chain state is a fixed-sized
/// array: nothing on the render path may allocate or resize, and rebuilding that state on a publish
/// would be exactly that. The limit is published to clients as part of the engine's limits.
inline constexpr int kMaxChains = 8;

/// Control thread -> render thread: the identity of prepared work that is already finished.
///
/// One tagged record rather than one ring per op: the render thread drains it in order at the top of
/// the block, and `kind` is what selects the work. `PublishPlan` carries a finished plan for one
/// chain; `SetParam` carries the vtable dispatch of §5.3's `smooth` class, which is why it needs no
/// plan compile and no fade.
struct PlanCommand
{
  enum class Kind : uint32_t
  {
    PublishPlan,
    SetParam,
  };

  Kind kind = Kind::PublishPlan;
  PlanSlot* slot = nullptr; ///< PublishPlan: one chain's prepared plan
  /// PublishPlan: which chain this plan belongs to, and how many chains the document it came from
  /// holds. Every command of one document carries the same count, so the render thread learns the
  /// document's shape from the commands themselves and never from state the control thread mutates.
  uint32_t chain = 0;
  uint32_t chain_count = 0;
  uint64_t generation = 0; ///< SetParam: the plan generation it targets
  uint32_t node = 0;       ///< SetParam: index into the plan's execution order
  uint32_t param = 0;      ///< SetParam: the node's param id
  float value = 0.0f;      ///< SetParam
};

/// The longest structural crossfade the weight table covers; a larger `fade_ms` is clamped to it.
/// 16384 frames is 341 ms at 48 kHz and 43 ms at 384 kHz, well past §5.3's 5 ms default.
inline constexpr int kMaxTransitionFrames = 1 << 14;

/// \brief The reference daemon engine.
///
/// What this is: a backend, a compiled plan per chain, a render thread, a Unix SOCK_SEQPACKET control
/// socket speaking the versioned JSON protocol, SIGHUP reload and device-loss survival, the four edit
/// classes of §5.3 (a ramped parameter, a structural crossfade, bypass and the non-finite guard), and
/// -- since docs/ui-integration-plan.md slice A2 -- documents of up to \ref kMaxChains chains, each
/// compiled, bound to its own device channels and transitioned independently, with a guitar chain and
/// a vocal chain in one rig the case that forced it.
///
/// What it deliberately is not yet (docs/ui-integration-plan.md): no admission control, so a
/// structural change that is too expensive to dual-render is not rejected before it runs and the fade
/// length is fixed rather than shortened under load -- which is why a document apply transitions its
/// chains **one at a time**. No tail-hold for a bypassed node with a tail. No stereo: the schema
/// declares one channel per port and a client asking for more is refused by name.
class Daemon
{
public:
  explicit Daemon(Config config);
  ~Daemon();

  Daemon(const Daemon&) = delete;
  Daemon& operator=(const Daemon&) = delete;

  /// \brief Build the startup rig, bind the socket, open the signal set and start the render thread.
  ///
  /// Fatal only for things an operator must fix before the daemon can mean anything: an unusable
  /// startup rig, a socket path that will not bind, a signal set that cannot be created. A missing
  /// audio device is **not** fatal -- that is the unplugged case, and the daemon serves status and
  /// retries while it waits.
  bool start(std::string& error);

  /// \brief The control loop. Blocks until SIGTERM/SIGINT or \ref request_stop, then tears down.
  /// \return the process exit code (0 for an ordinary stop).
  int run();

  /// Ask the loop to finish, from any thread. The render thread is stopped by `run()` on the way out.
  void request_stop();

  /// \brief Handle one protocol request, returning one response.
  ///
  /// Public because it is the unit-testable half of the transport: the socket code only has to move
  /// whole SEQPACKET messages. Never throws; errors come back as a structured JSON response.
  std::string handle_request(const std::string& request);

  /// The device state, for tests and for a caller that wants the same number the status reports.
  DeviceState device_state() const { return static_cast<DeviceState>(device_state_.load()); }

  /// The chains the running document holds, in the order their channels were bound. Control thread.
  const std::vector<graph::Chain>& chains() const { return current_document_.chains; }

private:
  // --- control thread only -------------------------------------------------
  bool bind_socket(std::string& error);
  void close_socket();
  bool setup_signals(std::string& error);
  /// Compile \p rig_path and publish it. Called for the startup rig, SIGHUP and `rig.load`.
  bool load_rig(const std::string& rig_path, std::string& error);
  /// \brief Compile every chain of \p document, prewarm the plans, and publish them together.
  ///
  /// Control thread. All-or-nothing: if any chain fails to compile, or its channels are not available,
  /// or the command ring cannot take every chain's command, nothing is published and the running
  /// document keeps rendering. On success \ref current_document_ becomes \p document, the generation
  /// advances, and the render thread crossfades each chain into its new plan -- one chain at a time,
  /// so the peak cost is two of the largest chain rather than two of the document.
  /// \param path the file the document came from, or empty when it was applied over the socket.
  /// \param source `"file"` or `"applied"`, reported so a client can tell a rig it can re-read from
  /// disk from one that exists only in this process.
  /// \param base_dir the directory a relative model/IR path resolves against: the rig file's own
  /// directory for a load, the rigs root for an applied document.
  bool publish_document(const graph::Document& document, const std::string& path, const std::string& source,
                        const std::string& base_dir, std::string& error);
  /// \brief Resolve a `set_param` against a chain's newest published plan and push it on the ring.
  ///
  /// \param chain_id the chain to resolve against, or empty to resolve by node id -- which is an error
  /// if more than one chain holds that node, because guessing would edit an arbitrary chain.
  bool publish_param(const std::string& chain_id, const std::string& node, const std::string& param, double value,
                     std::string& error);
  bool push_slot(uint32_t chain, uint32_t chain_count, PlanSlot* slot);
  /// The index of the chain with this id in the running document, or -1. Control thread.
  int find_chain(const std::string& chain_id) const;
  /// The index of the one chain whose newest plan holds \p node, or -1 for none and -2 for several.
  int find_chain_with_node(const std::string& node) const;
  void set_device_state(DeviceState state);
  void stop();

  // --- render thread only --------------------------------------------------
  void render_loop();
  bool open_device(std::string& error);
  /// Give the backend its chance to flush what it accumulated, before the object is destroyed: a
  /// `file` backend writes its output WAV here, and a device backend has nothing to do.
  void finish_device(std::string& error);
  void close_device();
  void fail_device(const std::string& reason);
  void drain_commands();
  /// Begin an equal-power transition for one chain. Render thread.
  void start_transition(int index, PlanSlot* next);
  /// Retire the faded-out plan, promote the incoming one, and start whatever is queued. Render thread.
  void finish_transition(int index);
  /// Start the next queued chain transition, if one is waiting and none is running. Render thread.
  void start_queued_transition();
  /// Retire a chain's plans and forget it: a document with fewer chains than the last one. Render
  /// thread.
  void drop_chain(int index);
  /// Recompute the document-level generation fields from the per-chain ones. Render thread.
  void update_aggregate_state();

  /// \brief One chain's render state.
  ///
  /// A plain member array of these is the whole reason for \ref kMaxChains: the render thread must
  /// never allocate, and a per-chain container that could grow would be exactly that. Everything the
  /// control thread reads for status is an atomic; everything else is render-thread-owned.
  struct ChainRuntime
  {
    PlanSlot* current = nullptr; ///< the audible plan (fading out during a transition)
    PlanSlot* next = nullptr;    ///< the plan fading in, or null
    /// A publish that arrived for this chain while it was waiting its turn. One slot, newest wins:
    /// a burst coalesces to the last request and the superseded plan is retired.
    PlanSlot* pending = nullptr;
    rt::Crossfade crossfade{kMaxTransitionFrames};
    std::vector<float> in;
    std::vector<float> out;
    std::vector<float> old;
    std::vector<float> incoming;

    std::atomic<bool> mute{false};
    std::atomic<bool> transition_active{false};
    std::atomic<uint64_t> audible_generation{0};
    std::atomic<uint64_t> target_generation{0};
    std::atomic<uint64_t> adopted_generation{0};
    std::atomic<unsigned> failed_nodes{0};
    std::atomic<uint64_t> guard_trips{0};
  };

  Config config_;
  // Sized once; the render path only pushes/pops. Big enough for a whole document plus a burst of
  // parameter edits, because a document publish is all-or-nothing: if every chain's command does not
  // fit, none of them is pushed.
  rt::SpscRing<PlanCommand> commands_{kMaxChains + 8};
  // `fade + 2` blocks of grace. There is no fade yet, so two epochs; capacity is a publish storm's
  // worth, and a full queue is counted rather than silently growing.
  rt::Reclaimer reclaimer_{2, 16};

  int listen_fd_ = -1;
  int signal_fd_ = -1;
  /// The newest published document. Control-thread-only, and never dangling: a slot is retired only
  /// when a *newer* document is adopted, and this pointer has already moved to that newer one by then.
  const graph::Document* last_published_ = nullptr;
  /// The document the newest published plans were built from. Control-thread-only; it is the base a
  /// structural `rig.patch` mutates, and it is replaced only by a successful publish.
  graph::Document current_document_;
  std::vector<PlanSlot*> published_slots_; ///< parallel to current_document_.chains
  /// The signal mask in force before \ref setup_signals, restored on stop so a library that started
  /// this daemon does not inherit a blocked SIGTERM for the rest of the process's life.
  sigset_t previous_mask_{};
  bool mask_saved_ = false;
  std::vector<int> clients_;
  std::string current_rig_;
  /// How the running document arrived: `"file"`, `"applied"`, or empty before anything is loaded.
  std::string current_source_;
  /// The document's own name, which is all an applied document has.
  std::string current_name_;
  /// What a relative path in the document (or in a patch against it) resolves against.
  std::string current_base_dir_;
  uint64_t generation_ = 0;
  bool started_ = false;

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> mute_{false};
  std::thread render_thread_;

  // Render-thread-owned state. Only the render thread touches these.
  std::unique_ptr<AudioBackend> device_;
  bool device_started_ = false;
  /// Whether \ref finish_device has already run for this open. A `file` backend writes its WAV there,
  /// and the render loop can leave by two paths (input exhausted, or a stop), so "finish once, before
  /// the backend is destroyed" is stated rather than implied.
  bool device_finished_ = false;
  /// One entry per chain of the running document, and how many of them there are. The control thread
  /// learns the count from its own copy; the render thread learns it from the publish commands.
  ChainRuntime chains_[kMaxChains];
  int chain_count_ = 0;
  /// The chain whose crossfade is in flight, or -1. One at a time, which is what bounds the peak cost
  /// of a document apply to two of the largest chain rather than two of the whole document.
  int transitioning_chain_ = -1;
  /// The document generation the render thread has been told about, and the one before it. The pair
  /// is what lets `audible_generation` mean "the generation the device is hearing" while an apply is
  /// still working through the chains.
  uint64_t rendered_generation_ = 0;
  uint64_t previous_generation_ = 0;
  std::vector<unsigned char> capture_buffer_;
  std::vector<unsigned char> playback_buffer_;
  /// The playback bus, planar: one run of \ref Config::period_size floats per device playback channel.
  /// Every chain adds its output into its own channel's plane and the whole bus is encoded once, which
  /// is what makes two chains writing one channel sum instead of overwrite.
  std::vector<float> bus_;

  std::atomic<int> device_state_{static_cast<int>(DeviceState::Closed)};
  std::atomic<uint64_t> blocks_{0};
  std::atomic<uint64_t> xruns_{0};
  std::atomic<uint64_t> device_lost_{0};
  std::atomic<uint64_t> device_restored_{0};
  std::atomic<uint64_t> render_errors_{0};
  std::atomic<uint64_t> adopted_generation_{0};
  /// Document-level transition state, for engine.status. Derived from the per-chain fields rather
  /// than tracked separately: `audible_generation` is the *oldest* generation any chain is still
  /// playing, so it stays behind while a staggered apply works through the chains, and
  /// `transition_active` is true while any chain is fading or waiting to.
  std::atomic<bool> transition_active_{false};
  std::atomic<uint64_t> audible_generation_{0};
  std::atomic<uint64_t> target_generation_{0};
  std::atomic<uint64_t> transitions_started_{0};
  /// The most chains that were ever mid-crossfade at the same moment. "One transition at a time" is
  /// the rule that keeps a document apply's peak cost at two of the largest chain rather than two of
  /// the whole document, and this is that rule as a number a client can read instead of a promise:
  /// 1 means it never overlapped, which is what a staggered apply should show.
  std::atomic<unsigned> max_concurrent_transitions_{0};
  /// Parameter edits that reached the render thread and were applied; and those dropped because the
  /// plan they named had already been superseded.
  std::atomic<uint64_t> params_applied_{0};
  std::atomic<uint64_t> params_stale_{0};
  /// The non-finite guard across the audible chains: nodes failed, and how many times one tripped.
  std::atomic<unsigned> failed_nodes_{0};
  std::atomic<uint64_t> guard_trips_{0};
  std::atomic<unsigned> active_rate_{0};
  std::atomic<unsigned> active_period_{0};
  std::atomic<unsigned> active_capture_channels_{0};
  std::atomic<unsigned> active_playback_channels_{0};
  std::atomic<bool> memory_locked_{false};
  std::atomic<int> rt_policy_{0};
  std::atomic<int> rt_priority_{0};
  std::atomic<bool> rt_realtime_{false};
};

} // namespace daemon
} // namespace namchain
