#include "namchain/working_set.h"

#include <fstream>
#include <sstream>
#include <vector>

#include <json.hpp>

namespace namchain
{
namespace
{

using json = nlohmann::json;

constexpr long long kFloat = static_cast<long long>(sizeof(float));
/// Larger than any model this engine could hold in memory, and far from overflowing a long long when
/// added up: a config claiming more is nonsense, not a big model.
constexpr long long kLimit = 1LL << 40;

/// Bytes, or -1 with `why` set. Every architecture below reports this way, so a failure deep in a
/// container's submodel comes back as the reason, not a crash.
struct Bytes
{
  long long value = -1;
  std::string why;
};

Bytes fail(std::string why)
{
  return {-1, std::move(why)};
}

Bytes model_bytes(const json& model, int q); // forward

long long weight_bytes(const json& model)
{
  if (model.contains("weights") && model["weights"].is_array())
    return static_cast<long long>(model["weights"].size()) * kFloat;
  return 0;
}

/// Per channel of a convolution's input, what a block reads: `lookback` frames behind the block and
/// the block's own `q`.
long long history_bytes(long long channels, long long lookback, int q)
{
  return channels * (lookback + q) * kFloat;
}

/// The same walk as compute_wavenet() in lookback.cpp, counting bytes instead of samples. Each dilated
/// layer reads `dilation * (kernel - 1)` frames of history across `channels`; a layer array's head
/// rechannel does the same across its input, the bottleneck. A post-stack head reads its receptive
/// field across the last layer array's head width.
Bytes wavenet_bytes(const json& model, const json& cfg, int q)
{
  long long total = weight_bytes(model);

  const json* cond = nullptr;
  if (model.contains("condition_dsp") && !model["condition_dsp"].is_null())
    cond = &model["condition_dsp"];
  else if (cfg.contains("condition_dsp") && !cfg["condition_dsp"].is_null())
    cond = &cfg["condition_dsp"];
  if (cond != nullptr)
  {
    const Bytes c = model_bytes(*cond, q);
    if (c.value < 0)
      return c;
    total += c.value;
  }

  if (!cfg.contains("layers") || !cfg["layers"].is_array() || cfg["layers"].empty())
    return fail("WaveNet config has no 'layers' array");

  long long last_head_width = 1;
  for (const auto& la : cfg["layers"])
  {
    if (!la.contains("channels") || !la["channels"].is_number_integer())
      return fail("layer array missing an integer 'channels'");
    const long long channels = la["channels"].get<long long>();
    const long long bottleneck =
      la.contains("bottleneck") && la["bottleneck"].is_number_integer() ? la["bottleneck"].get<long long>() : channels;
    if (channels < 1 || bottleneck < 1)
      return fail("'channels' and 'bottleneck' must be >= 1");

    if (!la.contains("dilations") || !la["dilations"].is_array() || la["dilations"].empty())
      return fail("layer array missing a non-empty 'dilations' array");
    const auto& ds = la["dilations"];
    const std::size_t num_layers = ds.size();

    const bool has_kernel_size = la.contains("kernel_size");
    const bool has_kernel_sizes = la.contains("kernel_sizes");
    if (has_kernel_size && has_kernel_sizes)
      return fail("layer array has both 'kernel_size' and 'kernel_sizes'");
    std::vector<long long> ks;
    if (has_kernel_sizes)
    {
      if (!la["kernel_sizes"].is_array() || la["kernel_sizes"].size() != num_layers)
        return fail("'kernel_sizes' must be an array matching 'dilations'");
      for (const auto& k : la["kernel_sizes"])
      {
        if (!k.is_number_integer())
          return fail("'kernel_sizes' must be integers");
        ks.push_back(k.get<long long>());
      }
    }
    else if (has_kernel_size && la["kernel_size"].is_number_integer())
    {
      ks.assign(num_layers, la["kernel_size"].get<long long>());
    }
    else
    {
      return fail("layer array has no integer 'kernel_size' or 'kernel_sizes'");
    }

    for (std::size_t i = 0; i < num_layers; ++i)
    {
      if (!ds[i].is_number_integer())
        return fail("'dilations' must be integers");
      const long long k = ks[i];
      const long long d = ds[i].get<long long>();
      if (k < 1 || d < 1 || k > kLimit / d)
        return fail("kernel size and dilation must be >= 1");
      total += history_bytes(channels, d * (k - 1), q);
      if (total > kLimit)
        return fail("working set out of range");
    }

    if (la.contains("head") && !la["head"].is_null())
    {
      const auto& h = la["head"];
      if (!h.is_object() || !h.contains("kernel_size") || !h["kernel_size"].is_number_integer())
        return fail("layer array 'head' must be an object with 'kernel_size'");
      const long long k = h["kernel_size"].get<long long>();
      const long long d = h.value("head_dilation", 1LL);
      if (k < 1 || d < 1 || k > kLimit / d)
        return fail("head kernel size and dilation must be >= 1");
      total += history_bytes(bottleneck, d * (k - 1), q);
      last_head_width = h.value("out_channels", 1LL);
    }
    else if (la.contains("head_size") && la["head_size"].is_number_integer())
    {
      // Legacy: a kernel-1 rechannel, which reads only the block.
      total += history_bytes(bottleneck, 0, q);
      last_head_width = la["head_size"].get<long long>();
    }
    else
    {
      return fail("layer array has neither 'head' nor legacy 'head_size'");
    }
  }

  if (cfg.contains("head") && cfg["head"].is_object())
  {
    const json& head = cfg["head"];
    long long lookback = 0;
    if (head.contains("kernel_sizes") && head["kernel_sizes"].is_array())
    {
      for (const auto& k : head["kernel_sizes"])
        lookback += k.get<long long>() - 1;
    }
    else if (head.contains("kernel_size") && head["kernel_size"].is_number_integer())
    {
      lookback += head["kernel_size"].get<long long>() - 1;
    }
    if (lookback < 0 || last_head_width < 1)
      return fail("post-stack head is malformed");
    total += history_bytes(last_head_width, lookback, q);
  }

  if (total > kLimit)
    return fail("working set out of range");
  return {total, {}};
}

/// nam::convnet: kernel-2 dilated convolutions, the first reading the one input channel and the rest
/// reading `channels`, then a kernel-1 head.
Bytes convnet_bytes(const json& model, const json& cfg, int q)
{
  if (!cfg.contains("channels") || !cfg["channels"].is_number_integer() || cfg["channels"].get<long long>() < 1)
    return fail("ConvNet config has no 'channels'");
  if (!cfg.contains("dilations") || !cfg["dilations"].is_array() || cfg["dilations"].empty())
    return fail("ConvNet config has no 'dilations'");
  const long long channels = cfg["channels"].get<long long>();
  long long total = weight_bytes(model);
  bool first = true;
  for (const auto& d : cfg["dilations"])
  {
    if (!d.is_number_integer() || d.get<long long>() < 1 || d.get<long long>() > kLimit)
      return fail("'dilations' must be integers >= 1");
    total += history_bytes(first ? 1 : channels, d.get<long long>(), q);
    first = false;
  }
  total += history_bytes(channels, 0, q);
  if (total > kLimit)
    return fail("working set out of range");
  return {total, {}};
}

/// nam::Linear (an IR as a model): its taps, and that many frames of input history plus the block.
Bytes linear_bytes(const json& model, const json& cfg, int q)
{
  long long taps = 0;
  if (cfg.contains("receptive_field") && cfg["receptive_field"].is_number_integer())
    taps = cfg["receptive_field"].get<long long>();
  else
    taps = weight_bytes(model) / kFloat;
  if (taps < 1 || taps > kLimit)
    return fail("Linear receptive field must be >= 1");
  return {weight_bytes(model) + history_bytes(1, taps, q), {}};
}

/// nam::lstm: its weights, and per layer a hidden and a cell state and the four gates' work.
Bytes lstm_bytes(const json& model, const json& cfg)
{
  const long long hidden = cfg.value("hidden_size", 0LL);
  const long long layers = cfg.value("num_layers", 0LL);
  if (hidden < 1 || layers < 1 || hidden > kLimit || layers > kLimit / hidden)
    return fail("LSTM config needs 'hidden_size' and 'num_layers' >= 1");
  return {weight_bytes(model) + layers * 6 * hidden * kFloat, {}};
}

Bytes sequential_bytes(const json& cfg, int q)
{
  if (!cfg.contains("models") || !cfg["models"].is_array() || cfg["models"].empty())
    return fail("Sequential config has no 'models'");
  long long total = 0;
  for (const auto& member : cfg["models"])
  {
    const Bytes b = model_bytes(member, q);
    if (b.value < 0)
      return b;
    total += b.value;
    if (total > kLimit)
      return fail("working set out of range");
  }
  return {total, {}};
}

Bytes model_bytes(const json& model, int q)
{
  if (!model.is_object())
    return fail("model is not a JSON object");
  const std::string arch = model.value("architecture", std::string());
  if (arch.empty())
    return fail("missing 'architecture'");
  if (!model.contains("config") || !model["config"].is_object())
    return fail("missing 'config' object");
  const json& cfg = model["config"];

  if (arch == "WaveNet" || arch == "SlimmableWaveNet")
    return wavenet_bytes(model, cfg, q);
  if (arch == "ConvNet")
    return convnet_bytes(model, cfg, q);
  if (arch == "Linear")
    return linear_bytes(model, cfg, q);
  if (arch == "LSTM")
    return lstm_bytes(model, cfg);
  if (arch == "Sequential")
    return sequential_bytes(cfg, q);
  if (arch == "SlimmableContainer")
    return fail("a SlimmableContainer nested in another model");
  return fail("unknown architecture '" + arch + "'");
}

WorkingSet finish(WorkingSet ws, const Bytes& full, const Bytes& slim)
{
  if (full.value < 0)
  {
    ws.detail = full.why;
    return ws;
  }
  if (slim.value < 0)
  {
    ws.detail = slim.why;
    return ws;
  }
  ws.ok = true;
  ws.full_bytes = static_cast<std::size_t>(full.value);
  ws.slim_bytes = static_cast<std::size_t>(slim.value);
  ws.slimmable = ws.slim_bytes < ws.full_bytes;
  return ws;
}

/// A container loads its last submodel (container.cpp: "Default to full size"), and
/// `SetSlimmableSize(0.0)` selects the first whose `max_value` is above 0.0 -- the same scan as
/// ContainerModel::_get_index_for_slimmable_size.
WorkingSet container_working_set(const json& model, int q)
{
  WorkingSet ws;
  const json& cfg = model.at("config");
  if (!cfg.contains("submodels") || !cfg["submodels"].is_array() || cfg["submodels"].empty())
  {
    ws.detail = "SlimmableContainer has no 'submodels'";
    return ws;
  }
  const json& subs = cfg["submodels"];
  std::size_t slim_index = subs.size() - 1;
  for (std::size_t i = 0; i < subs.size(); ++i)
  {
    if (!subs[i].is_object() || !subs[i].contains("max_value") || !subs[i]["max_value"].is_number())
    {
      ws.detail = "SlimmableContainer submodel entry missing a numeric 'max_value'";
      return ws;
    }
    if (0.0 < subs[i]["max_value"].get<double>())
    {
      slim_index = i;
      break;
    }
  }
  if (!subs.back().contains("model") || !subs[slim_index].contains("model"))
  {
    ws.detail = "SlimmableContainer submodel entry missing 'model'";
    return ws;
  }
  const Bytes full = model_bytes(subs.back()["model"], q);
  const Bytes slim = model_bytes(subs[slim_index]["model"], q);
  ws.detail = "SlimmableContainer";
  return finish(std::move(ws), full, slim);
}

WorkingSet compute(const json& model, int q)
{
  if (model.is_object() && model.value("architecture", std::string()) == "SlimmableContainer" &&
      model.contains("config") && model["config"].is_object())
    return container_working_set(model, q);
  WorkingSet ws;
  const Bytes b = model_bytes(model, q);
  if (b.value >= 0)
    ws.detail = model.value("architecture", std::string());
  return finish(std::move(ws), b, b);
}

} // namespace

WorkingSet working_set_from_json(const std::string& json_text, int max_quantum)
{
  WorkingSet ws;
  if (max_quantum < 1)
  {
    ws.detail = "max_quantum must be >= 1";
    return ws;
  }
  json doc;
  try
  {
    doc = json::parse(json_text);
  }
  catch (const std::exception& e)
  {
    ws.detail = std::string("JSON parse error: ") + e.what();
    return ws;
  }
  try
  {
    return compute(doc, max_quantum);
  }
  catch (const std::exception& e)
  {
    // at()/get() on unexpected shapes throw; report rather than propagate.
    ws.detail = std::string("schema error: ") + e.what();
    return ws;
  }
}

WorkingSet working_set_from_file(const std::string& path, int max_quantum)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    WorkingSet ws;
    ws.detail = "cannot open " + path;
    return ws;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return working_set_from_json(ss.str(), max_quantum);
}

