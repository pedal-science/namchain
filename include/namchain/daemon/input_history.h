#pragma once

// The input history: what the device thread heard, kept long enough to replay a model's receptive
// field into a plan that has not been rendering. This is P8's catch-up
// (pedal.project docs/managing-processing-power-and-optimising-for-fast-switching.md) and the piece
// `docs/daemon.md` names as missing from the Lite-first publish: a cold instance starting mid-note is
// wrong for the length of its receptive field, and no hand-over buffer can fix that because a
// hand-over buffer *drops* what does not fit.
//
// Two buffers, because they have different owners:
//
//   * \ref InputRing -- written by the device thread on the render path, read by the catch-up thread.
//     One producer, one consumer, storage sized before either runs: no allocation, no lock and no
//     blocking on the render path. When the consumer is behind by the whole ring the producer **drops**
//     and counts it, exactly as `BlockPool` does -- the ring is a rolling buffer and not a queue, and a
//     drop is what makes a window discontiguous, which is what makes a replay inexact. Nothing ever
//     overwrites a block the consumer has not given back, so a reader cannot tear.
//
//   * \ref InputWindow -- the catch-up thread's own copy of the newest `frames` of that input, in whole
//     blocks. It is one thread's memory, so the replay reads it while the device keeps rendering with
//     no synchronisation at all, which is the property that makes running a plan beside the audible
//     one safe.
//
// Both store **planar** channel data (`channel * block_frames + frame`), which is what a plan's
// `execute` takes: the device thread decodes straight into that layout, the window copies a block with
// one `memcpy`, and the replay and the mirror hand a channel's plane to a plan with no copy at all.
//
// Only one thread is ever the ring's producer and only one is ever its consumer; the window belongs to
// the consumer alone.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace namchain
{
namespace daemon
{

/// \brief The device thread's rolling ring of decoded input blocks.
///
/// Sized once, by the control thread, before it is armed -- `configure` allocates, and nothing else
/// here does. Disarming never frees: a writer that is in the middle of a block finishes into storage
/// that is still there, and the control thread is free to leave the allocation in place for the next
/// catch-up (which is what makes a second `lite_first` press cost no allocation at all).
class InputRing
{
public:
  InputRing() = default;

  /// \brief Size the storage. Control thread, before \ref arm, and never while armed.
  ///
  /// \param block_frames the device's period: one block is one rendered period, which is also what
  ///        readds a block-delay node's state back exactly (its state lives in the plan's block
  ///        buffer, so a replay that is not block-aligned cannot restore it).
  /// \param channels the capture channels recorded, planar per channel.
  /// \param blocks how many blocks the ring holds: the window a replay needs, plus the slack that
  ///        covers the compile the ring is filling through.
  void configure(int block_frames, unsigned channels, std::size_t blocks);

  /// Start recording. The storage is already there: this is one store, so it may be called while the
  /// device thread is rendering.
  void arm() noexcept { armed_.store(true, std::memory_order_release); }
  /// Stop recording. Not a free: see \ref configure.
  void disarm() noexcept { armed_.store(false, std::memory_order_release); }
  bool armed() const noexcept { return armed_.load(std::memory_order_acquire); }

  bool configured() const noexcept { return block_frames_ != 0; }
  int block_frames() const noexcept { return block_frames_; }
  unsigned channels() const noexcept { return channels_; }
  std::size_t blocks() const noexcept { return blocks_; }
  std::size_t floats_per_block() const noexcept
  {
    return static_cast<std::size_t>(block_frames_) * channels_;
  }
  std::size_t capacity_frames() const noexcept { return blocks_ * static_cast<std::size_t>(block_frames_); }

  // --- the device thread (render path) -------------------------------------------------------

  /// \brief Storage for one block, planar, with \p seq recorded, or null when the ring is full or
  /// unarmed.
  ///
  /// A null return is the ring refusing rather than blocking, and the caller simply carries on: the
  /// block was not recorded, and \ref dropped counts it.
  float* acquire(uint64_t seq) noexcept
  {
    if (!armed_.load(std::memory_order_relaxed))
      return nullptr;
    const uint64_t head = head_.load(std::memory_order_relaxed);
    const uint64_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= blocks_)
    {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    const std::size_t slot = static_cast<std::size_t>(head) & mask_;
    seqs_[slot] = seq;
    return storage_.data() + slot * floats_per_block();
  }

  /// Hand the block written through \ref acquire to the consumer.
  void publish() noexcept
  {
    const uint64_t head = head_.load(std::memory_order_relaxed);
    newest_seq_.store(seqs_[static_cast<std::size_t>(head) & mask_], std::memory_order_relaxed);
    written_.fetch_add(1, std::memory_order_relaxed);
    head_.store(head + 1, std::memory_order_release);
  }

  // --- the catch-up thread -------------------------------------------------------------------

  /// The oldest unread block, or false. The storage stays valid until \ref give_back.
  bool take(const float*& data, uint64_t& seq) noexcept
  {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire))
      return false;
    const std::size_t slot = static_cast<std::size_t>(tail) & mask_;
    data = storage_.data() + slot * floats_per_block();
    seq = seqs_[slot];
    oldest_seq_.store(seq, std::memory_order_relaxed);
    return true;
  }

  void give_back() noexcept
  {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    consumed_.fetch_add(1, std::memory_order_relaxed);
    tail_.store(tail + 1, std::memory_order_release);
  }

  /// Whether the producer has published a block the consumer has not taken yet. A peek, for a
  /// consumer deciding whether it has reached the newest history or is merely behind it.
  bool has_data() const noexcept
  {
    return tail_.load(std::memory_order_relaxed) != head_.load(std::memory_order_acquire);
  }

  /// Blocks the producer could not record because the consumer was behind by the whole ring.
  uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  // --- what the ring has done, for `engine.status` ---------------------------------------------
  //
  // Five counters and two indices, read by the control thread while the device thread writes and the
  // catch-up thread drains. They are the difference between "the window never covered" and "the window
  // was never fed", which nothing else in this engine reports and which is the whole diagnosis on a
  // board with no debugger. Each is written by one thread, each read relaxed: they are a report, not
  // synchronisation, and none of them is on the render path's critical work (one increment beside the
  // store that was already there).
  //
  // Every counter is cumulative since \ref configure, so a client takes deltas across a press.

  /// Blocks the device thread has published. Zero means the producer never recorded anything, whatever
  /// the consumer did -- the first question to ask of a `no_input` replay.
  uint64_t written() const noexcept { return written_.load(std::memory_order_relaxed); }
  /// Blocks the catch-up thread has taken and given back.
  uint64_t consumed() const noexcept { return consumed_.load(std::memory_order_relaxed); }
  /// The block at the head (next to be written) and at the tail (oldest unread). Both monotonic:
  /// `head - tail` is the backlog the consumer has not drained.
  uint64_t head() const noexcept { return head_.load(std::memory_order_relaxed); }
  uint64_t tail() const noexcept { return tail_.load(std::memory_order_relaxed); }
  /// The `seq` of the newest block published, and of the oldest block taken. A `seq` that stands still
  /// while \ref written climbs is a device whose block index is frozen; one that climbs by more than
  /// one is a gap between what was written and what the plan will be told it rendered.
  uint64_t newest_seq() const noexcept { return newest_seq_.load(std::memory_order_relaxed); }
  uint64_t oldest_seq() const noexcept { return oldest_seq_.load(std::memory_order_relaxed); }

private:
  std::vector<float> storage_;
  std::vector<uint64_t> seqs_;
  int block_frames_ = 0;
  unsigned channels_ = 0;
  std::size_t blocks_ = 0;
  std::size_t mask_ = 0;
  std::atomic<bool> armed_{false};
  std::atomic<uint64_t> head_{0};
  std::atomic<uint64_t> tail_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> written_{0};
  std::atomic<uint64_t> consumed_{0};
  std::atomic<uint64_t> newest_seq_{0};
  std::atomic<uint64_t> oldest_seq_{0};
};

