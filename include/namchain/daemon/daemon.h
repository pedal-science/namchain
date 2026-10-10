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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "namchain/assets/registry.h"
#include "namchain/daemon/config.h"
#include "namchain/daemon/catch_up.h"
#include "namchain/daemon/doc_runtime.h"
#include "namchain/daemon/framing.h"
#include "namchain/daemon/cost_table.h"
#include "namchain/daemon/input_history.h"
#include "namchain/daemon/recorder.h"
#include "namchain/daemon/render_pool.h"
#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"
#include "namchain/io/backend.h"
#include "namchain/level.h"
#include "namchain/rt/arena.h"

#include <json.hpp>

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

/// Which transport a request arrived on (docs/ui-integration-plan.md §1.8).
///
/// The two have different trust levels and that is the whole reason the type exists: a Unix socket is
/// authorised by filesystem permissions, a TCP connection is authorised by nothing until it presents
/// the transport token.
enum class PeerKind
{
  Local,  ///< Unix `SOCK_SEQPACKET`: the local operator.
  Remote, ///< TCP: untrusted until authenticated.
};

/// \brief One connection's protocol state, owned by the transport and handed to the request handler.
///
/// Authorisation is a property of the *connection*, not of the request: a TCP client that has not sent
/// `engine.hello` with the right token may not call anything else, however well-formed the call is.
/// Keeping it here rather than in the handler is what makes that a rule the transport can enforce
/// (it closes the connection) instead of a check the handler has to remember.
struct Session
{
  PeerKind peer = PeerKind::Local;
  /// True on a local connection from the moment it is accepted; false on a remote one until
  /// `engine.hello` carries the token.
  bool authorized = true;
  /// Failed authentications on this connection. The transport closes it at \ref kMaxAuthFailures.
  unsigned auth_failures = 0;
};

/// Failed authentications one connection is allowed before the daemon closes it.
///
/// Three is enough for a client with a stale token to be told clearly, and small enough that guessing
/// gets three tries per connection rather than a stream of them.
inline constexpr unsigned kMaxAuthFailures = 3;

/// Open Unix control connections allowed at once. Separate from \ref Config::tcp_max_connections
/// because the local transport's cost is a file descriptor and the remote one's is a network peer.
inline constexpr std::size_t kMaxLocalClients = 16;

/// The `SO_SNDBUF` given to an accepted TCP connection.
///
/// A control response is small, so a modest send buffer costs nothing and buys two things: the
/// kernel's per-connection memory is bounded, and a client that stops reading produces a partial write
/// the control loop walks away from promptly instead of after megabytes of buffered reply. It is also
/// what makes "a slow reader does not stall the control loop" a test that finishes in milliseconds.
inline constexpr int kTcpSendBufferBytes = 64 * 1024;

/// How long a connection that is being closed may take to drain its last reply before it is dropped
/// anyway. A client that will not read its own refusal does not get to keep the slot.
inline constexpr unsigned kFlushDeadlineMs = 5000;

/// How often the recent-trouble buckets are drained, in blocks. A hundred blocks is a sixth of a
/// second at 64 frames and 48 kHz, so a bucket halves in about a second: long enough that a dropout
/// is still visible when the user looks, short enough that the light goes back to green.
inline constexpr uint64_t kRecentDecayBlocks = 100;

/// \brief One step of the recent-trouble decay: a bucket that reaches zero in bounded time.
///
/// The first version decayed by `n - n/8`, which with integer division sticks at 7 for ever -- so
/// after a single dropout the health light stayed amber for the rest of the session, which is exactly
/// what the bucket was introduced to avoid (docs/ui-integration-plan.md §1.6). Subtracting at least
/// one per step makes the guarantee the bucket is for actually true: every count reaches zero, and a
/// count that started at n takes about `log(n) * 8` steps to get there.
inline uint64_t decay_recent_trouble(uint64_t value)
{
  return value == 0 ? 0 : value - (value / 8 > 1 ? value / 8 : 1);
}

/// \brief Who may run a plan that is being caught up (P8).
///
/// A plan that has not been rendering has no state, and the catch-up thread replays recent input into
/// it to build one. That is the one time a plan is executed by a thread other than the one that will
/// render it, so who is executing it has to be a fact both threads agree on rather than an assumption:
/// the catch-up thread claims this before each block it replays and releases it after, and the render
/// thread claims it before the crossfade that makes the plan audible. Exactly one of them can win, so
/// a block is never run twice and the two never touch the plan at once.
enum class CatchUpOwner : uint8_t
{
  Free = 0,    ///< nobody is in it: either thread may claim
  CatchUp = 1, ///< the catch-up thread is replaying or keeping it current
  Render = 2,  ///< the render thread has taken it over for the crossfade
};

/// Claim a catch-up plan for \p who. \return false when the other thread has it.
inline bool claim_catch_up(std::atomic<CatchUpOwner>& owner, CatchUpOwner who) noexcept
{
  CatchUpOwner expected = CatchUpOwner::Free;
  return owner.compare_exchange_strong(expected, who, std::memory_order_acq_rel, std::memory_order_acquire);
}

/// Let go of a catch-up plan, after the block being run has finished.
inline void release_catch_up(std::atomic<CatchUpOwner>& owner) noexcept
{
  owner.store(CatchUpOwner::Free, std::memory_order_release);
}

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
  ~PlanSlot() { delete then; }

  PlanSlot(const PlanSlot&) = delete;
  PlanSlot& operator=(const PlanSlot&) = delete;

  rt::Arena arena;
  std::unique_ptr<graph::RenderPlan> plan;
  /// The device channels this chain reads and writes. Carried in the slot rather than in a
  /// control-side table because the render thread needs them every block and may not read anything
  /// that the control thread can move underneath it.
  int input_channel = 0;
  int output_channel = 0;
  uint64_t generation = 0;

  /// \brief Taken over from \ref base without a whole-chain crossfade (docs/transitions.md, "Local
  /// transitions"): this plan shares the instances it kept with that one, so the switch is at a block
  /// boundary and nothing runs twice.
  bool local = false;
  /// The slot this one was planned against, and the only one it may take over from. Compared, never
  /// dereferenced: by the time it could be, it may have been reclaimed.
  const PlanSlot* base = nullptr;
  /// A local transition's final plan, which takes over once this one has run \ref fade_frames frames;
  /// null when this slot is itself the final plan. Owned: retiring a transitional slot that never ran
  /// frees its final plan too, and the render thread detaches it before it takes over.
  PlanSlot* then = nullptr;
  int fade_frames = 0;
  /// Buffers copied when this slot takes over from \ref base, and when \ref then takes over from it:
  /// (source, destination), one period each. A block-delay node's state is the input buffer its plan
  /// last wrote, not anything in the node, so this is what carries a feedback loop across.
  std::vector<std::pair<const float*, float*>> on_adopt;
  std::vector<std::pair<const float*, float*>> on_then;

  // --- P8's catch-up (docs/daemon.md, "Lite-first publishes") ---------------------------------
  /// \brief This plan is being caught up: it must not be executed by the render thread until the
  /// catch-up has made its state current.
  ///
  /// Set by the control thread on the Full of a `lite_first` publish, cleared by the render thread
  /// when it takes the plan over. A plan marked this way is held in `ChainRuntime::pending` until
  /// \ref Daemon::advance_catch_up decides its state is exactly right, or gives up waiting.
  std::atomic<bool> catch_up{false};
  /// \brief The sequence of the last block *fed* into this plan -- replayed or mirrored -- which is
  /// what the render thread decides the hand-over on: it takes the plan over when this has reached the
  /// block it just rendered, so the plan's state is the state a plan that had been running would have,
  /// and it processes every block exactly once and in order.
  ///
  /// \ref catch_up_seq is the same number, but it is *not* what the decision reads. The two are written
  /// by the catch-up thread either side of executing the plan, so a reader that compared the pair could
  /// see a sequence whose block has not been executed yet; reading this one alone can only ever be one
  /// block *behind* the truth, which delays a hand-over and never advances one (2026-10-10: comparing
  /// the executed sequence cost the pedal every two-step publish, because it needed the hand-over to
  /// win a race inside one 1.33 ms block, and on an unpaced producer the plan is always behind).
  std::atomic<uint64_t> catch_up_ready{0};
  /// The input block sequence the plan's state is "as of": the last block replayed or mirrored into
  /// it. Written after the block has been executed, so this is the state, not the intent; the
  /// hand-over reads \ref catch_up_ready rather than this.
  std::atomic<uint64_t> catch_up_seq{0};
  /// The catch-up could not be made exact (the render thread ran out of patience, or the history had
  /// a hole): take the plan over as soon as the catch-up thread is out of it, and say so.
  std::atomic<bool> catch_up_abandoned{false};
  /// Who is executing it right now (\ref CatchUpOwner).
  std::atomic<CatchUpOwner> catch_up_owner{CatchUpOwner::Free};
  /// Blocks the catch-up thread has replayed or mirrored into this plan: what the replay cost and
  /// what a report can show.
  std::atomic<uint64_t> catch_up_blocks{0};
  /// \brief How long the render thread will hold this plan in `pending` before it takes it over
  /// anyway, in frames. Set by the control thread when the catch-up job is handed over, and read by
  /// the render thread every block: a plain `int` because it is written before the plan is published
  /// and never again, so the publication is the barrier (the same reason \ref input_channel is).
  ///
  /// This is the chain's own memory plus \ref kCatchUpHoldMarginFrames, floored at
  /// \ref kCatchUpHoldFrames: a window that covers a model's receptive field is filled at the speed
  /// of the audio clock, so a fixed hold shorter than the field expires before there is anything to
  /// replay (2026-10-10, and the reason every pedal `lite_first` press reported `no_input`).
  int catch_up_hold_frames = 0;
};