namespace
{

/// The `architecture` a document declares, or "" when it declares none this can read.
std::string family_of(const json& model)
{
  if (model.is_object() && model.contains("architecture") && model["architecture"].is_string())
    return model["architecture"].get<std::string>();
  return {};
}

} // namespace

std::string model_family_from_json(const std::string& json_text)
{
  json doc;
  try
  {
    doc = json::parse(json_text);
  }
  catch (const std::exception&)
  {
    return {};
  }
  // A SlimmableContainer is a wrapper: its cost is its submodels', and so is its name. Its last
  // submodel is the one the model loads at (container_working_set above) and every submodel of one
  // container is built the same way, so the last is as good a witness as any.
  if (family_of(doc) == "SlimmableContainer" && doc.contains("config") && doc["config"].is_object())
  {
    const json& cfg = doc["config"];
    if (cfg.contains("submodels") && cfg["submodels"].is_array() && !cfg["submodels"].empty())
    {
      const json& last = cfg["submodels"].back();
      if (last.is_object() && last.contains("model"))
      {
        const std::string inner = family_of(last["model"]);
        if (!inner.empty())
          return inner;
      }
    }
  }
  return family_of(doc);
}

std::string model_family_from_file(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return model_family_from_json(ss.str());
}

} // namespace namchain
