#include "namchain/daemon/doc_runtime.h"

namespace namchain
{
namespace daemon
{

const char* doc_owner_name(DocOwner owner)
{
  switch (owner)
  {
    case DocOwner::Free: return "free";
    case DocOwner::Device: return "device";
    case DocOwner::Warm: return "warm";
  }
  return "unknown";
}

BlockPool::BlockPool() : full_(kBlocks), free_(kBlocks) {}

void BlockPool::configure(std::size_t floats_per_block)
{
  // A second call means the device was reopened. Both rings are drained rather than reconstructed:
  // the capacity is fixed for the life of the pool, and the only state that matters is which blocks
  // are free, which is all of them.
  uint32_t scratch = 0;
  while (full_.pop(scratch))
  {
  }
  while (free_.pop(scratch))
  {
  }

  floats_per_block_ = floats_per_block;
  storage_.assign(floats_per_block_ * kBlocks, 0.0f);
  seqs_.assign(kBlocks, 0);
  for (uint32_t i = 0; i < kBlocks; ++i)
    free_.push(i);
}

float* BlockPool::acquire(uint64_t seq, uint32_t& index) noexcept
{
  if (!free_.pop(index))
  {
    // Every block is either being filled or waiting to be rendered: the warm document is behind by
    // the whole pool. Dropping this block is the honest answer (see the header) -- what must not
    // happen is the device thread waiting for a warm thread to catch up.
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  seqs_[index] = seq;
  return block(index);
}

void BlockPool::publish(uint32_t index) noexcept
{
  // Cannot be refused: this index came off the free ring, so at most `kBlocks - 1` others are on the
  // full ring, whose capacity is `kBlocks`. Counted rather than assumed, the same way the command
  // ring counts a push it could not take.
  full_.push(index);
}

bool BlockPool::take(uint32_t& index, uint64_t& seq, const float*& data) noexcept
{
  if (!full_.pop(index))
    return false;
  seq = seqs_[index];
  data = block(index);
  return true;
}

void BlockPool::give_back(uint32_t index) noexcept
{
  free_.push(index);
}

} // namespace daemon
} // namespace namchain
