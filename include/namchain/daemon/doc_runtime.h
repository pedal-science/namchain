#pragma once

// The render path's per-*document* state, and the handover protocol that lets one document change
// hands between two threads without a lock (docs/warm-set.md).
//
// Why this type exists at all: the engine renders **rigs**, and a rig is one document with up to
// `kMaxChains` chains that always render together on one core (D1). Before the warm set there was
// exactly one of those, so its state was the daemon's: one chain array, one bus, one set of
// generation counters. A warm document beside the audible one needs a second copy of that state, and
// naming the copy is the whole of the refactor -- the render path does the same things to a document
// it did before, and with one document and no warm thread it does them in the same order, which is
// what keeps "warmth off" bit-identical rather than merely similar.
//
// **Ownership.** A document is owned by exactly one thread at a time -- the device thread (the one
// that owns the device and renders what is audible) or a warm thread -- and only the owner executes
// its plans or writes its buffers. Ownership moves by a two-phase handover: the giver clears what it
// holds and stores `Free` with release ordering; the taker sees `Free`, claims it, and only then
// touches the plans. Nothing here blocks, allocates or locks, so both phases are render-path calls.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "namchain/rt/crossfade.h"
#include "namchain/rt/spsc_ring.h"

namespace namchain
{
namespace daemon
{

/// One chain's published plan and the arena its buffers live in, owned by \ref ChainRuntime while it
/// is playing and by the reclaimer on the way out. Defined in daemon.h; only the pointer is needed
/// here.
struct PlanSlot;

/// \brief The most chains one document may hold.
///
/// A fixed bound rather than a vector, because the render thread's per-chain state is a fixed-sized
/// array: nothing on the render path may allocate or resize, and rebuilding that state on a publish
/// would be exactly that. The limit is published to clients as part of the engine's limits.
inline constexpr int kMaxChains = 8;

/// \brief How many documents can exist at once: the audible one and the warm ones.
///
/// Two is what this slice needs -- one active, one warm, a switch between them a swap -- and the
/// array is sized for it rather than growing, for the same reason `kMaxChains` is: the render path
/// may not allocate. A bank that packs several warm rigs onto one core is a larger number here and a
/// policy above it, not a different mechanism.
inline constexpr int kMaxDocuments = 2;

/// The longest structural crossfade the weight table covers; a larger `fade_ms` is clamped to it.
/// 16384 frames is 341 ms at 48 kHz and 43 ms at 384 kHz, well past §5.3's 5 ms default.
inline constexpr int kMaxTransitionFrames = 1 << 14;

/// \brief Which thread is executing a document's plans right now.
///
/// `Free` is a real state and not a placeholder: it is the window between a giver letting go and a
/// taker picking up, and it is what makes the handover a protocol rather than a swap two threads
/// could perform at once. Nothing renders a document nobody owns.
enum class DocOwner : uint8_t
{
  Free = 0,
  Device = 1, ///< the thread that owns the device and renders the audible document
  Warm = 2,   ///< a warm thread, keeping it current for a switch
};

const char* doc_owner_name(DocOwner owner);

/// \brief Claim a free document for \p who. \return false when it was not free.
///
/// The acquire half of the handover: a successful claim happens-after the previous owner's release,
/// so everything it wrote to the document's plans and buffers is visible to the new owner.
inline bool claim_document(std::atomic<DocOwner>& owner, DocOwner who) noexcept
{
  DocOwner expected = DocOwner::Free;
  return owner.compare_exchange_strong(expected, who, std::memory_order_acq_rel, std::memory_order_acquire);
}

/// \brief Let go of a document, after its plans have stopped being touched.
///
/// The release half: the caller must have finished the block it was rendering and cleared what it
/// held, so a taker that sees `Free` cannot reach anything this thread is still using.
inline void release_document(std::atomic<DocOwner>& owner) noexcept
{
  owner.store(DocOwner::Free, std::memory_order_release);
}

/// \brief A pool of whole blocks passed from one thread to another, with no lock and no tearing.
///
/// The render path has two things to move between threads -- the decoded input the device thread
/// hands to a warm thread, and, during a document switch, the outgoing document's bus that a warm
/// thread hands back -- and both are "one block at a time, the newest one matters, falling behind must
/// be reported rather than waited for". So both use this: a fixed pool of blocks and two
/// single-producer rings. The producer fills a free block and publishes its index; the consumer pops
/// the index, reads it, and gives it back. A ring that is full means the consumer is behind by the
/// whole pool, and the producer **drops** that block and counts it -- which is the honest behaviour
/// for work whose consumer is not on a deadline: its state then has a gap, which is what "behind"
/// means, and nothing grows to hide it.
///
/// Fixed storage, sized when the device opens, so neither side allocates. Each pool has exactly one
/// producer and one consumer, both named by the caller; two warm threads share nothing.
class BlockPool
{
public:
  /// \brief How many blocks are in flight. Four is a block of catch-up in hand (about 5 ms at 64
  /// frames) while still being small enough that the warm document cannot drift far from the input
  /// being played.
  static constexpr unsigned kBlocks = 4;

  BlockPool();

  /// Control thread, before the clock: size the storage and put every block on the free ring.
  void configure(std::size_t floats_per_block);
  bool configured() const noexcept { return floats_per_block_ != 0; }

  /// Producer: a block to fill, with \p seq recorded as its sequence number, or null when the
  /// consumer is behind by the whole pool. The returned storage is writable until \ref publish.
  float* acquire(uint64_t seq) noexcept;