/// \brief The catch-up thread's rolling copy of the newest input, in whole blocks.
///
/// One owner, so nothing here is atomic: the catch-up thread appends as it takes blocks from the ring
/// and reads it back to replay. A window holds the newest `frames` frames; appending past that
/// overwrites the oldest block, which is what makes it a window rather than a log.
class InputWindow
{
public:
  InputWindow() = default;

  /// Size the storage. Control thread, before the catch-up thread may use it (which is "before the
  /// job is handed over").
  void configure(std::size_t frames, unsigned channels, int block_frames);

  bool configured() const noexcept { return block_frames_ > 0 && blocks_ > 0; }
  std::size_t frames() const noexcept { return blocks_ * static_cast<std::size_t>(block_frames_); }
  std::size_t blocks() const noexcept { return blocks_; }
  unsigned channels() const noexcept { return channels_; }
  int block_frames() const noexcept { return block_frames_; }

  void clear() noexcept
  {
    first_ = 0;
    held_ = 0;
    contiguous_ = 0;
  }

  /// Append one block, planar. Keeps the newest `frames` frames.
  void append(const float* planar, uint64_t seq) noexcept
  {
    if (!configured())
      return;
    if (held_ == blocks_)
    {
      // Full: the oldest block goes, and the run of contiguous blocks shrinks with it.
      first_ = (first_ + 1) % blocks_;
      --held_;
      if (contiguous_ > held_)
        contiguous_ = held_;
    }
    const std::size_t slot = (first_ + held_) % blocks_;
    std::copy(planar, planar + floats_per_block(), storage_.data() + slot * floats_per_block());
    seqs_[slot] = seq;
    // A block that does not follow the one before it is a hole the producer's drop left behind: the
    // run worth replaying restarts here. A hole *before* the window is harmless -- those frames are
    // gone from the window at the next appends.
    if (held_ > 0 && seq != seqs_[(slot + blocks_ - 1) % blocks_] + 1)
      contiguous_ = 1;
    else if (contiguous_ < held_ + 1)
      ++contiguous_;
    ++held_;
  }

