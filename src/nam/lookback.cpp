#include "namchain/lookback.h"

#include <fstream>
#include <sstream>
#include <vector>

#include <json.hpp>

namespace namchain
{
namespace
{

using json = nlohmann::json;

LookbackResult ok(long long samples, std::string detail)
{
  LookbackResult r;
  r.status = LookbackStatus::Ok;
  // The engine's sample indices are int-wide; a model needing more than 2^31 samples of
  // history (>12 h at 48 kHz) is nonsense and would overflow downstream arithmetic.
  if (samples < 0 || samples > 2147483647LL)
  {
    r.status = LookbackStatus::Malformed;
    r.detail = "receptive field out of representable range";
    return r;
  }
  r.samples = static_cast<int>(samples);
  r.detail = std::move(detail);
  return r;
}

LookbackResult fail(LookbackStatus status, std::string detail)
{
  LookbackResult r;
  r.status = status;
  r.detail = std::move(detail);
  return r;
}

LookbackResult compute_model(const json& model); // forward

/// Mirrors nam::wavenet::detail::Head::receptive_field():
///   rf = 1 + sum(kernel_size - 1) over the head's convolutions.
long long head_receptive_field(const json& head)
{
  long long rf = 1;
  if (head.contains("kernel_sizes") && head["kernel_sizes"].is_array())
  {
    for (const auto& k : head["kernel_sizes"])
      rf += k.get<long long>() - 1;
  }
  else if (head.contains("kernel_size") && head["kernel_size"].is_number_integer())
  {
    rf += head["kernel_size"].get<long long>() - 1;
  }
  return rf;
}

/// Mirrors WaveNet::mPrewarmSamples (NAM/wavenet/model.cpp:657):
///   base = condition_dsp ? condition_dsp->GetPrewarmSamples() : 1
///   for each layer array: += get_receptive_field()
///   if post-stack head:   += head->receptive_field() - 1
/// where LayerArray::get_receptive_field() (model.cpp:436) is
///   sum_i dilation_i * (kernel_i - 1) + head_rechannel_dilation * (head_kernel - 1)
LookbackResult compute_wavenet(const json& model, const json& cfg)
{
  long long total = 1;

  // condition_dsp may live on the model or inside its config depending on schema version.
  const json* cond = nullptr;
  if (model.contains("condition_dsp") && !model["condition_dsp"].is_null())
    cond = &model["condition_dsp"];
  else if (cfg.contains("condition_dsp") && !cfg["condition_dsp"].is_null())
    cond = &cfg["condition_dsp"];

  if (cond != nullptr)
  {
    // The core uses the condition DSP's prewarm count as the base value instead of 1,
    // because the conditioning signal itself is a stateful model that must be replayed.
    const LookbackResult c = compute_model(*cond);
    if (!c.ok())
      return c;
    total = c.samples;
  }

  if (!cfg.contains("layers") || !cfg["layers"].is_array() || cfg["layers"].empty())
    return fail(LookbackStatus::Malformed, "WaveNet config has no 'layers' array");

  for (const auto& la : cfg["layers"])
  {
    if (!la.contains("dilations") || !la["dilations"].is_array() || la["dilations"].empty())
      return fail(LookbackStatus::Malformed, "layer array missing a non-empty 'dilations' array");
    const auto& ds = la["dilations"];
    const size_t num_layers = ds.size();

    // Mirrors NAM/wavenet/model.cpp:1003-1039. Newer exports carry a per-layer
    // "kernel_sizes" array; legacy .nam files carry a single scalar "kernel_size"
    // replicated across every dilated layer. Providing both is an error.
    const bool has_kernel_size = la.contains("kernel_size");
    const bool has_kernel_sizes = la.contains("kernel_sizes");
    if (has_kernel_size && has_kernel_sizes)
      return fail(LookbackStatus::Malformed, "layer array has both 'kernel_size' and 'kernel_sizes'");

    std::vector<long long> ks;
    if (has_kernel_sizes)
    {
      if (!la["kernel_sizes"].is_array() || la["kernel_sizes"].size() != num_layers)
        return fail(LookbackStatus::Malformed, "'kernel_sizes' must be an array matching 'dilations'");
      for (const auto& k : la["kernel_sizes"])
      {
        if (!k.is_number_integer())
          return fail(LookbackStatus::Malformed, "'kernel_sizes' must be integers");
        ks.push_back(k.get<long long>());
      }
    }
    else if (has_kernel_size)
    {
      if (!la["kernel_size"].is_number_integer())
        return fail(LookbackStatus::Malformed, "'kernel_size' must be an integer");
      ks.assign(num_layers, la["kernel_size"].get<long long>());
    }
    else
    {
      return fail(LookbackStatus::Malformed, "layer array has neither 'kernel_size' nor 'kernel_sizes'");
    }

    for (size_t i = 0; i < num_layers; ++i)
    {
      if (!ds[i].is_number_integer())
        return fail(LookbackStatus::Malformed, "'dilations' must be integers");
      const long long k = ks[i];
      const long long d = ds[i].get<long long>();
      if (k < 1 || d < 1)
        return fail(LookbackStatus::Malformed, "kernel size and dilation must be >= 1");
      total += d * (k - 1);
    }

    // Layer-array head rechannel (causal Conv1D), mirrors model.cpp:964-993.
    //   nested "head"  -> kernel_size and optional head_dilation
    //   legacy         -> head_size + head_bias, with an implicit kernel of 1 (adds 0)
    if (la.contains("head") && !la["head"].is_null())
    {
      const auto& h = la["head"];
      if (!h.is_object() || !h.contains("kernel_size") || !h["kernel_size"].is_number_integer())
        return fail(LookbackStatus::Malformed, "layer array 'head' must be an object with 'kernel_size'");
      const long long k = h["kernel_size"].get<long long>();
      const long long d = h.value("head_dilation", 1);
      if (k < 1 || d < 1)
        return fail(LookbackStatus::Malformed, "head kernel size and dilation must be >= 1");
      total += d * (k - 1);
    }
    else if (!la.contains("head_size"))
    {
      return fail(LookbackStatus::Malformed, "layer array has neither 'head' nor legacy 'head_size'");
    }
  }

  // Post-stack head, if any.
  if (cfg.contains("head") && !cfg["head"].is_null() && cfg["head"].is_object())
    total += head_receptive_field(cfg["head"]) - 1;

  return ok(total, model.value("architecture", std::string("WaveNet")));
}

LookbackResult compute_slimmable_container(const json& model)
{
  const json& cfg = model.at("config");
  if (!cfg.contains("submodels") || !cfg["submodels"].is_array() || cfg["submodels"].empty())
    return fail(LookbackStatus::Malformed, "SlimmableContainer has no 'submodels'");

  long long worst = 0;
  for (const auto& entry : cfg["submodels"])
  {
    if (!entry.contains("model"))
      return fail(LookbackStatus::Malformed, "SlimmableContainer submodel entry missing 'model'");
    const LookbackResult sub = compute_model(entry["model"]);
    if (!sub.ok())
      return sub;
    if (sub.samples > worst)
      worst = sub.samples;
  }
  return ok(worst, "SlimmableContainer(max over submodels)");
}

/// nam::Linear (IR convolution). `config.receptive_field` is the IR length / tap count
/// (NAM/linear.cpp:427), and the core does not override GetPrewarmSamples(), so it
/// reports 0 even though an FIR needs all of its history replayed to be exact.
LookbackResult compute_linear(const json& model)
{
  const json& cfg = model.at("config");
  long long taps = 0;

  if (cfg.contains("receptive_field") && cfg["receptive_field"].is_number_integer())
  {
    taps = cfg["receptive_field"].get<long long>();
  }
  else if (cfg.contains("weights") && cfg["weights"].is_array() && !cfg["weights"].empty())
  {
    // weights = receptive_field (+1 trailing bias weight when bias is set)
    const long long n = static_cast<long long>(cfg["weights"].size());
    const long long in_ch = cfg.value("in_channels", 1);
    const long long out_ch = cfg.value("out_channels", 1);
    const long long denom = (in_ch > 0 && out_ch > 0) ? in_ch * out_ch : 1;
    const bool bias = cfg.value("bias", false);
    taps = n / denom - (bias ? 1 : 0);
  }
  else
  {
    return fail(LookbackStatus::Malformed, "Linear model has neither 'receptive_field' nor 'weights'");
  }

  if (taps < 1)
    return fail(LookbackStatus::Malformed, "Linear receptive field must be >= 1");
  return ok(taps, "Linear(IR taps; core reports 0)");
}

LookbackResult compute_model(const json& model)
{
  if (!model.is_object())
    return fail(LookbackStatus::Malformed, "model is not a JSON object");

  const std::string arch = model.value("architecture", std::string());
  if (arch.empty())
    return fail(LookbackStatus::Malformed, "missing 'architecture'");
  if (!model.contains("config") || !model["config"].is_object())
    return fail(LookbackStatus::Malformed, "missing 'config' object");

  if (arch == "WaveNet")
    return compute_wavenet(model, model["config"]);
  if (arch == "SlimmableWaveNet")
  {
    // The core hardcodes GetPrewarmSamples() == 0 for this class (NAM/wavenet/slimmable.h),
    // which is a limitation the engine must work around; compute it from the config instead.
    LookbackResult r = compute_wavenet(model, model["config"]);
    if (r.ok())
      r.detail = "SlimmableWaveNet(computed; core reports 0)";
    return r;
  }
  if (arch == "SlimmableContainer")
    return compute_slimmable_container(model);
  if (arch == "Linear")
    return compute_linear(model);
  if (arch == "LSTM")
    return fail(LookbackStatus::Unbounded, "LSTM has unbounded memory; replay cannot be exact");

  return fail(LookbackStatus::UnknownArchitecture, arch);
}

} // namespace

LookbackResult compute_lookback_from_json(const std::string& json_text)
{
  json doc;
  try
  {
    doc = json::parse(json_text);
  }
  catch (const std::exception& e)
  {
    return fail(LookbackStatus::Malformed, std::string("JSON parse error: ") + e.what());
  }
  try
  {
    return compute_model(doc);
  }
  catch (const std::exception& e)
  {
    // at()/get() on unexpected shapes throw; report rather than propagate.
    return fail(LookbackStatus::Malformed, std::string("schema error: ") + e.what());
  }
}

LookbackResult compute_lookback_from_file(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return fail(LookbackStatus::Malformed, "cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return compute_lookback_from_json(ss.str());
}

const char* to_string(LookbackStatus status)
{
  switch (status)
  {
  case LookbackStatus::Ok: return "ok";
  case LookbackStatus::Unbounded: return "unbounded";
  case LookbackStatus::UnknownArchitecture: return "unknown-architecture";
  case LookbackStatus::Malformed: return "malformed";
  }
  return "?";
}

} // namespace namchain