  /// Producer: hand the block acquired from \ref acquire to the consumer.
  void publish(uint32_t index) noexcept;

  /// Consumer: the oldest unread block, or false when there is none.
  bool take(uint32_t& index, uint64_t& seq, const float*& data) noexcept;

  /// Consumer: give a read block back.
  void give_back(uint32_t index) noexcept;

  /// Blocks the producer could not hand over because none was free.
  uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  std::size_t floats_per_block() const noexcept { return floats_per_block_; }
  const float* block(std::size_t index) const noexcept { return storage_.data() + index * floats_per_block_; }
  float* block(std::size_t index) noexcept { return storage_.data() + index * floats_per_block_; }
  uint64_t seq_of(std::size_t index) const noexcept { return seqs_[index]; }

private:
  std::vector<float> storage_;
  std::vector<uint64_t> seqs_;
  std::size_t floats_per_block_ = 0;
  rt::SpscRing<uint32_t> full_;
  rt::SpscRing<uint32_t> free_;
  std::atomic<uint64_t> dropped_{0};
};

/// \brief One chain's render state.
///
/// A plain member array of these is the whole reason for \ref kMaxChains: the render thread must
/// never allocate, and a per-chain container that could grow would be exactly that. Everything the
/// control thread reads for status is an atomic; everything else is the owning thread's.
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

/// \brief One document: its chains, its bus, and the counters a client reads.
///
/// The split between the two halves is the ownership rule. Everything above \ref owner is the owning
/// thread's and is never touched by another thread; everything below is atomic and is what
/// `engine.status` reads while the document is being rendered.
struct DocRuntime
{
  // --- the owning thread's state, and nobody else's ------------------------------------------
  ChainRuntime chains[kMaxChains];
  int chain_count = 0;
  /// The chain whose crossfade is in flight, or -1. One at a time, which is what bounds the peak
  /// cost of a document apply to two of the largest chain rather than two of the whole document.
  int transitioning_chain = -1;
  /// The document generation the owning thread has been told about, and the one before it. The pair
  /// is what lets `audible_generation` mean "the generation the device is hearing" while an apply is
  /// still working through the chains.
  uint64_t rendered_generation = 0;
  uint64_t previous_generation = 0;
  /// The playback bus, planar: one run of `period_size` floats per device playback channel. Every
  /// chain adds its output into its own channel's plane and the whole bus is encoded once, which is
  /// what makes two chains writing one channel sum instead of overwrite.
  std::vector<float> bus;
  /// The decoded capture block the owner last rendered, kept so the device thread has the outgoing
  /// side of a switch to mix against before the warm thread has published one.
  std::vector<float> published_bus;
  /// Where the outgoing side of a switch is published while one is running. The device thread sets
  /// \ref publish_wanted when it starts a fade; outside a fade nobody is reading and a document
  /// publishes nothing.
  BlockPool bus_pool;
  std::atomic<bool> publish_wanted{false};

  // --- ownership ------------------------------------------------------------------------------
  std::atomic<DocOwner> owner{DocOwner::Free};
  /// Which compiled content the document holds: the control thread's generation for it. Written by
  /// the owner when it takes content up, read by the control thread to know what it is looking at,
  /// and checked by the device thread so a switch cannot land on content that has been replaced.
  std::atomic<uint64_t> content{0};

  // --- counters, for `engine.status` -----------------------------------------------------------
  /// Blocks rendered by whichever thread owned the document, and how many of them were rendered
  /// warm. `blocks == 0` on a warmed document is "warming": compiled, not yet rendering.
  std::atomic<uint64_t> blocks{0};
  std::atomic<uint64_t> warm_blocks{0};
  /// The input block sequence the owner last rendered. The device thread's own sequence minus this
  /// is how far behind the document is; a gauge, not a total.
  std::atomic<uint64_t> input_seq{0};
  /// The worst \ref input_seq gap ever seen, and the input blocks the device thread dropped because
  /// no block was free (the warm side was behind by the whole pool).
  std::atomic<uint64_t> worst_behind{0};
  std::atomic<uint64_t> drops{0};
  /// Fade blocks during which the outgoing document had published nothing new, so the device thread
  /// held the block it already had. Counted, because it is the one way a switch can sound cheap.
  std::atomic<uint64_t> holds{0};
  /// Blocks the last switch into this document took, from the request to the fade closing.
  std::atomic<uint64_t> handover_blocks{0};
  /// When this document first rendered a block, as a steady-clock nanosecond count: the difference
  /// between "compiled" and "warm" is exactly whether this is set.
  std::atomic<uint64_t> first_block_ns{0};

  // --- document-level transition state, derived from the per-chain fields ----------------------
  std::atomic<bool> transition_active{false};
  std::atomic<uint64_t> audible_generation{0};
  std::atomic<uint64_t> target_generation{0};
  std::atomic<uint64_t> adopted_generation{0};

  /// Drop every plan this document holds, without freeing anything: the caller is a render thread,
  /// and what it drops goes to a reclaimer (or to the control thread, when nothing owns the
  /// document). Used by the release half of a handover.
  void forget_chains() noexcept
  {
    for (int i = 0; i < kMaxChains; ++i)
    {
      chains[i].current = nullptr;
      chains[i].next = nullptr;
      chains[i].pending = nullptr;
      chains[i].local_remaining = 0;
    }
    chain_count = 0;
    transitioning_chain = -1;
  }
};

} // namespace daemon
} // namespace namchain