  std::size_t held() const noexcept { return held_; }
  /// The blocks at the end of the window that are one after another, which is all a replay may use.
  std::size_t contiguous_blocks() const noexcept { return contiguous_; }
  uint64_t seq_of(std::size_t index) const noexcept
  {
    return index < held_ ? seqs_[(first_ + index) % blocks_] : 0;
  }
  uint64_t newest_seq() const noexcept { return held_ == 0 ? 0 : seq_of(held_ - 1); }

  /// Whether the newest \p frames frames are present and contiguous: the condition for replaying them
  /// and being able to say the state is exact afterwards.
  bool covers(std::size_t want) const noexcept
  {
    if (!configured() || want == 0)
      return want == 0;
    const std::size_t need_blocks = (want + static_cast<std::size_t>(block_frames_) - 1) /
                                    static_cast<std::size_t>(block_frames_);
    return held_ >= need_blocks && contiguous_ >= need_blocks;
  }

  /// One block's plane of one channel, or null. Block 0 is the oldest held.
  const float* block(std::size_t index, unsigned channel) const noexcept
  {
    if (index >= held_ || channel >= channels_)
      return nullptr;
    const std::size_t slot = (first_ + index) % blocks_;
    return storage_.data() + slot * floats_per_block() + static_cast<std::size_t>(channel) * block_frames_;
  }

  std::size_t floats_per_block() const noexcept
  {
    return static_cast<std::size_t>(block_frames_) * channels_;
  }

private:
  std::vector<float> storage_;
  std::vector<uint64_t> seqs_;
  unsigned channels_ = 0;
  int block_frames_ = 0;
  std::size_t blocks_ = 0; ///< slots, a power of two so the wrap is a mask
  std::size_t mask_ = 0;
  std::size_t first_ = 0; ///< the slot of the oldest held block
  std::size_t held_ = 0;
  /// The blocks at the end of the window that follow one another. Reset by a hole.
  std::size_t contiguous_ = 0;
};

} // namespace daemon
} // namespace namchain