/// The widest device whose levels are measured, per direction. Eight is the pedal's stereo interface
/// with room to spare, and it is a bound rather than a guess: the render thread accumulates into a
/// fixed array, because the audio thread allocates nothing (namchain/rt_check.h).
inline constexpr std::size_t kMaxLevelChannels = 8;

/// Control thread -> render thread: the identity of prepared work that is already finished.
///
/// One tagged record rather than one ring per op: the render thread drains it in order at the top of
/// the block, and `kind` is what selects the work. `PublishPlan` carries a finished plan for one
/// chain; `SetParam` carries the vtable dispatch of §5.3's `smooth` class, which is why it needs no
/// plan compile and no fade; `SetBypass` is a bypass flipped the same way, as a crossfade inside the
/// running plan.
struct PlanCommand
{
  enum class Kind : uint32_t
  {
    PublishPlan,
    SetParam,
    SetBypass,
  };

  Kind kind = Kind::PublishPlan;
  /// PublishPlan: one chain's prepared plan. SetParam: the plan the edit was resolved against, which
  /// the render thread compares (with \ref generation) and never dereferences.
  PlanSlot* slot = nullptr;
  /// PublishPlan: which chain this plan belongs to, and how many chains the document it came from
  /// holds. Every command of one document carries the same count, so the render thread learns the
  /// document's shape from the commands themselves and never from state the control thread mutates.
  uint32_t chain = 0;
  uint32_t chain_count = 0;
  uint64_t generation = 0; ///< SetParam, SetBypass: the plan generation it targets
  uint32_t node = 0;       ///< SetParam, SetBypass: index into the plan's execution order
  uint32_t param = 0;      ///< SetParam: the node's param id. SetBypass: the crossfade's length in frames
  float value = 0.0f;      ///< SetParam. SetBypass: 1 to bypass, 0 to bring the node back
};

/// \brief One compiled document, handed from the control thread to a warm thread.
///
/// A mailbox record rather than a command: it is only ever "here is content for document N", because
/// everything else a warm thread does -- letting a document go, taking the outgoing one over after a
/// switch -- is a state it reads, not a message it is sent (docs/warm-set.md).
struct WarmContent
{
  PlanSlot* slots[kMaxChains] = {};
  uint32_t chain_count = 0;
  /// The control thread's id for this content: what a switch request names, and what
  /// `DocRuntime::content` holds once it is installed.
  uint64_t generation = 0;
  /// The document the control thread would like it in. Checked again when it is installed, because a
  /// switch can move a document in between.
  uint32_t doc = 0;
};

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
///
/// Since slice C there is a second transport. Both carry the same JSON bodies; they differ in framing
/// (a length prefix, because TCP is a stream) and in trust (a token, because TCP has no filesystem
/// permissions to offer). See docs/daemon.md §"Two transports" and its threat model.
/// The buckets of the render load histogram: one per percent of the deadline, the last one being
/// "over it".
inline constexpr std::size_t kLoadBuckets = 101;

/// \brief Whether a window of the render load says the warm set is costing the audible rig more than
/// D3's charge predicted (2026-10-09).
///
/// The charge is a measurement, and a measurement of one board in one arrangement; this is what
/// catches the cases it gets wrong. `before` and `now` are two readings of the render load histogram
/// (\ref Daemon::load_histogram_), one bucket per percent of the deadline, and the window between
/// them is the blocks rendered since the last check -- so a spike an hour ago cannot keep the warm set
/// cooled now. True when any block in that window went over the deadline -- D7's bar, and the fault the
/// failsafe exists for -- or when a twentieth of them ran past 95% of it, which is a tail pressed
/// against the edge rather than a busy pedal (measured 2026-10-10: a 0.559 rig with the housekeeping
/// core at 100% spends a *few percent* of its blocks above 90% and none over the deadline, and an
/// earlier rule that cooled on a hundredth above 90% cooled a warm document every ten seconds for the
/// whole hour).
///
/// A free function of the two histograms so it can be read and tested without a render thread
/// (tests/test_cost_table.cpp).
bool warm_pressure(const uint64_t* before, const uint64_t* now);

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
  /// startup rig, a socket path that will not bind, a TCP listener asked for without a token, a signal
  /// set that cannot be created. A missing audio device is **not** fatal -- that is the unplugged
  /// case, and the daemon serves status and retries while it waits.
  bool start(std::string& error);

  /// \brief The control loop. Blocks until SIGTERM/SIGINT or \ref request_stop, then tears down.
  /// \return the process exit code (0 for an ordinary stop).
  int run();

  /// Ask the loop to finish, from any thread. The render thread is stopped by `run()` on the way out.
  void request_stop();

  /// \brief Handle one protocol request, returning one response.
  ///
  /// Public because it is the unit-testable half of the transport: the socket code only has to move
  /// whole messages and keep the connection's authorisation state. Never throws; errors come back as
  /// a structured JSON response.
  ///
  /// This overload is a local, already-authorised session, which is what a caller who is not a
  /// transport means. \ref Session carries the peer kind and the authorisation for a real connection.
  std::string handle_request(const std::string& request);
  std::string handle_request(const std::string& request, Session& session);

  /// The device state, for tests and for a caller that wants the same number the status reports.
  DeviceState device_state() const { return static_cast<DeviceState>(device_state_.load()); }

  /// The chains the running document holds, in the order their channels were bound. Control thread.
  const std::vector<graph::Chain>& chains() const { return current_document_.chains; }

  /// The port the TCP listener is bound to, or 0 when it is not listening. Read back from the socket,
  /// so a configured `tcp_port: 0` ("pick one") is reportable rather than a mystery. Control thread.
  unsigned tcp_port() const { return tcp_port_; }

  /// How many control connections are open on one transport. Control thread; the status reports it,
  /// and a test uses it to see a vanished client released.
  std::size_t connection_count(PeerKind peer) const;

  /// \brief The audio levels: the device's input and output, each chain's, and every node's.
  ///
  /// `engine.levels` builds its whole answer from this and `engine.status` embeds it under `levels`,
  /// so the two can never disagree about what a level is. Each reading *takes* its window: two
  /// callers in a row then describe disjoint windows instead of counting the same samples twice,
  /// which is what a meter wants and what makes the numbers add up over a long run.
  ///
  /// A node is labelled with `node` (its id), `element` (what the client named — its own block, or
  /// the macro it was lowered from) and `role` (which primitive of that macro), so a client that
  /// wants "the level of every block in the rig" groups by `element` and one that wants the part
  /// behind a block has the parts.
  ///
  /// Control thread; public because it is the unit-testable half of the feature, like
  /// \ref handle_request.
  ///
  /// This is the *rate-limited* entry point: with Config::levels_rate_hz above 0 it returns the
  /// held value unless the probes are due to be drained, and \ref collect_levels_if_due is what
  /// drains them. The reply says which happened (`cache.hits` / `cache.misses`), because a client
  /// that is being shown a second-old number should be able to find that out rather than infer it.
  nlohmann::json levels_json();

  /// \brief Drain the probes and rebuild the cached reply if the configured interval has passed.
  ///
  /// Called by the control loop on its own cadence as well as by \ref levels_json, so the cache is
  /// warm whether or not anybody is reading: with a rate set, a client's read should cost the
  /// daemon nothing rather than costing it a measurement at whatever moment the client chose.
  /// \return whether this call performed a drain.
  bool collect_levels_if_due();


  /// \brief Drain every probe and build the reply from what it took. Control thread.
  ///
  /// The half with no policy in it: it always measures and never considers the rate. \ref
  /// collect_levels_if_due decides *when* to call it, so the two questions -- how a level is read and
  /// how often -- are answerable separately, which is what the rate switch is.
  nlohmann::json build_levels_json();

