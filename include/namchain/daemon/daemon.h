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
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "namchain/assets/registry.h"
#include "namchain/daemon/config.h"
#include "namchain/daemon/framing.h"
#include "namchain/daemon/model_budget.h"
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
};

/// \brief The most chains one document may hold.
///
/// A fixed bound rather than a vector, because the render thread's per-chain state is a fixed-sized
/// array: nothing on the render path may allocate or resize, and rebuilding that state on a publish
/// would be exactly that. The limit is published to clients as part of the engine's limits.
inline constexpr int kMaxChains = 8;

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
///
/// Since slice C there is a second transport. Both carry the same JSON bodies; they differ in framing
/// (a length prefix, because TCP is a stream) and in trust (a token, because TCP has no filesystem
/// permissions to offer). See docs/daemon.md §"Two transports" and its threat model.
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
  /// Compile \p rig_path and publish it. Called for the startup rig, SIGHUP and `rig.load`.
  /// \param admit_over_budget passed on to \ref publish_document.
  bool load_rig(const std::string& rig_path, std::string& error, std::string* error_code = nullptr,
                bool admit_over_budget = false);
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
  /// document's models do not fit the model budget, which is `over_budget`.
  /// \param admit_over_budget publish a document that is over the model budget even with every
  /// slimmable model slimmed, and report it as a `model_budget_exceeded` warning, rather than refuse
  /// it. For the publishes nobody can be told "no" about: the configured rig at startup and on SIGHUP,
  /// and the running document again after a library rescan (docs/daemon.md, "Model budget").
  bool publish_document(const graph::Document& document, const std::string& path, const std::string& source,
                        const std::string& base_dir, std::string& error,
                        std::string* error_code = nullptr, bool admit_over_budget = false);
  /// \brief Resolve a `set_param` against a chain's newest published plan and push it on the ring.
  ///
  /// \param chain_id the chain to resolve against, or empty to resolve by node id -- which is an error
  /// if more than one chain holds that node, because guessing would edit an arbitrary chain.
  /// \param port the port a per-port parameter belongs to (a mixer's per-input level, mute, phase), or
  /// -1 for a parameter of the node itself.
  bool publish_param(const std::string& chain_id, const std::string& node, int port, const std::string& param,
                     double value, std::string& error);
  bool push_slot(uint32_t chain, uint32_t chain_count, PlanSlot* slot);
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
  void drain_commands();
  /// After a period is written: publish the backend's queue counters, and hand the period to the
  /// recording if there is one. Render thread.
  void account_period();
  /// Begin an equal-power transition for one chain. Render thread.
  void start_transition(int index, PlanSlot* next);
  /// Retire the faded-out plan, promote the incoming one, and start whatever is queued. Render thread.
  void finish_transition(int index);
  /// Take over a chain with a local transition's first plan: copy the carried buffers across and swap,
  /// with no whole-chain fade. Render thread.
  void start_local_transition(int index, PlanSlot* slot);
  /// The local transition's crossfades have closed: hand over to its final plan. Render thread.
  void finish_local_transition(int index);
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
    /// Frames left before a local transition's final plan takes over from \ref current; 0 when no
    /// local transition is running.
    int local_remaining = 0;
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
  /// What the model budget made of \ref current_document_: every model's estimate, which ones were
  /// slimmed, and whether the document was admitted over the budget (docs/daemon.md, "Model budget").
  BudgetOutcome model_budget_;
  /// The estimates the budget reads, by file. Control thread.
  WorkingSetCache working_sets_;
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
  /// The driver's xrun counter as of the last block, so the recent-trouble bucket can be fed the
  /// delta rather than the total.
  uint64_t last_xruns_ = 0;
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
  static constexpr std::size_t kLoadBuckets = 101;
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
