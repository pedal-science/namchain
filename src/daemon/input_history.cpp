#include "namchain/daemon/input_history.h"

#include <algorithm>

namespace namchain
{
namespace daemon
{

void InputRing::configure(int block_frames, unsigned channels, std::size_t blocks)
{
  block_frames_ = block_frames > 0 ? block_frames : 0;
  channels_ = channels;
  // A power of two so both sides index with a mask: `%` on the render path is a division, and this
  // ring is written once per rendered block for the length of a compile.
  std::size_t rounded = 1;
  while (rounded < blocks)
    rounded <<= 1;
  blocks_ = rounded;
  mask_ = rounded - 1;
  seqs_.assign(blocks_, 0);
  storage_.assign(blocks_ * floats_per_block(), 0.0f);
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  dropped_.store(0, std::memory_order_relaxed);
  written_.store(0, std::memory_order_relaxed);
  consumed_.store(0, std::memory_order_relaxed);
  newest_seq_.store(0, std::memory_order_relaxed);
  oldest_seq_.store(0, std::memory_order_relaxed);
}

void InputWindow::configure(std::size_t frames, unsigned channels, int block_frames)
{
  channels_ = channels;
  block_frames_ = block_frames > 0 ? block_frames : 0;
  const std::size_t per_block = floats_per_block();
  std::size_t want = per_block == 0 ? 0 : (frames + static_cast<std::size_t>(block_frames_) - 1) /
                                             static_cast<std::size_t>(block_frames_);
  std::size_t rounded = 1;
  while (rounded < want)
    rounded <<= 1;
  blocks_ = rounded;
  mask_ = rounded - 1;
  storage_.assign(blocks_ * per_block, 0.0f);
  seqs_.assign(blocks_, 0);
  first_ = 0;
  held_ = 0;
  contiguous_ = 0;
}

} // namespace daemon
} // namespace namchain