private:
  // --- control thread only -------------------------------------------------
  bool bind_socket(std::string& error);
  /// Bind and listen on the configured TCP address, unless \ref Config::tcp_listen is off.
  bool bind_tcp(std::string& error);
  void close_socket();
  /// Undo whatever \ref start managed before it failed: both listeners, the signal fd and the signal
  /// mask. Without this a failed start leaves a bound socket behind, which the next start then trips
  /// over.
  void close_transport();
  bool setup_signals(std::string& error);
  /// Compile \p rig_path and publish it. Called for the startup rig, SIGHUP, `rig.load` and the cold
  /// half of `rig.switch`.
  /// \param admit_over_budget passed on to \ref publish_document.
  /// \param lite_first publish the document's **Lite** first and the Full behind it (P8), rather than
  /// compiling the Full and leaving the player waiting for it.
  /// \param steps receives what a `lite_first` publish did, for the reply to the client that asked:
  /// `lite_first` (a Lite went first, so the rig started playing sooner), `lite` (that document) and
  /// `full` (what became of the Full behind it). Untouched when \p lite_first is false, and may be
  /// null.
  bool load_rig(const std::string& rig_path, std::string& error, std::string* error_code = nullptr,
                bool admit_over_budget = false, bool lite_first = false, nlohmann::json* steps = nullptr);
  /// \brief P8's two-step publish: the Lite document first, the Full behind it. Control thread.
  ///
  /// The Lite compiles in about a third of the time, so the rig starts playing about that much
  /// sooner; the Full is then compiled while the Lite plays and published as an ordinary second
  /// publish, whose crossfade is the existing transition machinery. The pedal's owner listened to
  /// the fixed E2 audition set and cannot hear a Lite standing in for a Full, which is what makes
  /// the substitution legitimate -- and D6 keeps it visible.
  ///
  /// **There is no catch-up replay**: the Full's chains start from a cold state, so a note that was
  /// already sounding is wrong through the Full for the length of its receptive field (P8's 170 ms).
  /// `nam/lookback.cpp` knows how much to replay, but the input to replay it is not reachable from
  /// this path -- the warm set's block pool holds four blocks of decoded capture and drops what does
  /// not fit (docs/daemon.md, "Lite-first publishes").
  ///
  /// \param report filled on success only, and always with `lite_first` and `full`, plus `lite` when a
  /// Lite really was published; may be null. `lite_first` there is about the publish that happened --
  /// a Lite went first -- while `engine.status`'s `cost.lite_first` is about what is standing in now,
  /// and the two differ only where the budget's own answer is the Lite (`full.state` `unchanged`).
  /// A failure of the *second* publish is not a failure of this function: the Lite is playing and the
  /// reply says the Full did not follow (`full.state` is `refused`, with `error`/`code`).
  /// \return false only when the first publish was refused, in which case nothing changed at all.
  bool publish_lite_first(const graph::Document& document, const std::string& path, const std::string& source,
                          const std::string& base_dir, std::string& error, std::string* error_code,
                          bool admit_over_budget, nlohmann::json* report);
  /// \brief A document worked out as far as this thread's caches can take it, before a model is
  /// loaded.
  ///
  /// `publish_document` is prepare -> compile -> install, and P8's Full splits it across two threads:
  /// the control thread prepares (validate, resolve, cost, which chains changed) and installs, while
  /// the catch-up thread compiles the plans. Nothing here is the daemon's published state -- it is a
  /// *candidate* one, and it only becomes the engine's when `publish_document` installs it.
  struct PreparedPublish
  {
    graph::Document document; ///< as the client wrote it
    graph::Document resolved; ///< references resolved, and models trimmed by the budget
    std::vector<assets::MissingAsset> missing;
    CostOutcome cost;
    std::vector<bool> changed; ///< per chain, against the document that is running
    std::size_t changed_count = 0;
    /// Nothing would change and the chain count matches: the publish is a no-op that only records
    /// where the document came from.
    bool unchanged = false;
    int fade_frames = 0;
    uint64_t generation = 0; ///< the generation the plans will be built for (`generation_ + 1`)
    std::size_t count = 0;
    render::CompileOptions options;
    std::string path, source, base_dir, name;
    bool admit_over_budget = false;
    bool lite_first = false;
    bool catching_up = false;
    /// Everything above that a compile depends on, as one string: how a publish compiled on another
    /// thread is checked against the document it is installed for.
    std::string stamp;
  };

  /// \brief Plans compiled off the control thread, indexed by chain (null where there are none).
  ///
  /// What the catch-up thread hands back for P8's Full: the publish still validates, costs and
  /// installs them -- it just does not spend the compile in the control loop, which is what lets
  /// `engine.status` describe the Lite that is playing (D6).
  using PrebuiltPlans = std::vector<std::unique_ptr<PlanSlot>>;

  /// \brief Everything a publish decides before it loads a model. Control thread.
  ///
  /// \param catching_up the document is the Full of a `lite_first` request: its chains are compiled
  /// whole (never as local transitions, which share node instances with the plan that is playing) and
  /// marked for the catch-up.
  /// \param prepared the candidate publish; untouched on failure except where an error is reported.
  bool prepare_publish(const graph::Document& document, const std::string& path, const std::string& source,
                       const std::string& base_dir, bool admit_over_budget, bool lite_first, bool catching_up,
                       PreparedPublish& prepared, std::string& error, std::string* error_code);

  /// \brief What a prepare produced, as one string a second prepare can be compared against. See
  /// \ref PreparedPublish::stamp.
  static std::string publish_stamp(const PreparedPublish& prepared);

  /// \brief Compile one chain's plan into a slot, off any publish state. Safe on a worker thread.
  ///
  /// The single home for "a whole chain, prewarmed, bound and optionally marked for the catch-up":
  /// `publish_document` calls it on the control thread, the catch-up thread calls it for P8's Full.
  static std::unique_ptr<PlanSlot> compile_chain_slot(const graph::Graph& graph,
                                                      const render::CompileOptions& options, uint64_t generation,
                                                      int input_channel, int output_channel, bool catch_up,
                                                      std::string& error);

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
  /// \param error_code the protocol error code a failure should be reported under. `compile_failed`
  /// unless the asset registry refused a model or IR reference, which is `asset_missing` -- a
  /// different fix for the user (pick another model) from a graph that will not compile -- or the
  /// document's units cost more than the cost table's headroom, which is `over_budget`.
  /// \param admit_over_budget publish a document that is over the headroom even with every model
  /// that can be trimmed trimmed, and report it as a `cost_over` warning, rather than refuse it. For
  /// the publishes nobody can be told "no" about: the configured rig at startup and on SIGHUP, and the
  /// running document again after a library rescan (docs/daemon.md, "Cost and admission").
  /// \param lite_first trim every slimmable model, whatever the budget says, so this publish compiles
  /// and starts playing sooner (\ref publish_lite_first is the whole of P8's first step; this is the
  /// single publish underneath it).
  /// \param catch_up when not null and replayable, this publish is the Full of a `lite_first`
  /// request: every chain is compiled whole -- never as a local transition, because a local
  /// transition *shares node instances* with the plan that is playing and the catch-up thread runs
  /// the new plan beside it -- and every slot is marked `catch_up`, so the render thread holds it
  /// until its state is current instead of crossfading it in cold (P8's catch-up replay).
  bool publish_document(const graph::Document& document, const std::string& path, const std::string& source,
                        const std::string& base_dir, std::string& error,
                        std::string* error_code = nullptr, bool admit_over_budget = false,
                        bool lite_first = false, const CatchUpPlan* catch_up = nullptr,
                        PrebuiltPlans* prebuilt = nullptr);
  /// \brief Resolve a `set_param` against a chain's newest published plan and push it on the ring.
  ///
  /// \param chain_id the chain to resolve against, or empty to resolve by node id -- which is an error
  /// if more than one chain holds that node, because guessing would edit an arbitrary chain.
  /// \param port the port a per-port parameter belongs to (a mixer's per-input level, mute, phase), or
  /// -1 for a parameter of the node itself.
  bool publish_param(const std::string& chain_id, const std::string& node, int port, const std::string& param,
                     double value, std::string& error);
  bool push_slot(uint32_t chain, uint32_t chain_count, PlanSlot* slot);
  /// \brief Compile \p document and hand it to the warm thread, so that switching to it is a
  /// handover rather than a load (docs/warm-set.md).
  ///
  /// Control thread. Everything `publish_document` does up to publication, and then a mailbox instead
  /// of the render path: no command is pushed, no generation a client polls is advanced, and the
  /// running document is not touched. The compilation is the expensive half -- every model is loaded
  /// and prewarmed -- and it happens here, on the control thread, so that the switch itself is only a
  /// change of which document the device thread renders.
  ///
  /// \param replace warm this rig even though another is already warm, displacing it. With false, a
  /// warm set that is already full is refused as `warm_full` rather than replaced silently.
  /// \param error_code `warm_disabled`, `warm_full`, `warm_not_ready`, `over_budget`, `asset_missing`
  /// or `compile_failed`.
  bool publish_warm(const graph::Document& document, const std::string& path, const std::string& source,
                    const std::string& base_dir, bool replace, std::string& error,
                    std::string* error_code = nullptr);
  /// \brief Stop warming: retire the warm document's plans and let the document go. Control thread.
  ///
  /// A no-op when nothing is warm. \p rig, when not empty, must name a rig that is actually warm, so
  /// that a client cooling the wrong one is told rather than obeyed. \p cooled receives the rigs that
  /// stopped being warm (one today).
  bool cool_warm(const std::string& rig, std::string& error, std::string* error_code, nlohmann::json& cooled);
  /// \brief Make \p rig audible, as a handover when it is warm and as a publish when it is not.
  /// Control thread; blocks until the switch completes or its deadline passes.
  /// \param lite_first asked for a two-step publish if this turns out to be a cold switch. A rig that
  /// is *warm* is already compiled, so a handover beats any Lite compile: the flag is ignored and the
  /// body says `switched: warm` with `lite_first: false`.
  /// \param steps receives what the cold path's `lite_first` publish did (\ref publish_lite_first).
  bool switch_to(const std::string& rig, bool fallback, std::string& error, std::string* error_code,
                 nlohmann::json& body, bool lite_first = false, nlohmann::json* steps = nullptr);
  /// \brief Catch the control thread's view up with a switch the render thread has performed.
  ///
  /// A warm switch changes which document is audible without a publish, so the fields that describe
  /// "the running document" -- its rig, its chains, its cost, its plans -- have to move with it. The
  /// two compiled contents swap; nothing is recompiled and nothing is freed. Control thread.
  void sync_warm_switch();
  /// \brief The warm set, as `engine.status` reports it. Control thread.
  nlohmann::json warm_json() const;
  /// Which document holds \p content, or -1. Read by either thread: the lookup a control-thread
  /// reply and the device thread's switch check both need.
  int document_with_content(uint64_t content) const;
  /// Whether warming is configured at all (\ref Config::warm_cpus).
  bool warm_enabled() const { return !config_.warm_cpus.empty(); }
  /// \brief How many warm documents the engine is holding right now, which is what D3's coexistence
  /// charge (2026-10-09) is keyed by. Control thread.
  int warm_documents() const { return warm_entry_.present ? 1 : 0; }
  /// \brief Whether the audible rig can afford one more warm document beside it: the charge for
  /// `warm_documents() + 1` applied to what it costs now must still fit the headroom. False when the
  /// engine has no cost for the audible document, in which case there is nothing to charge against and
  /// the honest answer is no. Control thread.
  bool coexistence_affords_one_more_warm() const;
  /// \brief Act on \ref warm_pressure: cool the warm set and refuse to hold another for a while.
  ///
  /// Called from the control loop. Nothing on the render path consults it; it changes what the engine
  /// is *holding*, not what it is rendering, so the worst case of getting it wrong is a press that
  /// costs a cold switch. Control thread.
  void check_warm_pressure();
  /// True while a pressure cool is in force: `rig.warm` is refused (`warm_pressure`) so a service
  /// asking every second cannot put the pedal back into the state the failsafe just left.
  bool warm_pressure_holds() const;
  /// \brief Whether the render thread is playing \ref published_slots_ for chain \p index and has
  /// nothing in flight or queued for it: the only state a local transition may be planned against,
  /// because it shares instances with exactly that plan. Control thread.
  bool chain_settled(std::size_t index) const;
  /// Wait, briefly, for \ref chain_settled. A fade is a few milliseconds, so a burst of edits is
  /// usually settled by the time the next one has compiled; if not, the caller falls back to a
  /// whole-chain crossfade, which can be queued behind anything. Control thread.
  bool wait_until_settled(std::size_t index);
  /// The index of the chain with this id in the running document, or -1. Control thread.
  int find_chain(const std::string& chain_id) const;
  /// The index of the one chain whose newest plan holds \p node, or -1 for none and -2 for several.
  int find_chain_with_node(const std::string& node) const;
  void set_device_state(DeviceState state);
  void stop();

  // --- control thread: the transports --------------------------------------
  struct Connection;
  /// Accept whatever is pending on \p listener, up to that transport's connection cap. Non-blocking,
  /// so a burst of connections costs a loop rather than a stall.
  void accept_connections(int listener, PeerKind peer);
  /// Answer \p fd with one refusal and close it. Used when a transport is already at its cap, where
  /// the useful answer is `too_many_connections` rather than a connection that hangs up silently.
  void refuse_connection(int fd, PeerKind peer, const char* code, const std::string& message);
  /// \brief Read what \p connection has to offer and answer every whole message in it.
  ///
  /// \return false when the connection is finished and should be closed: the peer went away, broke the
  /// framing, or was told its last reply before being disconnected.
  bool read_connection(Connection& connection);
  /// Read every whole SEQPACKET datagram waiting on a local connection. One datagram is one message,
  /// so this transport has no framing to reassemble.
  bool read_datagrams(Connection& connection);
  /// Reassemble length-prefixed messages (\ref framing.h) from a remote stream and answer each.
  bool read_framed(Connection& connection);
  /// Write as much of \p connection's pending reply as the kernel will take. \return false when the
  /// connection is finished -- the peer is gone, or the reply it was owed has now been delivered and
  /// the connection was marked to close.
  bool write_connection(Connection& connection);
  /// Queue one reply, framed or not depending on the transport, and mark the connection to close if a
  /// client that never reads has let it grow past the cap.
  void queue_reply(Connection& connection, std::string reply);
  void close_connection(std::size_t index);
  /// Close connections whose authentication window or flush deadline has passed.
  void drop_expired_connections();
  /// \brief Take the recording away from the render thread and finish its files. The stopped recorder
  /// is kept, so its summary stays readable until the next recording. Control thread; a no-op when
  /// nothing is recording.
  void end_recording();
  /// The largest message accepted on \p peer's transport. Local keeps the fixed SEQPACKET size;
  /// remote uses \ref Config::tcp_max_message_bytes.
  std::size_t max_message_bytes(PeerKind peer) const;

  // --- render thread only --------------------------------------------------
  void render_loop();
  /// \brief Start the render pool, if `render_threads` is above one.
  ///
  /// Called by the render thread *before* it promotes and pins itself: a worker is born on its own
  /// core, but on a platform where that cannot be arranged it still has to be able to run to pin
  /// itself, and a `SCHED_FIFO` coordinator already spinning on its core would never let it.
  /// A pool smaller than asked for is reported, never silent.
  bool start_render_pool(std::string& error);
  /// \brief Publish what the pool actually has, for `engine.status`. Render thread, once, after the
  /// coordinator has taken its own scheduling.
  void publish_render_state();
  /// \brief Ask for real-time scheduling with a window sized to \p period_seconds, and publish what
  /// resulted. Called at start-up (the configured period as an estimate) and again after a device
  /// opens, when the period it actually hands over is known.
  void promote_render_thread(double period_seconds);
  /// \brief One participant's share of a block: every `participants`-th chain, plans only.
  ///
  /// Run on the coordinator and on every worker. It touches chain plans and chain buffers and
  /// nothing else: no crossfade mix, no transition bookkeeping and no output bus, all of which stay
  /// on the coordinator so that a chain still has exactly one owner for state (docs/multi-core.md).
  void run_chains(unsigned participant, unsigned participants, int frames) noexcept;
  /// The pool's callback: `context` is the Daemon.
  static void run_chains_entry(void* context, unsigned participant, unsigned participants,
                               int frames) noexcept;
  bool open_device(std::string& error);
  /// Give the backend its chance to flush what it accumulated, before the object is destroyed: a
  /// `file` backend writes its output WAV here, and a device backend has nothing to do.
  void finish_device(std::string& error);
  void close_device();
  void fail_device(const std::string& reason);
  void drain_commands(DocRuntime& doc);
  /// \brief Run one chain's plans for this block, on the participant's share of them.
  ///
  /// The planes it reads and writes are the document's, so a document is only ever passed to the
  /// participant that owns it: the pool splits a *document's* chains when `render_threads` is above
  /// one, and a warm document is not part of that pool at all.
  void run_document_chains(DocRuntime& doc, unsigned participant, unsigned participants, int frames) noexcept;
  /// \brief Everything that must keep one owner, after the plans have run: the crossfade mixes, the
  /// local-transition bookkeeping and the sum into the document's bus. Render thread.
  void finish_document_block(DocRuntime& doc, int frames) noexcept;
  /// \brief Publish the decoded capture for the warm thread, if there is a warm document.
  ///
  /// One decode of every channel a chain may name, into a block from the pool. Nothing is decoded
  /// when nothing is warm, which is what keeps the cost of the feature at zero until it is used.
  void publish_warm_input() noexcept;
  /// \brief Advance a document switch by a block, before the plans run: ask for the incoming
  /// document, take it over when the warm thread has let go of it, and start the fade. Render thread.
  void switch_begin() noexcept;
  /// \brief Finish a block: what the device hears is the audible document's bus, or the crossfade
  /// between two of them while a switch runs. Render thread.
  void switch_mix(int frames) noexcept;
  /// The fade length in frames for a structural change, from `fade_ms` and the running rate.
  int transition_frames() const noexcept;
  /// \brief The document the device thread is rendering. Render thread (and a relaxed read elsewhere).
  DocRuntime& active_document() noexcept { return docs_[active_doc_.load(std::memory_order_relaxed)]; }
  const DocRuntime& active_document() const noexcept { return docs_[active_doc_.load(std::memory_order_relaxed)]; }
  /// After a period is written: publish the backend's queue counters, and hand the period to the
  /// recording if there is one. Render thread.
  void account_period();
  /// Begin an equal-power transition for one chain. Render thread.
  void start_transition(DocRuntime& doc, int index, PlanSlot* next);
  /// Retire the faded-out plan, promote the incoming one, and start whatever is queued. Render thread.
  void finish_transition(DocRuntime& doc, int index);
  /// Take over a chain with a local transition's first plan: copy the carried buffers across and swap,
  /// with no whole-chain fade. Render thread.
  void start_local_transition(DocRuntime& doc, int index, PlanSlot* slot);
  /// The local transition's crossfades have closed: hand over to its final plan. Render thread.
  void finish_local_transition(DocRuntime& doc, int index);
  /// Start the next queued chain transition, if one is waiting and none is running. Render thread.
  void start_queued_transition(DocRuntime& doc);
  /// Retire a chain's plans and forget it: a document with fewer chains than the last one. Render
  /// thread.
  void drop_chain(DocRuntime& doc, int index);
  /// Recompute a document's generation fields from its per-chain ones. Render thread.
  void update_aggregate_state(DocRuntime& doc);

  // --- the warm thread (docs/warm-set.md) ----------------------------------
  /// \brief Start the warm thread, if warmth is configured. Render thread, before it pins itself.
  ///
  /// Called before the render thread promotes itself for the same reason the render pool is: a thread
  /// is born on its own core, and one created on a core a `SCHED_FIFO` thread is already spinning on
  /// could never run to pin itself.
  bool start_warm_thread(std::string& error);
  /// Park the warm thread and join it. Idempotent; called on the way out of the render loop.
  void stop_warm_thread() noexcept;
  /// The warm thread's loop: take work, render what it owns, publish, park.
  void warm_loop();
  /// \brief One pass of the warm thread: drain the mailbox, adopt or cool documents, render every
  /// block waiting in the input pool, publish the bus when a switch is listening. Warm thread.
  void warm_service() noexcept;
  /// \brief The document the warm thread should own and render, or null.
  ///
  /// Every document that is free, is not the audible one and has content: adopting the outgoing
  /// document after a switch needs no message, which is what makes the handover one block of setup
  /// rather than a conversation. Warm thread.
  DocRuntime* claim_warm_document(int& index) noexcept;
  /// \brief Render one document for one input block. Warm thread; no crossfade, no transition
  /// bookkeeping, no levels: a warm document is a document, not a mix.
  void render_warm_document(DocRuntime& doc, const float* planes, uint64_t seq) noexcept;
  /// \brief Give \p doc back to nobody: clear what it holds, retire its plans, store `Free`.
  ///
  /// \param retire whether the content is going away (a cool) or merely changing hands (a switch,
  /// which keeps its plans for the device thread to take over).
  void release_warm_document(DocRuntime& doc, bool retire) noexcept;
  /// Retire every plan \p doc holds, through the warm thread's own reclaimer, and forget the content.
  /// Warm thread; the plans are freed by the control thread once the grace window has closed.
  void retire_warm_content_of(DocRuntime& doc) noexcept;
  /// Retire content that was never installed -- a compiled document superseded before it landed.
  void retire_warm_content(const WarmContent& content) noexcept;

  // --- the catch-up (P8's replay: pedal.project docs/…-fast-switching.md, "P8") ----------------
  /// \brief What a catch-up is asked to do: catch these plans up to the input the device is playing.
  ///
  /// One job per `lite_first` publish, handed from the control thread to the catch-up thread. Plain
  /// data: the plans are owned by the render thread's pending slots while this runs, and the job only
  /// names them.
  struct CatchUpJob
  {
    PlanSlot* slots[kMaxChains] = {};
    /// The capture channel each chain reads, and the frames of input its state needs.
    int channel[kMaxChains] = {};
    std::size_t frames[kMaxChains] = {};
    unsigned chain_count = 0;
    std::size_t window_frames = 0;
    uint64_t id = 0;
  };

  /// \brief What became of the last catch-up, for the reply and for `engine.status`. Control thread.
  struct CatchUpReport
  {
    /// The document's own answer (`CatchUpPlan::state`): `exact`, `inexact`, `empty`, `unbounded`,
    /// `unknown`, `too_long`. What the engine knows before it tries.
    std::string state = "none";
    /// What actually happened: `none` (not attempted), `exact` (the window covered and was
    /// replayed), `gap` (the history had a hole), `no_input` (no window arrived in time), `busy`
    /// (another catch-up was still running), `abandoned` (the render thread gave up waiting and
    /// faded the Full in anyway). Empty until a catch-up has been tried or deliberately not tried.
    std::string replay = "none";
    /// Where the two-step publish's Full is: `idle`, `preparing` (prepared, waiting for the compile),
    /// `compiling`, `published`, `unchanged`, `refused`, `superseded` or `catching_up`. This is the
    /// half of D6's visibility that the *reply* cannot carry once the compile moves off the control
    /// thread: a client polls `engine.status` and watches the Full arrive.
    std::string full = "idle";
    std::string detail;
    std::size_t frames = 0; ///< the window the document needs, in engine frames
    std::size_t blocks = 0; ///< blocks replayed (and mirrored) when the report was built
    bool running = false;   ///< a catch-up job is in flight
    /// How long the render thread will hold the Full waiting for that replay, in frames: the longest
    /// of the chains' own budgets (`PlanSlot::catch_up_hold_frames`). Reported because it is the
    /// bound that decides `abandoned` against `exact`, and it is not a constant any more.
    std::size_t hold_frames = 0;
    /// \brief Where the *catch-up thread* had got to when this report was folded in: `idle` (no job
    /// yet), `filling` (draining the ring into a window that covers), `replaying` (feeding the plans),
    /// `mirroring` (keeping them current until the render thread takes them over), `done`.
    std::string progress = "idle";
    /// Why the fill loop stopped: `need_met` (a window that covers, contiguous), `drained_head` (the
    /// ring was drained and what is held does not cover), `deadline` (two seconds without a window),
    /// `unarmed` (recording stopped under it), `quit`.
    std::string fill = "none";
    /// Which block size the recording was sized for, and what the device was actually rendering: a
    /// mismatch means `write_input_history` recorded nothing at all, which is the one cause of an
    /// empty ring that is invisible from outside the process.
    int history_block_frames = 0;
    int device_period_size = 0;
    /// Blocks the device thread refused to record because the device's period was not the one the
    /// history was sized for, since the last arm.
    uint64_t period_mismatch = 0;
    /// The window the catch-up thread had built when this was folded in: how many blocks it held and
    /// how many of them were contiguous at the end (a replay needs `covers` worth of the second).
    std::size_t window_held = 0;
    std::size_t window_contiguous = 0;
    std::size_t window_blocks = 0;
    std::size_t window_need_blocks = 0;
    /// The ring's own counters, read at the same moment.
    uint64_t ring_armed = 0;
    uint64_t ring_written = 0;
    uint64_t ring_consumed = 0;
    uint64_t ring_dropped = 0;
    uint64_t ring_head = 0;
    uint64_t ring_tail = 0;
    uint64_t ring_newest_seq = 0;
    uint64_t ring_oldest_seq = 0;
    /// \brief Whether the render thread took the Full over because the replay had made it exact, or
    /// because its patience ran out: the first is `catch_up.replay: exact`, the second `abandoned`.
    /// Cumulative since the daemon started, and the one pair of counters that says which of the two
    /// the hand-over was -- `replay` alone cannot, because a plan taken over as `exact` is briefly
    /// reportable as `abandoned` too if it is held a block longer than it needs to be.
    uint64_t hold_exact = 0;
    uint64_t hold_abandoned = 0;
    /// The block count each last happened at, for "was the replay already current when it was
    /// abandoned".
    uint64_t hold_exact_at = 0;
    uint64_t hold_abandoned_at = 0;
    /// \brief How far the replay had fed the plan, and how far the *executed* state had got.
    ///
    /// `fed` is what the hand-over decides on (`PlanSlot::catch_up_ready`); `executed` is the state
    /// itself (`PlanSlot::catch_up_seq`), written just after it. One block apart is the steady state;
    /// a `fed` that stands still while the device renders is the catch-up thread falling behind.
    uint64_t fed_seq = 0;
    uint64_t executed_seq = 0;
  };

  /// \brief Start recording input for a catch-up, and work out nothing else: the job comes later.
  ///
  /// Control thread. Sizes the ring and starts the device thread recording into it, so that the
  /// history covers the Lite's compile, the wait for it to settle and the Full's compile -- the whole
  /// of a two-step publish. False when a catch-up is already in flight (`busy`) or the plan needs no
  /// replay.
  bool arm_catch_up(const CatchUpPlan& plan, unsigned chains);
  /// \brief What a document's models need replayed, from the document as written.
  ///
  /// Control thread: resolves a copy the way a publish does -- relative paths, then the library --
  /// and asks `catch_up_plan` about the result. Never fails: a document that cannot be resolved gets
  /// `unknown` with the reason, which is what the reply reports.
  CatchUpPlan catch_up_plan_for(const graph::Document& document, const std::string& base_dir);
  /// Stop recording, and clear what the last catch-up needed. Control thread; a no-op when nothing is
  /// armed.
  void disarm_catch_up() noexcept;
  /// Hand \p job to the catch-up thread, starting it if this is the first. Control thread.
  void start_catch_up_job(const CatchUpJob& job);
  /// Join the catch-up thread. Called by `stop`, before anything it may be executing is freed.
  void stop_catch_up_thread() noexcept;
  /// The catch-up thread: take a job, drain the history, replay it, keep the plans current until the
  /// render thread takes them over.
  void catch_up_loop();
  /// One compile, on the catch-up thread: every chain of the Full the control thread prepared. Puts the
  /// plans on \ref async_full_ and moves it to `AwaitingInstall`.
  void compile_async_full();
  /// One job, on the catch-up thread.
  void serve_catch_up(const CatchUpJob& job);
  /// Render thread: record this block's decoded capture for a catch-up. One atomic load when none is
  /// armed, which is every block of an ordinary run.
  void write_input_history() noexcept;
  /// \brief Render thread: advance every held catch-up plan by one block: take over the ones whose
  /// state is exactly current, abandon the ones that have waited too long.
  void advance_catch_up(DocRuntime& doc, int frames) noexcept;
  /// How long a catch-up plan may be held before the render thread gives up, in frames.
  int catch_up_hold_frames() const noexcept;
  /// \brief How long the render thread will hold a catch-up plan whose chain needs \p chain_frames of
  /// input: \ref kCatchUpHoldMarginFrames above the chain's own memory, at least
  /// \ref kCatchUpHoldFrames, at most \ref kMaxCatchUpHoldFrames, rounded to a whole device period.
  /// Control thread, when a catch-up job is handed over. See `PlanSlot::catch_up_hold_frames`.
  int catch_up_hold_budget_frames(std::size_t chain_frames) const noexcept;
  /// Build the `catch_up` object the reply and the status carry.
  nlohmann::json catch_up_json() const;
  /// \brief The Full of a two-step publish, compiled off the control thread (P8, and D6's visibility).
  ///
  /// The control thread's half is the *prepare* -- validate, resolve, cost, which chains changed --
  /// and the install; the catch-up thread's half is the compile of every chain, which is the seconds
  /// a client would otherwise spend unable to ask the engine anything. Guarded by \ref catch_up_mutex_,
  /// because the two threads hand it back and forth.
  struct AsyncFull
  {
    /// Where the hand-over has got to. `Compiling` is the state D6 is about: the Lite is playing, the
    /// Full's plans do not exist yet, and the control loop is free to answer.
    enum class Phase
    {
      Idle = 0,
      Compiling,      ///< the catch-up thread is loading models
      AwaitingInstall,///< compiled; the control thread installs it on its next pass
      CatchingUp,     ///< installed; the catch-up thread is making its state current
    };

    Phase phase = Phase::Idle;
    /// Prepared on the control thread.
    graph::Document document;
    std::string path, source, base_dir;
    bool admit_over_budget = false;
    CatchUpPlan catch_up;
    /// \ref PreparedPublish::stamp for the prepare the compile was started from: if a second prepare
    /// of the same document says something else, the compiled plans are not the ones to install.
    std::string stamp;
    render::CompileOptions options;
    std::vector<graph::Graph> graphs; ///< one per chain, as the compile will read them
    std::vector<int> input_channel, output_channel;
    uint64_t generation = 0;
    /// Compiled on the catch-up thread; moved into the publish by the control thread, and anything
    /// left over is freed by it.
    PrebuiltPlans prebuilt;
    bool compiled = false;
    std::string compile_error;
    /// The catch-up job the install produced, handed to the catch-up thread when the install lands.
    CatchUpJob job;
  };

  /// \brief Prepare a Full and hand it to the catch-up thread to compile. Control thread.
  ///
  /// False when the Full is not one that can follow the Lite at all (over the headroom, an asset that
  /// has gone, nothing to trim): the caller reports it and the Lite stands, exactly as a synchronous
  /// second publish would have reported it.
  bool start_async_full(const graph::Document& document, const std::string& path, const std::string& source,
                        const std::string& base_dir, bool admit_over_budget, const CatchUpPlan& catch_up,
                        std::string& error, std::string* error_code);
  /// \brief Install a compiled Full, if there is one -- called from the control loop and from every
  /// request, so a quiet daemon still publishes it. Control thread.
  void service_async_full();
  /// Fold the catch-up thread's own counters into \ref catch_up_report_. Control thread.
  void service_catch_up_report();
  /// \brief Clear `cost.lite_first` once the Full the Lite was standing in for is what the device is
  /// playing. Control thread; cheap when there is nothing to clear.
  void clear_lite_first_when_adopted();

  /// The catch-up thread as the status reports it: whether it is running, and the ring's counters.
  bool catch_up_thread_running() const noexcept { return catch_up_thread_.joinable(); }

  /// \brief One open control connection, on either transport.
  ///
  /// The buffers are per connection because a stream socket has no message boundaries: \ref in holds
  /// the bytes of a message that has not finished arriving and \ref out holds a reply the kernel would
  /// not take. Both exist so that a client which reads slowly, or stops reading, costs its own
  /// connection rather than the control loop -- which is the failure mode of a blocking `send` on a
  /// single-threaded server, and the reason for the send-buffer limit in \ref kTcpSendBufferBytes.
  struct Connection
  {
    int fd = -1;
    Session session;
    /// The receive buffer. On TCP it holds bytes that have arrived but do not yet form a whole
    /// message, which is what reassembly means; on the datagram transport it is a scratch buffer for
    /// the one message being read. Either way it is per connection, so one client's half-message is
    /// never another's problem.
    std::vector<char> in;
    std::vector<char> out; ///< replies queued but not yet accepted by the kernel
    std::size_t out_sent = 0;
    /// When the connection was accepted, and when its current deadline expires: the authentication
    /// window for an unauthenticated remote peer, the flush window for one being disconnected. A
    /// connection with no deadline is a local one, or an authenticated remote one that is behaving.
    std::chrono::steady_clock::time_point accepted_at{};
    std::chrono::steady_clock::time_point deadline{};
    bool has_auth_deadline = false;
    bool has_flush_deadline = false;
    /// Set when the reply just queued is the last one this connection gets: the third failed
    /// authentication, a message that broke the framing, or an outbox past its cap. It is closed once
    /// that reply has drained, so the peer is told *why* rather than seeing a bare hangup.
    bool close_after_flush = false;
    std::string peer; ///< for logging: `unix`, or `127.0.0.1:41234`
  };

  Config config_;
  /// The device's model and IR library (slice D). Control thread only: it is read when a document is
  /// compiled and when `engine.assets`/`engine.schema` is answered, never on the render path.
  assets::Registry assets_;
  // Sized once; the render path only pushes/pops. Big enough for a whole document plus the ramps a
  // document apply can turn into (every value a kept node changed is one command), because a document
  // publish is all-or-nothing: if every chain's commands do not fit, none of them is pushed.
  rt::SpscRing<PlanCommand> commands_{kMaxChains + 504};
  // `fade + 2` blocks of grace. There is no fade yet, so two epochs; capacity is a publish storm's
  // worth (a local transition retires two slots: the plan it replaced and its own transitional one),
  // and a full queue is counted rather than silently growing.
  rt::Reclaimer reclaimer_{2, 64};

  int listen_fd_ = -1;
  /// The TCP listener, and the port it actually bound (read back from the socket, so `tcp_port: 0`
  /// is reportable). Both are control-thread-only.
  int tcp_fd_ = -1;
  unsigned tcp_port_ = 0;
  int signal_fd_ = -1;
  /// The newest published document. Control-thread-only, and never dangling: a slot is retired only
  /// when a *newer* document is adopted, and this pointer has already moved to that newer one by then.
  const graph::Document* last_published_ = nullptr;
  /// The document the newest published plans were built from. Control-thread-only; it is the base a
  /// structural `rig.patch` mutates, and it is replaced only by a successful publish.
  graph::Document current_document_;
  std::vector<PlanSlot*> published_slots_; ///< parallel to current_document_.chains
  /// \ref current_document_ with its asset references resolved and its missing assets stood in for:
  /// what the plans were actually compiled from. Whether a chain changed is decided on this, so a
  /// model that has appeared in the library since (or gone from it) is a change to recompile.
  graph::Document current_resolved_;
  /// The references in \ref current_document_ that resolved nowhere, each played as a bypass
  /// (assets::resolve_document). Reported as `asset_missing` issues and per chain in `engine.status`.
  std::vector<assets::MissingAsset> missing_assets_;
  /// What the cost table made of \ref current_document_: every unit's charge, which models were
  /// trimmed, and whether the document was admitted over the headroom (docs/daemon.md, "Cost and
  /// admission").
  CostOutcome cost_;
  /// This device's measured cost table, re-read on SIGHUP. Control thread.
  CostTable cost_table_;
  /// What a model's family and an IR's taps are, by file: what a charge needs, cached so a publish
  /// does not reparse a library. Control thread.
  UnitCache units_;
  /// \brief What the newest publish did with each chain it changed. Control thread; reported in the
  /// publish's reply and in `engine.status`, so a soak can say which edits faded locally.
  struct ChainTransition
  {
    std::string chain;
    /// `"local"` (kept what it could, faded what changed), `"instant"` (kept everything, nothing to
    /// fade), `"ramps"` (only values that ramp moved and bypasses flipped: no new plan), or
    /// `"crossfade"` (the whole chain).
    std::string kind;
    int fades = 0;
    int kept = 0;
    int created = 0;
    int retiring = 0;
    int ramps = 0;
    int bypasses = 0; ///< bypasses crossfaded inside the running plan
    /// Why a chain was crossfaded whole rather than locally; empty otherwise.
    std::string reason;
  };
  std::vector<ChainTransition> last_transitions_;
  /// Republish the running document after a library rescan, when it has missing assets: a model
  /// copied in since then replaces its stand-in without anyone editing the rig.
  void republish_after_rescan();
  /// Read \ref config_.cost_table again: at startup, and on SIGHUP, which is when an operator who has
  /// just re-measured the device expects the new numbers to be the ones admission uses. A table that
  /// cannot be read leaves every unit at its unknown cost and says so -- it is never fatal.
  void load_cost_table();
  /// The signal mask in force before \ref setup_signals, restored on stop so a library that started
  /// this daemon does not inherit a blocked SIGTERM for the rest of the process's life.
  sigset_t previous_mask_{};
  bool mask_saved_ = false;
  /// Every open control connection, on both transports. A vector rather than a fixed array because a
  /// connection is created and destroyed at runtime and nothing on the render path ever sees it.
  std::vector<Connection> clients_;
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
  /// The worker threads behind `Config::render_threads`. Idle (one participant, no threads) unless
  /// the configuration asks for more; stopped with the render thread.
  RenderPool render_pool_;

  // Render-thread-owned state. Only the render thread touches these.
  std::unique_ptr<AudioBackend> device_;
  bool device_started_ = false;
  /// Whether \ref finish_device has already run for this open. A `file` backend writes its WAV there,
  /// and the render loop can leave by two paths (input exhausted, or a stop), so "finish once, before
  /// the backend is destroyed" is stated rather than implied.
  bool device_finished_ = false;
  /// \brief The documents this engine can render: the audible one and the warm ones (docs/warm-set.md).
  ///
  /// `docs_[active_doc_]` is what the device is hearing. A document is owned by exactly one thread at
  /// a time (\ref DocOwner), and with warmth off only `docs_[0]` is ever touched -- which is what makes
  /// a build without it behave exactly as it did before there were documents here at all.
  DocRuntime docs_[kMaxDocuments];
  /// The document the device thread is rendering. Written by the device thread, read by everyone: it
  /// is how a client is told which rig is audible and how the control thread knows which compiled
  /// content it is looking at.
  std::atomic<int> active_doc_{0};
  /// The audio the device thread is writing: the active document's bus, or the crossfade between two
  /// of them while a switch runs.
  std::vector<float> bus_;
  /// \brief One channel's document-switch crossfade.
  ///
  /// A wrapper because \ref rt::Crossfade sizes its weight table once and so is deliberately not
  /// default-constructible, and the switch needs one per playback channel: the fade is over the two
  /// documents' *buses*, not over a chain, so each channel measures its own correlation.
  struct SwitchCrossfade
  {
    rt::Crossfade fade{kMaxTransitionFrames};
  };
  SwitchCrossfade switch_fades_[kMaxLevelChannels];
  /// \brief The document switch in flight, if any.
  ///
  /// Off, waiting for the warm thread to let go of the incoming document, or fading. Only one at a
  /// time: a switch that arrived while another is fading is served when the fade closes, and the
  /// control thread's request mailbox is checked then.
  enum class SwitchPhase
  {
    Idle,
    AwaitingRelease,
    Fading,
  };
  SwitchPhase switch_phase_ = SwitchPhase::Idle;
  int switch_incoming_ = -1;     ///< the document becoming audible
  int switch_outgoing_ = -1;     ///< the document fading out (released to the warm thread)
  /// The outgoing document's audio for the block being mixed: its own last block at first, then
  /// whatever the warm thread has published. Held when nothing new arrived, and counted.
  std::vector<float> switch_outgoing_bus_;
  uint64_t switch_started_block_ = 0;
  uint64_t switch_seen_seq_ = 0; ///< the last switch request this thread acted on
  /// The driver's xrun counter as of the last block, so the recent-trouble bucket can be fed the
  /// delta rather than the total.
  uint64_t last_xruns_ = 0;
  std::vector<unsigned char> capture_buffer_;
  std::vector<unsigned char> playback_buffer_;
  /// \brief Every decoded capture channel of the block being rendered, for the warm thread to read.
  ///
  /// The device thread is the only thread with the capture, so a warm thread renders from what it
  /// publishes: one plane per channel a chain may name (\ref Config::capture_channels), decoded once.
  /// Sized when the device opens.
  std::vector<float> warm_input_planes_;
  unsigned warm_input_channels_ = 0;
  /// The input sequence number of the block being rendered: the warm thread's `behind_blocks` is this
  /// minus the sequence of the last block it rendered.
  uint64_t input_sequence_ = 0;
  /// The same number, published for the control thread's status: a plain member would be a race with
  /// a reader that is not on the render path.
  std::atomic<uint64_t> input_sequence_published_{0};

  // --- the warm set (docs/warm-set.md) ---------------------------------------------------------
  //
  // One warm thread, one compiled document handed to it at a time, and the mailboxes the two threads
  // and the control thread use to hand work over. Nothing here is a lock: each mailbox is written by
  // one thread and read by one other, and the handover of a document is the two-phase claim/release
  // in doc_runtime.h.
  /// \brief The warm thread: the core it is pinned to, the input it renders from, and what it got.
  struct WarmRuntime
  {
    std::thread thread;
    /// The core it is pinned to, -1 before it is started. Atomic because the status reports it and the
    /// thread is started by the render thread.
    std::atomic<int> cpu{-1};
    /// What the thread asked the kernel for and what it got. Written by the warm thread once, before
    /// it renders anything; read only for a report.
    rt::SchedulingState scheduling;
    /// The decoded capture blocks the device thread hands over. One pool per warm thread: a pool is
    /// single-consumer, and two warm threads share nothing.
    BlockPool inputs;
    /// "There is work": bumped by whichever thread has published something, waited on by the warm
    /// thread when it has nothing to do. A counter rather than a flag so a burst cannot be missed.
    std::atomic<uint64_t> sequence{0};
    std::atomic<bool> parked{false};
    /// Blocks this thread has rendered, and the input sequence of the last one.
    std::atomic<uint64_t> blocks{0};
  };
  WarmRuntime warm_;
  /// \brief Wake the warm thread if it has parked. Any producer of work for it calls this.
  ///
  /// The counter is bumped either way -- that is what makes the work visible -- but the futex wake is
  /// only worth its syscall when the thread is actually parked, which is the handshake `RenderPool`
  /// uses for the same reason.
  void wake_warm_thread() noexcept
  {
    warm_.sequence.fetch_add(1, std::memory_order_release);
    if (warm_.parked.load(std::memory_order_acquire))
      warm_.sequence.notify_one();
  }
  /// Control thread -> warm thread: compiled content waiting to be rendered. Single producer, single
  /// consumer, and a document is only adopted when one is free -- so a command can wait here across a
  /// switch without anything being lost.
  rt::SpscRing<WarmContent> warm_commands_{16};
  /// Device thread -> warm thread: let go of this document (index + 1), or 0. It keeps its content:
  /// the device thread is about to render it, which is what a switch is.
  std::atomic<unsigned> warm_release_{0};
  /// The document the device thread has asked for and not yet taken, or -1.
  ///
  /// A second statement of the same handover, and it has to be separate: the warm thread's rule is to
  /// render every free document that has content, so a document it has just let go of looks exactly
  /// like one it should pick up, and it would take it back before the device thread could claim it.
  /// Cleared when the claim lands, or when the request is withdrawn.
  std::atomic<int> take_request_{-1};
  /// Control thread -> warm thread: retire this document's content (index + 1), or 0, and then let
  /// the document go. Used by `rig.cool`; the opposite of \ref warm_release_ in that the content is
  /// going away rather than changing hands.
  std::atomic<unsigned> warm_cool_{0};
  /// Device thread -> warm thread: stop. Set on the way out of the render loop.
  std::atomic<bool> warm_quit_{false};
  /// Control thread -> device thread: switch to this content (0 = none), plus a sequence number that
  /// makes a repeat request visible. The content is checked, not the document: a request for content
  /// that has since been replaced is withdrawn rather than served.
  std::atomic<uint64_t> switch_request_content_{0};
  std::atomic<uint64_t> switch_request_seq_{0};
  /// Device thread -> control thread: the request it has finished with, and what happened to it.
  std::atomic<uint64_t> switch_done_seq_{0};
  std::atomic<uint64_t> switches_{0};
  std::atomic<uint64_t> switches_withdrawn_{0};
  /// Which document the last served request landed in, or -1.
  std::atomic<int> switch_last_doc_{-1};
  /// Warm thread only: content that could not be installed when it arrived (the document the control
  /// thread named was not free yet), kept and retried. One slot, newest wins.
  WarmContent pending_warm_;
  bool warm_have_pending_ = false;
  /// Warm thread only: the document it is holding, for the behind-blocks gauge in the status.
  int warm_doc_ = -1;
  /// The warm thread's own reclaimer. A plan is retired by whichever thread owns the document it was
  /// playing in, so there are two producers and the control loop collects both.
  rt::Reclaimer warm_reclaimer_{2, 64};

  // --- the catch-up (P8) -----------------------------------------------------------------------
  //
  // The device thread records the decoded capture into `catch_up_input_` while a two-step publish is
  // in flight; the catch-up thread drains that into a window of its own, replays it into the Full's
  // plans and then keeps them current until the render thread takes them over. Three threads, three
  // disjoint pieces of state: the ring is the only thing shared, and it is single-producer,
  // single-consumer.
  InputRing catch_up_input_;
  /// The catch-up thread. Started on the first catch-up and parked on a condition variable when
  /// there is none -- a daemon that never publishes with `lite_first` never has it.
  std::thread catch_up_thread_;
  mutable std::mutex catch_up_mutex_;
  std::condition_variable catch_up_wake_;
  bool catch_up_job_ready_ = false; ///< guarded by \ref catch_up_mutex_
  CatchUpJob catch_up_job_;         ///< guarded by \ref catch_up_mutex_
  std::atomic<bool> catch_up_quit_{false};
  /// The catch-up thread's own memory: the window it replays from and the scratch the replay writes
  /// its discarded output into. Touched by that thread alone.
  InputWindow catch_up_window_;
  std::vector<float> catch_up_scratch_;
  /// What became of the last catch-up. Control thread.
  CatchUpReport catch_up_report_;
  /// The Full of a two-step publish, being compiled and then caught up off this thread. Guarded by
  /// \ref catch_up_mutex_, which is also the catch-up thread's parking lock.
  AsyncFull async_full_;
  /// Which job the fields below belong to, and the outcome the catch-up thread published for it.
  std::atomic<uint64_t> catch_up_job_id_{0};
  std::atomic<uint64_t> catch_up_done_id_{0};
  std::atomic<uint64_t> catch_up_done_blocks_{0};
  std::atomic<uint64_t> catch_up_done_frames_{0};
  /// 0 none, 1 exact, 2 gap, 3 no input, 4 abandoned -- `CatchUpReport::replay`'s vocabulary, as a
  /// number the catch-up thread can publish without a lock.
  std::atomic<int> catch_up_done_outcome_{0};
  /// True from the moment the catch-up thread picks a job up until it has finished with it: what
  /// `engine.status` reports and what `arm_catch_up` refuses a second job against.
  std::atomic<bool> catch_up_busy_{false};
  /// Blocks the ring has dropped since the last arm: a hole in the history, which is what makes a
  /// replay inexact rather than merely late.
  std::atomic<uint64_t> catch_up_dropped_{0};

  // --- the catch-up thread's own progress, as `engine.status` reports it ------------------------
  //
  // The catch-up thread writes these as it goes; the control thread reads them when it builds a
  // status. All relaxed, all one writer: they are a flight recorder, not a handshake, and nothing on
  // the render path reads or writes any of them. They exist because a `no_input` replay on a board
  // with no debugger is otherwise indistinguishable between "the ring was never written", "the window
  // never covered" and "the window covered but the replay refused the block size".
  /// \ref CatchUpReport::progress: a pointer to one of `kCatchUpProgress`'s literals, written by the
  /// catch-up thread and read by the control thread. A pointer store is one relaxed store, and it
  /// cannot be a torn string the way a `std::string` shared across the two would be. Null is "no job
  /// has run".
  std::atomic<const char*> catch_up_progress_{nullptr};
  /// \ref CatchUpReport::fill, the same way: a pointer to one of `kCatchUpFill`'s literals.
  std::atomic<const char*> catch_up_fill_{nullptr};
  /// The window when the fill loop stopped: held blocks, the contiguous run at its end, and the
  /// blocks it needed. `need` above `contiguous` is a history with a hole in it; `need` above `held`
  /// is a history that never filled at all.
  std::atomic<uint64_t> catch_up_window_held_{0};
  std::atomic<uint64_t> catch_up_window_contiguous_{0};
  std::atomic<uint64_t> catch_up_window_blocks_{0};
  std::atomic<uint64_t> catch_up_window_need_{0};
  /// The ring's block size at the last arm (what a replay is aligned to) against what the device was
  /// rendering when the device thread last looked. A mismatch in the second is why a `no_input` has an
  /// empty ring behind it.
  std::atomic<int> catch_up_history_period_{0};
  std::atomic<int> catch_up_device_period_{0};
  /// Blocks the device thread refused to record since the last arm because `info.period_size` was not
  /// the period the history was sized for.
  std::atomic<uint64_t> catch_up_period_mismatch_{0};
  /// Which way the render thread's hand-over went, and at which block: the replay made the plan exact
  /// (`exact`), or its patience ran out (`abandoned`). Cumulative since the last arm, one relaxed
  /// increment on the render path -- which is two atomics per hand-over, not per block.
  std::atomic<uint64_t> catch_up_hold_exact_{0};
  std::atomic<uint64_t> catch_up_hold_abandoned_{0};
  std::atomic<uint64_t> catch_up_hold_exact_at_{0};
  std::atomic<uint64_t> catch_up_hold_abandoned_at_{0};
  /// The newest block the catch-up thread had fed, and executed, since the last arm: kept here rather
  /// than read back from the slots, because a slot stops being a catch-up slot the moment the render
  /// thread takes it over, and the report is read after that.
  std::atomic<uint64_t> catch_up_fed_seq_{0};
  std::atomic<uint64_t> catch_up_executed_seq_{0};

  /// \brief The warm document as the control thread sees it: what it was compiled from, what it cost,
  /// and the plans it owns until the warm thread retires them.
  struct WarmEntry
  {
    bool present = false;
    graph::Document document;
    graph::Document resolved;
    std::vector<PlanSlot*> slots;
    std::vector<assets::MissingAsset> missing;
    CostOutcome cost;
    std::string rig;
    std::string source;
    std::string name;
    std::string base_dir;
    uint64_t generation = 0;
    /// The document it was installed into, as last seen. Recomputed rather than trusted: a switch
    /// moves the content between documents and the entry follows it.
    int doc = -1;
  };
  WarmEntry warm_entry_;
  /// Where the warmed rig came from, in the same vocabulary as \ref current_source_.
  std::atomic<uint64_t> warm_cold_switches_{0};
  /// The failsafe's own state (2026-10-09), control thread only: the render load histogram as of the
  /// last check (so a window can be taken), how many times pressure has cooled the warm set, and until
  /// when a new warm is refused because of it.
  std::array<uint64_t, kLoadBuckets> pressure_histogram_{};
  bool pressure_histogram_valid_ = false;
  /// When the last publish or switch finished. The failsafe does not judge the seconds right after
  /// one: publishing a heavy rig costs a compile, a page-in and a fade, and T11's run showed the
  /// setup transient (one block at 215%) being read as "the warm set is costing too much", which
  /// cooled a warm document that had nothing to do with it.
  std::chrono::steady_clock::time_point last_publish_at_{};
  uint64_t warm_pressure_cools_ = 0;
  std::chrono::steady_clock::time_point warm_pressure_until_{};

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
  /// Of those, the ones that kept what the edit did not touch and faded only what it changed.
  std::atomic<uint64_t> local_transitions_{0};
  /// Local transitions that found a different plan playing than the one they were planned against, and
  /// were switched to at once instead. Never expected; counted so it is seen if it happens.
  std::atomic<uint64_t> local_base_mismatches_{0};
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
  /// \brief Trouble that is happening *now*, rather than trouble that has ever happened.
  ///
  /// The driver's xrun and error counters only go up, so read raw they say "this box has glitched
  /// since it booted", which is not a health indicator: it never clears, so a client's "is it well"
  /// light would stay amber for the rest of the session after one dropout. These are the same events
  /// through a leaky bucket -- fed by the deltas, drained every few hundred blocks -- so an issue can
  /// go away, which is what makes it worth showing (docs/ui-integration-plan.md §1.6).
  std::atomic<uint64_t> recent_xruns_{0};
  std::atomic<uint64_t> recent_render_errors_{0};
  std::atomic<unsigned> active_rate_{0};
  std::atomic<unsigned> active_period_{0};
  std::atomic<unsigned> active_capture_channels_{0};
  std::atomic<unsigned> active_playback_channels_{0};
  std::atomic<bool> memory_locked_{false};
  std::atomic<int> rt_policy_{0};
  std::atomic<int> rt_priority_{0};
  std::atomic<bool> rt_realtime_{false};
  /// \brief The render pool as it actually is, for `engine.status`.
  ///
  /// Written by the render thread when the pool starts and read by the control thread, so these are
  /// atomics rather than the pool itself: the pool's own state is the render thread's, and a status
  /// request may not walk a vector another thread is rebuilding. `render_cores_mask_` is a bit per
  /// CPU the participants were actually confined to, read back from the kernel.
  std::atomic<unsigned> render_threads_active_{1};
  std::atomic<uint64_t> render_cores_mask_{0};
  /// True when the pool is smaller than the configuration asked for, or could not start at all. The
  /// status reports it and the run is not a multi-core result.
  std::atomic<bool> render_pool_degraded_{false};
  std::atomic<int> active_format_{static_cast<int>(PcmFormat::Unknown)};

  /// Whether a probe folds one frame per period or every frame (Config::levels_first_frame).
  const bool levels_first_frame_;
  /// Whether a probe folds into private memory and publishes once per period
  /// (Config::levels_publish_period). Read once, like the two above: a plan's probes are made with it.
  const bool levels_publish_period_;
  /// \brief The same decision, as the probes want it.
  ///
  /// Declared **before** the probe arrays below because they are constructed with it in the
  /// initialiser list, so it has to be the earlier member for their construction to see its value.
  const level::Probe::Publish levels_publish_mode_;

  /// \brief The device's input and output levels, and each chain's contribution to the bus.
  ///
  /// Written by the render thread as it walks the samples it already has, read by
  /// \ref levels_json. Fixed-size arrays rather than vectors: the channel count a device will be
  /// opened with is bounded (\ref kMaxLevelChannels), and the render path may not grow anything.
  /// A chain's probe is indexed by chain, not by id, because the id is a string and this is the
  /// audio thread.
  std::array<level::Probe, kMaxLevelChannels> input_levels_;
  std::array<level::Probe, kMaxLevelChannels> output_levels_;
  std::array<level::Probe, kMaxChains> chain_levels_;

  /// \brief Add every device and chain window to its atomic fields, once per period. Render thread.
  ///
  /// The other half of \ref level::Probe::publish_window: with `levels_publish_period` the probes
  /// above accumulate privately all period and are published here, which is the only place the
  /// render thread writes their atomics. Called from \ref account_period, which the render loop
  /// already calls exactly once per period -- so "once per period" is structural, not a promise.
  void publish_level_windows() noexcept;

  /// \brief Whether this daemon measures levels at all (`Config::levels`, `level.h`).
  ///
  /// Const after construction, because it decides two things that cannot change while plans are
  /// running: whether a compiled plan has probes at all (they are made as the plan is built, and a
  /// local transition builds one on the render path), and whether `engine.levels` answers. A render
  /// thread reading it per block is reading a value that never changes.
  const bool levels_enabled_;

  /// \brief The held levels reply and when it was drained, for Config::levels_rate_hz.
  ///
  /// Control thread only, so a plain member rather than anything atomic. Empty until the first
  /// drain; `levels_taken_at_` is a steady-clock instant, not a wall one, because this is an
  /// interval and not a date.
  nlohmann::json levels_cache_;
  std::chrono::steady_clock::time_point levels_taken_at_{};
  bool levels_ever_taken_ = false;
  /// How many reads were answered from the cache and how many cost a drain, since start, plus every
  /// drain there has been -- including the control loop's own, which no read asked for. Three
  /// counters rather than two because they answer different questions: `hits`/`misses` say what a
  /// client's poll cost, and `drains` says whether the cadence is running at all. Reported in the
  /// reply itself, because "the rate was honoured" is a claim about the daemon and this is how it is
  /// checked rather than assumed.
  uint64_t levels_cache_hits_ = 0;
  uint64_t levels_cache_misses_ = 0;
  uint64_t levels_drains_ = 0;

  /// The backend's output-queue counters (StreamQueueStats), published per block. A dropped or an
  /// inserted period is a click that is not an xrun, so these belong in `engine.status` beside it.
  std::atomic<uint64_t> output_drops_{0};
  std::atomic<uint64_t> output_target_drops_{0};
  std::atomic<uint64_t> floor_insertions_{0};
  std::atomic<uint64_t> capture_bursts_{0};
  /// Render thread: the counters as of the last block, so a recording can be given the deltas.
  StreamQueueStats last_queue_{};

  /// \brief How long each block took, from the capture read returning to the playback write
  /// returning, as a share of the period's deadline.
  ///
  /// An xrun says the device noticed. This says how close the render came, which is what a soak's
  /// gate is written in: the worst block, and how many went past 90% and past 100% of the deadline.
  /// One bucket per percent; the last holds everything at or past the deadline. Cumulative for the
  /// daemon's life, so a poller takes deltas.
  std::array<std::atomic<uint64_t>, kLoadBuckets> load_histogram_{};
  std::atomic<uint64_t> worst_block_ns_{0};
  std::atomic<uint64_t> deadline_ns_{0};
  /// Render thread: file one block's time.
  void account_render_time(std::chrono::steady_clock::duration elapsed, const BackendInfo& info) noexcept;

  /// \brief The recording `engine.record` started, if any.
  ///
  /// The control thread owns it. The render thread sees it only through \ref recorder_live_, and
  /// holds \ref recorder_busy_ while it pushes, so \ref end_recording can take it away without a
  /// lock: clear the pointer, wait for busy to drop, then destroy.
  std::unique_ptr<Recorder> recorder_;
  std::string recording_name_;
  std::atomic<Recorder*> recorder_live_{nullptr};
  std::atomic<bool> recorder_busy_{false};
};

} // namespace daemon
} // namespace namchain
