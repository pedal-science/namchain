// Tests for the render-path RT-safety instrument.
//
// The instrument has to be trustworthy before anything is judged by it, so it is tested both ways:
// a deliberate allocation inside the scope must be caught (otherwise a clean result means nothing),
// and a real model's process() must come back clean.
#include <doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include "namchain/rt_check.h"

namespace
{

std::string model_path(const char* name)
{
  return std::string(NAMCHAIN_TEST_DATA_DIR) + "/" + name;
}

/// Drive a model's process() inside an RtScope and report what the interposer saw.
struct ProcessAllocationResult
{
  uint64_t allocations = 0;
  uint64_t deallocations = 0;
};

ProcessAllocationResult measure_process(const std::string& path, int max_buffer, int iterations)
{
  std::unique_ptr<nam::DSP> model = nam::get_dsp(std::filesystem::path(path));
  REQUIRE(model != nullptr);
  model->Reset(48000.0, max_buffer);

  const int in_channels = model->NumInputChannels();
  const int out_channels = model->NumOutputChannels();
  std::vector<std::vector<NAM_SAMPLE>> in(static_cast<size_t>(in_channels));
  std::vector<std::vector<NAM_SAMPLE>> out(static_cast<size_t>(out_channels));
  std::vector<NAM_SAMPLE*> in_ptrs(static_cast<size_t>(in_channels));
  std::vector<NAM_SAMPLE*> out_ptrs(static_cast<size_t>(out_channels));
  for (int ch = 0; ch < in_channels; ++ch)
  {
    in[static_cast<size_t>(ch)].assign(static_cast<size_t>(max_buffer), static_cast<NAM_SAMPLE>(0));
    in_ptrs[static_cast<size_t>(ch)] = in[static_cast<size_t>(ch)].data();
  }
  for (int ch = 0; ch < out_channels; ++ch)
  {
    out[static_cast<size_t>(ch)].assign(static_cast<size_t>(max_buffer), static_cast<NAM_SAMPLE>(0));
    out_ptrs[static_cast<size_t>(ch)] = out[static_cast<size_t>(ch)].data();
  }

  // Warm up outside the scope. Upstream's contract is that process() is allocation-free *after*
  // Reset(); anything allocated lazily on the very first call belongs to setup, not to the render
  // path, and the engine pre-touches at plan build for exactly that reason (§3.3).
  model->process(in_ptrs.data(), out_ptrs.data(), max_buffer);

  namchain::rt_reset_counters();
  {
    namchain::RtScope scope;
    for (int i = 0; i < iterations; ++i)
    {
      // Sweep the buffer sizes upstream's own RT-safety test covers.
      for (int frames = 1; frames <= max_buffer; frames = (frames < 16 ? frames + 1 : frames * 2))
        model->process(in_ptrs.data(), out_ptrs.data(), frames);
    }
  }

  ProcessAllocationResult result;
  result.allocations = namchain::rt_allocation_count();
  result.deallocations = namchain::rt_deallocation_count();
  return result;
}

} // namespace

TEST_CASE("the allocation interposer is compiled in and starts clean")
{
  CHECK(namchain::rt_check_available());
  namchain::rt_reset_counters();
  CHECK(namchain::rt_allocation_count() == 0);
  CHECK(namchain::rt_deallocation_count() == 0);
  CHECK_FALSE(namchain::rt_in_scope());
}

TEST_CASE("allocations inside the scope are counted and outside are ignored")
{
  namchain::rt_reset_counters();

  // Outside: must not be attributed to the render path.
  {
    std::vector<int> scratch;
    scratch.reserve(4096);
  }
  CHECK(namchain::rt_allocation_count() == 0);
  CHECK(namchain::rt_deallocation_count() == 0);

  // Inside: the allocation must be caught, or a clean result would prove nothing.
  {
    namchain::RtScope scope;
    CHECK(namchain::rt_in_scope());
    {
      std::vector<int> scratch;
      scratch.reserve(4096);
      CHECK(namchain::rt_allocation_count() >= 1);
      CHECK(namchain::rt_deallocation_count() == 0); // still alive
    }
    // Destroyed while the scope is still active (reverse construction order), so the free is
    // render-path work too -- and free-on-the-render-thread is the same class of mistake.
    CHECK(namchain::rt_deallocation_count() >= 1);
  }

  CHECK_FALSE(namchain::rt_in_scope());
}

TEST_CASE("render-path scopes nest")
{
  namchain::rt_reset_counters();
  {
    namchain::RtScope outer;
    CHECK(namchain::rt_in_scope());
    {
      namchain::RtScope inner;
      CHECK(namchain::rt_in_scope());
    }
    CHECK(namchain::rt_in_scope()); // still inside the outer scope
  }
  CHECK_FALSE(namchain::rt_in_scope());
}

TEST_CASE("A2 process() is allocation-free on the render path, including the planar kernels")
{
  // This is the check that matters for the code we adopted in vendor/patches/0001: upstream's
  // allocation test only ever covered the generic a2_fast path, and the planar NEON kernels are
  // ours. On an AArch64 host the factory routes here, so this exercises them for real.
  for (const char* name : {"A2-Full.nam", "A2-Lite.nam"})
  {
    const ProcessAllocationResult result = measure_process(model_path(name), 256, 4);
    INFO("model = ", name);
    CHECK(result.allocations == 0);
    CHECK(result.deallocations == 0);
  }
}

TEST_CASE("the IR path is allocation-free too")
{
  const ProcessAllocationResult result = measure_process(model_path("CabIR.nam"), 256, 4);
  CHECK(result.allocations == 0);
  CHECK(result.deallocations == 0);
}
