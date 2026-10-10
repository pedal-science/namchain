#pragma once

// P8's catch-up: how much input a document's models need replayed into a plan that has not been
// rendering, and the replay itself (pedal.project
// docs/managing-processing-power-and-optimising-for-fast-switching.md, "P8"; the engine's own
// docs/daemon.md, "Lite-first publishes").
//
// The two halves are deliberately separate from the daemon:
//
//   * \ref catch_up_plan reads a **resolved** document and asks `nam/lookback.cpp` what each node's
//     memory is. `Ok` gives a finite, exact length; `Unbounded` (an LSTM) means no replay can be
//     exact, and the honest answer is to refuse to promise -- reported, never silently replaced by a
//     cold Full. Anything the engine cannot size exactly (a biquad's IIR state, a delay whose time
//     may have moved) withholds the promise the same way, naming the node.
//   * \ref replay_window runs a plan over whole blocks of an \ref InputWindow, oldest first, and
//     discards the output: after it, the plan's state is that of a plan that has rendered that input
//     continuously, which is the property the crossfade to the Full then relies on.
//
// Nothing here allocates on a render thread: `catch_up_plan` is a control-thread walk of a document
// and its asset files, and `replay_window` writes only into `scratch`, which the caller owns.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "namchain/daemon/cost_table.h"
#include "namchain/daemon/input_history.h"
#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"

namespace namchain
{
namespace daemon
{

/// \brief The longest replay the engine will promise, in engine frames.
///
/// 131072 frames is 2.7 s at 48 kHz and 0.7 s at 192 kHz. It bounds the history the render path
/// records (twice this in the ring, so a few hundred KiB) and the work one catch-up does; a model
/// whose receptive field is longer than this is reported as `too_long` rather than approximated.
inline constexpr std::size_t kMaxCatchUpFrames = 1u << 17;

/// \brief What the ring holds *beside* the window, in frames.
///
/// The ring is what covers the compile: the window is filled from it when the plans are ready, so it
/// has to hold the window plus however long the compile takes. A second at 48 kHz is several times a
/// Full's compile on the pedal, and when a compile does outrun it the catch-up waits for one fresh
/// window (reported, not hidden) rather than promising a state it does not have.
inline constexpr std::size_t kCatchUpSlackFrames = 48000;

/// \brief How long the render thread holds a Full that is being caught up before it gives up and
/// crossfades it in anyway, in frames.
///
/// A quarter of a second at 48 kHz: long enough for a replay and the handful of blocks it takes the
/// catch-up thread to be current, short enough that a press is never left on the Lite for a
/// noticeable time. The plan is taken over whatever state it is in at that point, and the reply says
/// the catch-up was abandoned (`full.catch_up.state`).
inline constexpr std::size_t kCatchUpHoldFrames = 12000;

/// One chain's share of a catch-up.
struct CatchUpChain
{
  std::string id;
  /// Engine frames of input that must be replayed into this chain for its state to be exact.
  std::size_t frames = 0;
  /// False when some node in the chain has memory this cannot size exactly.
  bool exact = true;
  /// Which node, and why, when \ref exact is false.
  std::string detail;
};

/// \brief What a document's catch-up is, before anything is published.
struct CatchUpPlan
{
  /// \brief One of:
  ///
  ///   * `"exact"`      -- every chain's memory is finite and sizeable; \ref replayable is true.
  ///   * `"empty"`      -- nothing in the document has any memory to replay (or it is empty).
  ///   * `"unbounded"`  -- a model says its memory cannot be replayed (an LSTM). No replay is done
  ///                       and no exactness is promised; the Full is published cold and says so.
  ///   * `"unknown"`    -- a model or a node whose memory this cannot size (an architecture the
  ///                       calculator does not know, a file it cannot read, an IR with no tap count).
  ///   * `"too_long"`   -- a finite memory longer than \ref kMaxCatchUpFrames.
  ///   * `"inexact"`    -- the models are sizeable but some other node's state cannot be restored
  ///                       exactly by a replay (an IIR biquad, a delay whose time may have moved).
  ///                       The replay still runs -- the models are then exact -- and the promise is
  ///                       withheld with the node named.
  std::string state = "empty";
  std::string detail;
  std::vector<CatchUpChain> chains;
  /// The most any one chain needs: what the window has to hold.
  std::size_t window_frames = 0;
  /// Whether the daemon should record input and replay it before the Full is crossfaded in.
  bool replayable = false;
};

/// \brief Work out what a resolved document's catch-up is.
///
/// \param resolved a document whose model and IR references have already been resolved to paths
///        (`assets::resolve_document`), because that is what the compile will load.
/// \param engine_rate the rate the engine renders at, for scaling a model's own rate.
/// \param max_quantum the block size plans are compiled with, which is what an IR's tap count is
///        truncated against.
/// \param units the daemon's unit cache, used for a cab IR's tap count (a `.wav` cab has no `.nam`
///        lookback); null asks the lookback calculator alone.
CatchUpPlan catch_up_plan(const graph::Document& resolved, double engine_rate, int max_quantum,
                          UnitCache* units = nullptr);

/// What one \ref replay_window call did.
struct ReplayResult
{
  std::size_t blocks = 0;
  std::size_t frames = 0;
  /// The input sequence of the last block replayed: the plan's state is "as of" this block.
  uint64_t last_seq = 0;
  /// Whether the window held the whole requested length, contiguously. False means the replay is
  /// short of what the model needs and the state is not promised to be exact.
  bool covered = false;
};

/// \brief Replay the newest whole blocks of \p window into \p plan, oldest first, discarding output.
///
/// \param channel the capture channel the chain reads.
/// \param frames how many frames of input the chain needs.
/// \param scratch at least `plan.max_quantum()` floats: the replay's output goes nowhere.
ReplayResult replay_window(graph::RenderPlan& plan, const InputWindow& window, unsigned channel,
                           std::size_t frames, float* scratch);

} // namespace daemon
} // namespace namchain
