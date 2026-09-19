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

/// \brief One published plan and the arena its buffers live in.
///
/// Owns both, so retiring a slot frees the plan and every intermediate buffer together. The control
/// thread builds it and pushes a pointer; the render thread swaps it in; the control thread frees it
/// once the reclaimer's grace window has closed (rig-pipeline.md §6.4). A plan is never freed on the
/// render thread and never published before it is complete.
struct PlanSlot
{
  explicit PlanSlot(std::size_t arena_bytes) : arena(arena_bytes) {}

  rt::Arena arena;
  std::unique_ptr<graph::RenderPlan> plan;
  std::string rig;
  uint64_t generation = 0;
};

/// Control thread -> render thread: the identity of prepared work that is already finished.
///
/// One tagged record rather than one ring per op: the render thread drains it in order at the top of
/// the block, and `kind` is what selects the work. `PublishPlan` carries a finished plan; `SetParam`
/// carries the vtable dispatch of §5.3's `smooth` class, which is why it needs no plan compile and
/// no fade.
struct PlanCommand
{
  enum class Kind : uint32_t
  {
    PublishPlan,
    SetParam,
  };

  Kind kind = Kind::PublishPlan;
  PlanSlot* slot = nullptr; ///< PublishPlan
  uint64_t generation = 0;  ///< SetParam: the plan generation it targets
  uint32_t node = 0;        ///< SetParam: index into the plan's execution order
  uint32_t param = 0;       ///< SetParam: the node's param id
  float value = 0.0f;       ///< SetParam
};

/// The longest structural crossfade the weight table covers; a larger `fade_ms` is clamped to it.
/// 16384 frames is 341 ms at 48 kHz and 43 ms at 384 kHz, well past §5.3's 5 ms default.
inline constexpr int kMaxTransitionFrames = 1 << 14;

/// \brief The reference daemon engine.
///
/// What this skeleton is: a backend, a compiled plan, a render thread, a Unix SOCK_SEQPACKET control
/// socket speaking the versioned JSON protocol, SIGHUP reload and device-loss survival.
///
/// What it deliberately is not yet (all M2, docs/rig-pipeline.md §7): the transition machinery. A
/// structural reload here is a cold swap at a block boundary -- the new plan is complete before it
/// is published, and the old one is retired for `grace + 2` blocks, but there is no dual-render
/// crossfade, no parameter ramp and no admission control. So a reload can click, and the status
/// response says `"transitions": "none"` rather than implying a fade happened.
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

private:
  // --- control thread only -------------------------------------------------
  bool bind_socket(std::string& error);
  void close_socket();
  bool setup_signals(std::string& error);
  /// Compile \p rig_path and publish it. Called for the startup rig, SIGHUP and `rig.load`.
  bool load_rig(const std::string& rig_path, std::string& error);
  /// \brief Compile \p graph, prewarm it and publish it, keeping it as the graph later patches apply to.
  ///
  /// Control thread. On success \ref current_graph_ becomes \p graph, the generation advances and the
  /// render thread crossfades into the new plan; on any failure nothing changes and the running plan
  /// keeps rendering.
  bool publish_graph(const graph::Graph& graph, const std::string& label, std::string& error);
  /// Resolve a `set_param` against the newest published plan and push it on the ring.
  bool publish_param(const std::string& node, const std::string& param, double value, std::string& error);
  bool push_slot(PlanSlot* slot);
  void set_device_state(DeviceState state);
  void stop();

  // --- render thread only --------------------------------------------------
  void render_loop();
  bool open_device(std::string& error);
  void close_device();
  void fail_device(const std::string& reason);
  void drain_commands();
  /// Begin an equal-power transition from \ref current_slot_ to \p next. Render thread.
  void start_transition(PlanSlot* next);
  /// Retire the faded-out plan and promote the incoming one. Render thread.
  void finish_transition();

  Config config_;
  // Sized once; the render path only pushes/pops.
  rt::SpscRing<PlanCommand> commands_{8};
  // `fade + 2` blocks of grace. There is no fade yet, so two epochs; capacity is a publish storm's
  // worth, and a full queue is counted rather than silently growing.
  rt::Reclaimer reclaimer_{2, 16};

  int listen_fd_ = -1;
  int signal_fd_ = -1;
  /// The newest published plan. Control-thread-only, and never dangling: a slot is retired only when
  /// a *newer* plan is adopted, and this pointer has already moved to that newer plan by then.
  PlanSlot* last_published_ = nullptr;
  /// The signal mask in force before \ref setup_signals, restored on stop so a library that started
  /// this daemon does not inherit a blocked SIGTERM for the rest of the process's life.
  sigset_t previous_mask_{};
  bool mask_saved_ = false;
  std::vector<int> clients_;
  std::string current_rig_;
  /// The graph the newest published plan was built from. Control-thread-only; it is the base a
  /// structural `rig.patch` mutates, and it is replaced only by a successful publish.
  graph::Graph current_graph_;
  uint64_t generation_ = 0;
  bool started_ = false;

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> mute_{false};
  std::thread render_thread_;

  // Render-thread-owned state. Only the render thread touches these.
  std::unique_ptr<AudioBackend> device_;
  bool device_started_ = false;
  PlanSlot* current_slot_ = nullptr; ///< the audible plan (fading out during a transition)
  PlanSlot* next_slot_ = nullptr;    ///< the plan fading in, or null
  /// A structural publish that arrived while a transition was in flight. One slot, newest wins: a
  /// burst of rig.load coalesces to the last request, and the superseded plan is retired.
  PlanSlot* pending_slot_ = nullptr;
  /// Equal-power crossfade for the transition. Sized once, on the control thread.
  rt::Crossfade crossfade_{kMaxTransitionFrames};
  std::vector<unsigned char> capture_buffer_;
  std::vector<unsigned char> playback_buffer_;
  std::vector<float> mono_in_;
  std::vector<float> mono_out_;
  /// The two chain outputs during a transition, crossfaded into \ref mono_out_.
  std::vector<float> mono_old_;
  std::vector<float> mono_new_;

  std::atomic<int> device_state_{static_cast<int>(DeviceState::Closed)};
  std::atomic<uint64_t> blocks_{0};
  std::atomic<uint64_t> xruns_{0};
  std::atomic<uint64_t> device_lost_{0};
  std::atomic<uint64_t> device_restored_{0};
  std::atomic<uint64_t> render_errors_{0};
  std::atomic<uint64_t> adopted_generation_{0};
  /// Structural transition state, for engine.status. `audible_generation` is what the device is
  /// hearing now; `target_generation` is what it is fading to while `transition_active`.
  std::atomic<bool> transition_active_{false};
  std::atomic<uint64_t> audible_generation_{0};
  std::atomic<uint64_t> target_generation_{0};
  std::atomic<uint64_t> transitions_started_{0};
  /// Parameter edits that reached the render thread and were applied; and those dropped because the
  /// plan they named had already been superseded.
  std::atomic<uint64_t> params_applied_{0};
  std::atomic<uint64_t> params_stale_{0};
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
