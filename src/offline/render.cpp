#include "namchain/offline.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"

#include "namchain/wav.h"

namespace namchain
{
namespace
{

OfflineRenderResult fail(std::string message)
{
  OfflineRenderResult r;
  r.ok = false;
  r.error = std::move(message);
  return r;
}

} // namespace

OfflineRenderResult render_model_to_wav(const std::string& model_path, const std::string& input_wav_path,
                                        const std::string& output_wav_path, const OfflineRenderOptions& options)
{
  if (options.quantum < 1 || options.quantum > 4096)
    return fail("quantum " + std::to_string(options.quantum) + " is out of range [1, 4096]");

  std::unique_ptr<nam::DSP> model;
  try
  {
    model = nam::get_dsp(std::filesystem::path(model_path));
  }
  catch (const std::exception& e)
  {
    return fail("failed to load model '" + model_path + "': " + e.what());
  }
  if (!model)
    return fail("failed to load model '" + model_path + "'");

  if (options.apply_slim)
  {
    if (options.slim_size < 0.0 || options.slim_size > 1.0)
      return fail("slim size must be in [0, 1]");
    auto* slimmable = dynamic_cast<nam::SlimmableModel*>(model.get());
    if (slimmable == nullptr)
      return fail("--slim requires a model implementing nam::SlimmableModel");
    slimmable->SetSlimmableSize(options.slim_size);
  }

  std::vector<float> input;
  double input_rate = 0.0;
  std::string wav_error;
  if (!load_wav_mono(input_wav_path, input, input_rate, wav_error))
    return fail(wav_error);

  const double expected_rate = model->GetExpectedSampleRate();
  if (expected_rate > 0.0 && std::abs(input_rate - expected_rate) > 0.5)
  {
    return fail("input WAV is " + std::to_string(static_cast<long long>(input_rate)) +
                " Hz but the model expects " + std::to_string(static_cast<long long>(expected_rate)) +
                " Hz; the core has no resampler, so resampling belongs to the I/O layer (plan §9.2)");
  }
  const double engine_rate = expected_rate > 0.0 ? expected_rate : input_rate;

  const int in_channels = model->NumInputChannels();
  const int out_channels = model->NumOutputChannels();
  if (in_channels != 1)
  {
    return fail("M0 offline render supports mono input only; this model wants " + std::to_string(in_channels) +
                " input channels");
  }
  if (out_channels < 1)
    return fail("model reports no output channels");

  try
  {
    model->Reset(engine_rate, options.quantum);
  }
  catch (const std::exception& e)
  {
    return fail(std::string("Reset failed: ") + e.what());
  }

  std::vector<std::vector<NAM_SAMPLE>> in_buffers(static_cast<size_t>(in_channels));
  std::vector<std::vector<NAM_SAMPLE>> out_buffers(static_cast<size_t>(out_channels));
  std::vector<NAM_SAMPLE*> in_ptrs(static_cast<size_t>(in_channels));
  std::vector<NAM_SAMPLE*> out_ptrs(static_cast<size_t>(out_channels));
  for (int ch = 0; ch < in_channels; ++ch)
  {
    in_buffers[static_cast<size_t>(ch)].assign(static_cast<size_t>(options.quantum), static_cast<NAM_SAMPLE>(0));
    in_ptrs[static_cast<size_t>(ch)] = in_buffers[static_cast<size_t>(ch)].data();
  }
  for (int ch = 0; ch < out_channels; ++ch)
  {
    out_buffers[static_cast<size_t>(ch)].assign(static_cast<size_t>(options.quantum), static_cast<NAM_SAMPLE>(0));
    out_ptrs[static_cast<size_t>(ch)] = out_buffers[static_cast<size_t>(ch)].data();
  }

  // Interleaved output. Frames out == frames in: no repeats and no insertions, which is the
  // property §10.3's gap/dropout metric checks on the engine path.
  std::vector<float> interleaved;
  interleaved.reserve(input.size() * static_cast<size_t>(out_channels));

  const size_t start = std::min<size_t>(static_cast<size_t>(options.start_frame), input.size());
  const size_t available = input.size() - start;
  const size_t frames_to_render =
    options.max_frames == 0 ? available : std::min<size_t>(available, static_cast<size_t>(options.max_frames));

  size_t processed = 0;
  while (processed < frames_to_render)
  {
    const int frames = static_cast<int>(
      std::min<size_t>(static_cast<size_t>(options.quantum), frames_to_render - processed));
    for (int i = 0; i < frames; ++i)
      in_buffers[0][static_cast<size_t>(i)] = static_cast<NAM_SAMPLE>(input[start + processed + static_cast<size_t>(i)]);

    model->process(in_ptrs.data(), out_ptrs.data(), frames);

    for (int i = 0; i < frames; ++i)
      for (int ch = 0; ch < out_channels; ++ch)
        interleaved.push_back(static_cast<float>(out_buffers[static_cast<size_t>(ch)][static_cast<size_t>(i)]));

    processed += static_cast<size_t>(frames);
  }

  std::string save_error;
  if (!save_wav_float32(output_wav_path, interleaved.data(), frames_to_render, out_channels, engine_rate, save_error))
    return fail(save_error);

  OfflineRenderResult result;
  result.ok = true;
  result.stats.frames = static_cast<int>(frames_to_render);
  result.stats.input_channels = in_channels;
  result.stats.output_channels = out_channels;
  result.stats.sample_rate = engine_rate;
  result.stats.quantum = options.quantum;
  return result;
}

} // namespace namchain
