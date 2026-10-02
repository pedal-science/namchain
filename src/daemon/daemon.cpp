// namchaind: the reference daemon (plan.md §8.2) and the control thread of docs/rig-pipeline.md.
//
// Platform note: the control transport wants "block in one poll set", and signalfd, accept4 and a
// Unix SOCK_SEQPACKET listener are the Linux spelling of it. The four places that spelling shows
// through go via namchain/daemon/platform.h, so this file stays one implementation rather than two
// interleaved by preprocessor. Ports beyond Linux and macOS need that header, not this one.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#if defined(__linux__) || defined(__APPLE__)

#include "namchain/daemon/daemon.h"

#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
#include <system_error>

#include <json.hpp>

#include "namchain/daemon/platform.h"
#include "namchain/graph/carry.h"
#include "namchain/graph/macro.h"
#include "namchain/graph/rig.h"
#include "namchain/graph/patch.h"
#include "namchain/pcm.h"
#include "namchain/rt/scheduling.h"
#include "namchain/rt_check.h"
#include "namchain/version.h"

namespace namchain
{
namespace daemon
{
namespace
{

/// Every plan gets its own arena; 1 MiB is what the soak compiles any sane rig out of, and a rig
/// that needs more gets "arena exhausted" rather than a silently truncated graph.
constexpr std::size_t kArenaBytes = 1u << 20;

/// \brief Whether this peer's messages carry a length prefix rather than being one datagram each.
///
/// A TCP peer always does: a stream has no message boundaries of its own. A local peer does only
/// where the local socket is a stream too, which is everywhere `PF_LOCAL` has no `SOCK_SEQPACKET`
/// (see namchain/daemon/platform.h). The two carry identical bytes inside the envelope, so this is
/// the only thing the rest of the daemon needs to know about the difference.
constexpr bool peer_is_framed(PeerKind peer)
{
  return peer == PeerKind::Remote || platform::local_transport_is_stream;
}

/// One SEQPACKET message. A request names a rig, it does not carry one, so this is generous; a
/// message that fills it is refused rather than truncated into something that parses. The TCP
/// transport has its own cap (\ref Config::tcp_max_message_bytes), because a network peer is a
/// different kind of caller than a local one -- but a size that *is* accepted means the same thing on
/// both, which is why this is a byte count and not a message shape.
constexpr std::size_t kMaxUnixMessageBytes = 256u * 1024;

/// The protocol version this daemon speaks. Incremented when the request/response shape changes.
///
/// 2 added `rig.apply` (a whole document in memory), the document in `rig.get`, the per-chain array in
/// `engine.status`, a `chain` on `rig.patch` and `engine.mute`, and `features`. A version-1 client is
/// still served: every change is additive, and unknown fields are ignored by both sides, so the
/// negotiation accepts anything from 1 up to this (docs/ui-integration-plan.md §1.4).
constexpr int kProtocolVersion = 2;
constexpr int kOldestProtocolVersion = 1;

/// What this daemon can be relied on to do, so a client degrades rather than guesses. Grows as the
/// slices land (docs/ui-integration-plan.md §1.4): a client that does not see "stereo" does not offer
/// a pan control, and one that sees "schema" knows to build its inspector from `engine.schema`.
nlohmann::json protocol_features()
{
  return nlohmann::json::array({"multi_chain", "documents", "rig_apply", "rig_get_document",
                                "structural_crossfade", "smooth_params", "bypass", "guard",
                                "per_chain_mute", "schema", "tcp_transport", "assets", "record",
                                "local_transitions", "meta", "bypass_ramp", "model_budget"});
}

/// The longest recording `engine.record` will make, so a client that forgets to stop one cannot fill
/// the disk with it. An hour of eight channels at 48 kHz is about 5 GB; the WAV format's 4 GiB
/// limit, which the recorder also enforces, is reached first on an interface that wide.
constexpr double kMaxRecordSeconds = 3600.0;
constexpr double kDefaultRecordSeconds = 600.0;

/// A recording's name: one plain path component, so it can only ever name a directory directly under
/// Config::record_dir. Letters, digits, '.', '_' and '-', not starting with '.', at most 64.
bool valid_recording_name(const std::string& name)
{
  if (name.empty() || name.size() > 64 || name[0] == '.')
    return false;
  for (char c : name)
  {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '_' || c == '-';
    if (!ok)
      return false;
  }
  return true;
}

std::string utc_now_iso8601()
{
  const std::time_t now = std::time(nullptr);
  std::tm parts{};
  gmtime_r(&now, &parts);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &parts);
  return text;
}

/// One parameter as `engine.schema` reports it. One shape for the node's own parameters and for the
/// per-port ones, so a client builds a control the same way wherever it is addressed.
///
/// A parameter whose choices live in the asset registry (`options_from: "models"` / `"irs"`) is
/// resolved here rather than left for the client to join: the app gets a palette from one call, and
/// the answer cannot disagree with `engine.assets` because it is the same scan (slice D2).
nlohmann::json param_schema(const render::ParamDesc& param, const assets::Registry& registry)
{
  nlohmann::json p;
  p["id"] = param.id;
  p["name"] = param.name == nullptr ? param.id : param.name;
  switch (param.kind)
  {
  case render::ParamKind::Number: p["kind"] = "number"; break;
  case render::ParamKind::Option: p["kind"] = "option"; break;
  case render::ParamKind::Flag: p["kind"] = "flag"; break;
  }
  p["unit"] = param.unit == nullptr ? nlohmann::json(nullptr) : nlohmann::json(param.unit);
  // A required parameter, and one of a `one_of` group, have no default: publishing the zero they
  // happen to be stored with would be a value no node could be built from.
  p["default"] = (param.required || param.one_of != nullptr) ? nlohmann::json(nullptr)
                                                            : nlohmann::json(param.default_value);
  p["min"] = param.has_min ? nlohmann::json(param.min) : nlohmann::json(nullptr);
  p["min_inclusive"] = param.min_inclusive;
  p["max"] = param.has_max ? nlohmann::json(param.max) : nlohmann::json(nullptr);
  p["max_inclusive"] = param.max_inclusive;
  p["step"] = param.step > 0.0 ? nlohmann::json(param.step) : nlohmann::json(nullptr);
  p["snap"] = param.snap;
  p["scale"] = param.scale == render::ParamScale::Log ? "log" : "linear";
  p["rampable"] = param.rampable;
  p["patchable"] = param.patchable;
  p["hidden"] = param.hidden;
  p["required"] = param.required;
  p["one_of"] = param.one_of == nullptr ? nlohmann::json(nullptr) : nlohmann::json(param.one_of);
  p["one_of_required"] = param.one_of_required;
  p["options_from"] = param.options_from == nullptr ? nlohmann::json(nullptr) : nlohmann::json(param.options_from);

  assets::Kind kind = assets::Kind::Model;
  const bool from_registry = assets::kind_from_options_from(param.options_from, kind);
  if (param.options != nullptr)
  {
    nlohmann::json options = nlohmann::json::array();
    for (int o = 0; o < param.option_count; ++o)
      options.push_back({{"value", param.options[o].value}, {"label", param.options[o].label}});
    p["options"] = std::move(options);
  }
  else if (from_registry)
  {
    // The value is the registry id -- what a document names, and what a network client is allowed to
    // name. The label is the id too: prettifying "tweed/deluxe" into "Deluxe" is presentation, and
    // presentation is the app's job (docs/ui-integration-plan.md §1.9). An empty library is an empty
    // list, which is a true answer a client can show as "no models installed".
    nlohmann::json options = nlohmann::json::array();
    for (const assets::Asset* asset : registry.of_kind(kind))
      options.push_back({{"value", asset->id}, {"label", asset->id}});
    p["options"] = std::move(options);
  }
  else
  {
    p["options"] = nlohmann::json(nullptr);
  }
  return p;
}

/// One asset as `engine.assets` reports it.
nlohmann::json asset_json(const assets::Asset& asset)
{
  nlohmann::json entry;
  entry["id"] = asset.id;
  entry["relative"] = asset.relative;
  entry["path"] = asset.path;
  entry["name"] = asset.name;
  entry["group"] = asset.group;
  entry["kind"] = assets::kind_name(asset.kind);
  entry["size"] = asset.size;
  entry["mtime"] = asset.mtime;
  // Null rather than 0 for "the file did not say": a palette showing 0 Hz would be showing a lie,
  // and a client that wants a number can test for null once.
  entry["sample_rate"] = asset.sample_rate > 0.0 ? nlohmann::json(asset.sample_rate) : nlohmann::json(nullptr);
  entry["frames"] = asset.frames == 0 ? nlohmann::json(nullptr) : nlohmann::json(asset.frames);
  entry["architecture"] = asset.architecture.empty() ? nlohmann::json(nullptr)
                                                     : nlohmann::json(asset.architecture);
  return entry;
}

/// One library root as the scan found it.
nlohmann::json root_json(const assets::Root& root)
{
  nlohmann::json entry;
  entry["path"] = root.path;
  entry["readable"] = root.readable;
  entry["error"] = root.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(root.error);
  entry["count"] = root.count;
  return entry;
}

/// The transform names `engine.schema` publishes, so the param map is legible to a client that wants
/// to know what a macro parameter actually drives.
const char* transform_name(graph::Transform transform)
{
  switch (transform)
  {
  case graph::Transform::Direct: return "direct";
  case graph::Transform::DbToLinear: return "db_to_linear";
  case graph::Transform::LinearToDb: return "linear_to_db";
  case graph::Transform::OneMinus: return "one_minus";
  case graph::Transform::ScaleBy: return "scale_by";
  }
  return "direct";
}

/// \brief One macro as `engine.schema` reports it.
///
/// The same fields as a node type -- so a palette lists both from one array and tells them apart by
/// `kind` -- plus two a primitive has no need for: the `roles` a client can address with
/// `element` + `role`, and, on each parameter, the `targets` it drives. Publishing the targets is not
/// required to use the API, but it is what makes "why did my knob move two nodes" answerable from the
/// daemon rather than from the source.
nlohmann::json macro_type_schema(const graph::MacroType& macro, const assets::Registry& registry)
{
  nlohmann::json entry;
  entry["id"] = macro.id;
  entry["kind"] = "macro";
  entry["name"] = macro.name;
  entry["category"] = macro.category;
  entry["inputs"] = macro.inputs;
  entry["outputs"] = macro.outputs;
  entry["channels"] = 1;
  entry["stereo"] = false;
  entry["bypass"] = macro.bypass == graph::MacroBypass::AllRolesCarry ? "roles" : "crossfade";

  nlohmann::json roles = nlohmann::json::array();
  for (int i = 0; i < macro.node_count; ++i)
    roles.push_back({{"role", macro.nodes[i].role}, {"type", macro.nodes[i].type}});
  entry["roles"] = std::move(roles);

  nlohmann::json params = nlohmann::json::array();
  for (int i = 0; i < macro.param_count; ++i)
  {
    nlohmann::json param = param_schema(macro.params[i].desc, registry);
    nlohmann::json targets = nlohmann::json::array();
    for (int t = 0; t < macro.params[i].target_count; ++t)
    {
      const graph::MacroTarget& target = macro.params[i].targets[t];
      targets.push_back({{"role", target.role},
                         {"param", target.param},
                         {"transform", transform_name(target.transform)},
                         {"scale", target.scale}});
    }
    param["targets"] = std::move(targets);
    params.push_back(std::move(param));
  }
  entry["params"] = std::move(params);
  return entry;
}

/// One node type as `engine.schema` reports it: what a palette offers and what an inspector lays out.
///
/// Every parameter carries its kind, unit, default, bounds (with inclusive/exclusive made explicit),
/// step and whether it can be ramped -- the catalogue a client would otherwise have to guess at, and
/// the thing that makes "the knob does nothing" impossible rather than merely unlikely
/// (docs/ui-integration-plan.md §1.4).
nlohmann::json node_type_schema(const render::NodeType& type, const assets::Registry& registry)
{
  nlohmann::json entry;
  entry["id"] = type.id;
  entry["name"] = type.name == nullptr ? type.id : type.name;
  entry["category"] = type.category == nullptr ? "utility" : type.category;
  entry["kind"] = "primitive";

  nlohmann::json in_ports = nlohmann::json::array();
  for (int i = 0; i < type.inputs; ++i)
  {
    // An unconnected input is legal and is bound to silence, which is what makes a dry lane a matter
    // of not wiring it rather than of wiring a special node.
    in_ports.push_back({{"name", "in" + std::to_string(i)}, {"channels", type.channels}, {"optional", true}});
  }
  nlohmann::json out_ports = nlohmann::json::array();
  for (int i = 0; i < type.outputs; ++i)
    out_ports.push_back({{"name", "out" + std::to_string(i)}, {"channels", type.channels}, {"optional", false}});
  entry["ports"] = {{"in", std::move(in_ports)}, {"out", std::move(out_ports)}};

  if (type.arity_param != nullptr)
  {
    entry["arity"] = {{"param", type.arity_param},
                      {"min", type.min_ports},
                      {"max", type.max_ports},
                      {"default", type.arity_param == std::string("inputs") ? type.inputs : type.outputs}};
  }
  else
  {
    entry["arity"] = nullptr;
  }

  entry["flags"] = {{"stateful", type.create != nullptr},
                    {"has_lookback", false},
                    {"has_tail", false},
                    {"bypassable", type.inputs == 1 && type.outputs == 1},
                    {"block_delay", type.block_delay}};
  entry["latency_samples"] = 0;
  entry["tail_samples"] = 0;

  nlohmann::json params = nlohmann::json::array();
  for (int i = 0; i < type.param_count; ++i)
    params.push_back(param_schema(type.params[i], registry));
  entry["params"] = std::move(params);

  // Per-port parameters, declared once and addressed with a port. Same shapes as the node's own, so a
  // client builds one control per lane without a naming convention of its own.
  nlohmann::json per_port = nlohmann::json::array();
  for (int i = 0; i < type.per_port_param_count; ++i)
    per_port.push_back(param_schema(type.per_port_params[i], registry));
  entry["per_port_params"] = std::move(per_port);
  return entry;
}


/// \brief Compare a presented secret with the configured one without an early exit.
///
/// `presented == expected` stops at the first wrong byte, and how long it took says how much of the
/// token was right -- which over a LAN is enough to recover it a byte at a time. This folds every byte
/// into one accumulator and checks the length in the same pass, so the work depends only on the
/// token's length, which is not the secret. It is a real comparison, not a substitute for a long
/// token: three guesses per connection is a meaningful bound only if guessing is hopeless.
bool constant_time_equal(const std::string& presented, const std::string& expected)
{
  const std::size_t longest = std::max(presented.size(), expected.size());
  unsigned char difference = presented.size() == expected.size() ? 0u : 1u;
  for (std::size_t i = 0; i < longest; ++i)
  {
    const unsigned char a = i < presented.size() ? static_cast<unsigned char>(presented[i]) : 0u;
    const unsigned char b = i < expected.size() ? static_cast<unsigned char>(expected[i]) : 0u;
    difference = static_cast<unsigned char>(difference | static_cast<unsigned char>(a ^ b));
  }
  return difference == 0u;
}

/// The plan and the document chain a patch resolves against. Passed explicitly rather than reached
/// through the daemon, because resolving an edit is a pure question about what is running.
struct EditContext
{
  const PlanSlot* slot = nullptr;
  const graph::Chain* chain = nullptr;
  std::size_t chain_index = 0;
};

/// \brief One `set_param` op resolved against the live plan: the commands it becomes, and where the
/// value lands in the document.
///
/// Resolved before anything is pushed, so a patch that fails half way through leaves the rig exactly
/// as it was -- which is the rule the structural class already lives by, and there is no reason for
/// the smooth one to be different.
struct ResolvedEdit
{
  std::vector<PlanCommand> commands;
  /// The document node and parameter to record the value against, so `rig.get` and a later
  /// structural edit see what is playing. Empty `document_node` means "not a document parameter":
  /// a per-port setting, or a primitive inside a macro, both of which are runtime state.
  std::string document_node;
  std::string document_param;
  double value = 0.0;
  bool is_text = false;
  std::string text;
};

/// \brief Resolve one smooth-class op, or report that the patch has to go the structural way.
///
/// \param structural true when the op is well formed but cannot be a ramp -- a parameter that is not
/// rampable, an op that is not a `set_param` at all, a target the plan does not have. The structural
/// path then rebuilds the graph from the document, and it is the authority on *why* something is not
/// applicable; producing a second, differently worded complaint here would be worse than none.
/// \param error set only when the op is malformed, or when it addresses a per-port parameter that
/// does not exist -- which has no structural fallback, because a lane is state rather than a graph.
bool resolve_smooth_edit(const EditContext& context, const nlohmann::json& op, ResolvedEdit& edit,
                         bool& structural, std::string& error)
{
  structural = false;
  const PlanSlot* slot = context.slot;
  if (slot == nullptr || slot->plan == nullptr)
  {
    structural = true;
    return false;
  }

  if (!op.is_object() || !op.contains("op") || !op["op"].is_string() ||
      op["op"].get<std::string>() != "set_param")
  {
    structural = true;
    return false;
  }
  if (!op.contains("param") || !op["param"].is_string() || op["param"].get<std::string>().empty())
  {
    structural = true; // let apply_patch say what a set_param needs
    return false;
  }
  const std::string param = op["param"].get<std::string>();

  const bool is_text = op.contains("value") && op["value"].is_string();
  if (!op.contains("value") || !(op["value"].is_number() || op["value"].is_string()))
  {
    structural = true;
    return false;
  }
  const double value = op["value"].is_number() ? op["value"].get<double>() : 0.0;
  if (op["value"].is_number() && !std::isfinite(value))
  {
    error = "'" + param + "' is not finite";
    return false;
  }

  int port = -1;
  if (op.contains("port"))
  {
    if (!op["port"].is_number_integer() || op["port"].get<int>() < 0)
    {
      error = "'port' must be a non-negative integer";
      return false;
    }
    port = op["port"].get<int>();
  }

  const bool has_node = op.contains("node") || op.contains("id");
  std::string element;
  if (op.contains("element"))
  {
    if (!op["element"].is_string() || op["element"].get<std::string>().empty())
    {
      error = "'element' must be a non-empty string";
      return false;
    }
    element = op["element"].get<std::string>();
  }
  std::string role;
  if (op.contains("role"))
  {
    if (!op["role"].is_string() || op["role"].get<std::string>().empty())
    {
      error = "'role' must be a non-empty string";
      return false;
    }
    role = op["role"].get<std::string>();
  }

  const graph::Chain& chain = *context.chain;
  const std::vector<std::string>& node_ids = slot->plan->node_ids();

  // (a) `element` alone naming a macro: the macro-level parameter, mapped onto every node it drives.
  //     Tried before the plan lookup, because a macro node is not *in* the plan -- it was lowered.
  if (!element.empty() && role.empty() && !has_node)
  {
    const graph::MacroType* macro = nullptr;
    const graph::NodeSpec* macro_spec = nullptr;
    for (const graph::NodeSpec& spec : chain.graph.nodes)
    {
      if (spec.element != element)
        continue;
      if (const graph::MacroType* candidate = graph::find_macro_type(spec.type))
      {
        macro = candidate;
        macro_spec = &spec;
        break;
      }
    }
    if (macro != nullptr && macro_spec != nullptr)
    {
      const graph::MacroParamDecl* declared = graph::find_macro_param(*macro, param);
      if (declared == nullptr || declared->target_count == 0 || is_text)
      {
        // Not a parameter of this macro, or one the lowering itself consumes (a band count, a slope):
        // both are structural, and apply_patch gives the better message for the first.
        structural = true;
        return false;
      }
      for (int t = 0; t < declared->target_count; ++t)
      {
        const graph::MacroTarget& target = declared->targets[t];
        const int index = slot->plan->node_index_of_role(element, target.role);
        if (index < 0)
        {
          structural = true;
          return false;
        }
        std::size_t node_index = 0;
        uint32_t param_id = 0;
        if (!slot->plan->resolve_param(node_ids[static_cast<std::size_t>(index)], target.param, -1, node_index,
                                       param_id))
        {
          // A target that cannot ramp: the whole edit goes the structural way, because half a macro
          // parameter applied smoothly and half by a rebuild is not a state anybody asked for.
          structural = true;
          return false;
        }
        PlanCommand command;
        command.kind = PlanCommand::Kind::SetParam;
        command.slot = const_cast<PlanSlot*>(slot); // compared on the render thread, never dereferenced
        command.chain = static_cast<uint32_t>(context.chain_index);
        command.generation = slot->generation;
        command.node = static_cast<uint32_t>(node_index);
        command.param = param_id;
        command.value = static_cast<float>(graph::apply_transform(target.transform, value, target.scale));
        edit.commands.push_back(command);
      }
      edit.document_node = macro_spec->id;
      edit.document_param = param;
      edit.value = value;
      return true;
    }
  }

  // (b) A node of the plan, by id or by element (+ role).
  int index = -1;
  if (has_node)
  {
    std::string id;
    if (!op["node"].is_string() && op.contains("node"))
    {
      error = "'node' must be a string";
      return false;
    }
    id = op.contains("node") ? op["node"].get<std::string>() : op["id"].get<std::string>();
    if (id.empty())
    {
      error = "'node' must be a non-empty string";
      return false;
    }
    for (std::size_t i = 0; i < node_ids.size(); ++i)
    {
      if (node_ids[i] == id)
      {
        index = static_cast<int>(i);
        break;
      }
    }
  }
  else if (!element.empty())
  {
    index = slot->plan->node_index_of_role(element, role);
  }
  else
  {
    structural = true; // no address at all: apply_patch says what is missing
    return false;
  }

  if (index < 0)
  {
    // Either an id that is not in the plan (a macro's own id, or a node the plan does not have) or an
    // element that names several nodes. Both are the structural path's to explain: it works on the
    // document, where a macro still exists and where an ambiguous element has a different answer.
    if (port >= 0)
    {
      error = has_node ? "node '" + op["node"].get<std::string>() + "' port " + std::to_string(port) +
                           " has no settable param '" + param + "'"
                       : "element '" + element + "' has no single node to address a port on";
      return false;
    }
    structural = true;
    return false;
  }

  std::size_t node_index = 0;
  uint32_t param_id = 0;
  if (is_text || !slot->plan->resolve_param(node_ids[static_cast<std::size_t>(index)], param, port, node_index,
                                            param_id))
  {
    if (port >= 0 && !is_text)
    {
      // A port-addressed op has no structural fallback: a per-port parameter lives in the node's
      // state, not in the graph, so rewriting the graph could not express it. Falling through would
      // turn "lane 2 does not exist" into "the rig failed to compile", which is a worse answer.
      error = "node '" + node_ids[static_cast<std::size_t>(index)] + "' port " + std::to_string(port) +
              " has no settable param '" + param + "'";
      return false;
    }
    structural = true;
    return false;
  }

  PlanCommand command;
  command.kind = PlanCommand::Kind::SetParam;
  command.slot = const_cast<PlanSlot*>(slot); // compared on the render thread, never dereferenced
  command.chain = static_cast<uint32_t>(context.chain_index);
  command.generation = slot->generation;
  command.node = static_cast<uint32_t>(node_index);
  command.param = param_id;
  command.value = static_cast<float>(value);
  edit.commands.push_back(command);

  // The document is updated for a *document* parameter, and only then: a node the document holds, and
  // a parameter of that node rather than one of its ports. A lowered primitive inside a macro is not
  // in the document at all -- the macro's own parameter is what the document has to say about it --
  // and a per-port setting is runtime state by design (docs/daemon.md §"One document, several
  // chains"). Recording neither is deliberate; guessing a mapping back would be inventing one.
  if (port < 0)
  {
    const std::string& id = node_ids[static_cast<std::size_t>(index)];
    for (const graph::NodeSpec& spec : chain.graph.nodes)
    {
      if (spec.id == id)
      {
        edit.document_node = spec.id;
        edit.document_param = param;
        edit.value = value;
        break;
      }
    }
  }
  return true;
}

/// \brief The first absolute path in \p document, or an empty string.
///
/// A Unix client is the operator -- filesystem permissions already decided who may talk to the daemon
/// -- so an absolute path in a document it applies is the operator's own business. A network client is
/// not, and an absolute path there is an arbitrary-file-read primitive: model, IR and other string
/// parameters name files the daemon will open. Remote documents therefore name assets relative to the
/// rigs root, which is the same boundary a `rig.load` *name* already lives behind.
std::string first_absolute_path(const graph::Document& document)
{
  for (const graph::Chain& chain : document.chains)
  {
    for (const graph::NodeSpec& node : chain.graph.nodes)
    {
      for (const auto& [key, value] : node.strings)
      {
        if (value.size() > 1 && value.front() == '/')
          return value;
      }
    }
  }
  return {};
}

/// \brief The first absolute path among a patch's ops, or an empty string.
///
/// Checked on the *ops* rather than on the patched document on purpose: a remote client may edit a
/// document the operator loaded from a rig file that uses absolute paths, it may just not introduce
/// any of its own. Refusing the whole document would break the legitimate case; refusing only what the
/// client wrote is the rule it can actually satisfy.
std::string first_absolute_path_in_ops(const nlohmann::json& ops)
{
  for (const auto& op : ops)
  {
    if (!op.is_object())
      continue;
    for (const auto& [key, value] : op.items())
    {
      if (!value.is_string())
        continue;
      const std::string text = value.get<std::string>();
      if (text.size() > 1 && text.front() == '/')
        return text;
    }
  }
  return {};
}

/// A node's string parameter, or an empty string when it has none.
std::string string_param(const graph::NodeSpec& node, const char* name)
{
  const auto found = node.strings.find(name);
  return found == node.strings.end() ? std::string() : found->second;
}

/// Whether the model budget slimmed anything.
bool budget_slimmed(const BudgetOutcome& budget)
{
  for (const BudgetedModel& model : budget.models)
  {
    if (model.slimmed)
      return true;
  }
  return false;
}

/// Every model the budget counted, with what it reads per block as published: the part of a message a
/// person acts on, because it says which block to swap for a smaller model. The chain is named only
/// when the document has more than one.
std::string describe_models(const BudgetOutcome& budget, bool name_chains)
{
  std::string text;
  for (const BudgetedModel& model : budget.models)
  {
    text += text.empty() ? "" : ", ";
    text += name_chains ? model.chain + "/" + model.node : model.node;
    if (!model.estimated)
      text += " (not estimated: " + model.why + ")";
    else if (model.slimmed)
      text += " " + format_kib(model.slim_bytes) + " (slimmed from " + format_kib(model.full_bytes) + ")";
    else
      text += " " + format_kib(model.full_bytes) + (model.slimmable ? "" : " (cannot be slimmed)");
  }
  return text;
}

void delete_plan_slot(void* object)
{
  delete static_cast<PlanSlot*>(object);
}

/// mlockall(MCL_CURRENT), never MCL_FUTURE (§14.10). The soft limit is raised to the hard one first,
/// because on a host whose administrator has already granted a large LimitMEMLOCK the soft limit is
/// the only thing in the way.
bool lock_process_memory()
{
  rlimit limit{};
  if (::getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_cur != limit.rlim_max)
  {
    limit.rlim_cur = limit.rlim_max;
    ::setrlimit(RLIMIT_MEMLOCK, &limit); // best effort: the hard limit is the administrator's call
  }
  return ::mlockall(MCL_CURRENT) == 0;
}

std::string ok_response(const std::string& id, nlohmann::json body)
{
  body["ok"] = true;
  if (!id.empty())
    body["id"] = id;
  return body.dump();
}

std::string error_response(const std::string& id, const std::string& code, const std::string& message)
{
  nlohmann::json body;
  body["ok"] = false;
  if (!id.empty())
    body["id"] = id;
  body["error"] = {{"code", code}, {"message", message}};
  return body.dump();
}

} // namespace

const char* device_state_name(DeviceState state)
{
  switch (state)
  {
  case DeviceState::Closed:
    return "closed";
  case DeviceState::Running:
    return "running";
  case DeviceState::Lost:
    return "lost";
  }
  return "unknown";
}

Daemon::Daemon(Config config) : config_(std::move(config)) {}

Daemon::~Daemon()
{
  stop();
}

void Daemon::request_stop()
{
  stop_requested_.store(true);
}

bool Daemon::setup_signals(std::string& error)
{
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGHUP);

  // Blocked before the render thread exists, so every thread inherits the block and the reader is
  // the only consumer. Blocking after the thread started would leave a window where the default
  // disposition killed the process. The old mask is kept and restored by stop().
  if (::pthread_sigmask(SIG_BLOCK, &mask, &previous_mask_) != 0)
  {
    error = std::string("pthread_sigmask: ") + std::strerror(errno);
    return false;
  }
  mask_saved_ = true;
  const int fd = platform::open_signal_reader(mask, error);
  if (fd < 0)
    return false;
  signal_fd_ = fd;
  return true;
}

bool Daemon::bind_socket(std::string& error)
{
  const std::string& path = config_.socket_path;
  if (path.empty())
  {
    error = "the socket path is empty";
    return false;
  }

  // RuntimeDirectory= creates this in the unit; a hand-run needs it too, and creating it here makes
  // the socket path the only thing an operator has to get right.
  std::error_code ignored;
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent, ignored);

  // Non-blocking, because accepting is a loop until EAGAIN now that more than one connection can be
  // waiting; on a blocking listener that loop would stop in accept() rather than in the poll that
  // said there was something to accept.
  const int fd = platform::open_socket(AF_UNIX, platform::local_socket_type(), 0);
  if (fd < 0)
  {
    error = std::string("socket: ") + std::strerror(errno);
    return false;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path))
  {
    ::close(fd);
    error = "socket path is too long: " + path;
    return false;
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

  // A node left by a previous run is stale, and bind() would refuse it. Removing it before binding
  // is what makes `systemctl restart` work.
  ::unlink(path.c_str());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
  {
    error = "bind " + path + ": " + std::strerror(errno);
    ::close(fd);
    return false;
  }
  if (::chmod(path.c_str(), static_cast<mode_t>(config_.socket_mode)) != 0)
  {
    error = "chmod " + path + ": " + std::strerror(errno);
    ::close(fd);
    ::unlink(path.c_str());
    return false;
  }
  if (::listen(fd, 8) != 0)
  {
    error = "listen " + path + ": " + std::strerror(errno);
    ::close(fd);
    ::unlink(path.c_str());
    return false;
  }
  listen_fd_ = fd;
  return true;
}

void Daemon::close_socket()
{
  for (const Connection& client : clients_)
    ::close(client.fd);
  clients_.clear();
  // The path is unlinked only when this process bound it: a failed bind must not remove a socket a
  // previous (or another) daemon is still serving.
  if (listen_fd_ >= 0)
  {
    ::close(listen_fd_);
    listen_fd_ = -1;
    if (!config_.socket_path.empty())
      ::unlink(config_.socket_path.c_str());
  }
  if (tcp_fd_ >= 0)
  {
    ::close(tcp_fd_);
    tcp_fd_ = -1;
    tcp_port_ = 0;
  }
}

bool Daemon::bind_tcp(std::string& error)
{
  if (!config_.tcp_listen)
    return true;

  // AI_NUMERICHOST: no DNS at startup. An address that resolves to something else tomorrow is not an
  // address an appliance should bind, and a name that fails to resolve at boot would take the daemon
  // down with it.
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
  addrinfo* candidates = nullptr;
  const std::string service = std::to_string(config_.tcp_port);
  const int resolved = ::getaddrinfo(config_.tcp_address.c_str(), service.c_str(), &hints, &candidates);
  if (resolved != 0)
  {
    error = "tcp_address '" + config_.tcp_address + "' is not a numeric address: " + ::gai_strerror(resolved);
    return false;
  }

  int fd = -1;
  std::string last;
  for (addrinfo* candidate = candidates; candidate != nullptr; candidate = candidate->ai_next)
  {
    fd = platform::open_socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (fd < 0)
    {
      last = std::strerror(errno);
      continue;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(fd, candidate->ai_addr, candidate->ai_addrlen) != 0)
    {
      last = std::strerror(errno);
      ::close(fd);
      fd = -1;
      continue;
    }
    if (::listen(fd, static_cast<int>(config_.tcp_max_connections)) != 0)
    {
      last = std::strerror(errno);
      ::close(fd);
      fd = -1;
      continue;
    }
    break;
  }
  ::freeaddrinfo(candidates);
  if (fd < 0)
  {
    error = "cannot listen on tcp " + config_.tcp_address + ":" + service + ": " +
            (last.empty() ? "no usable address" : last);
    return false;
  }

  // A configured port of 0 means "pick one". Reading it back is what makes that reportable, and it is
  // also how a test gets a listener with no fixed port and so no collision with a parallel run.
  tcp_port_ = config_.tcp_port;
  sockaddr_storage bound{};
  socklen_t length = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length) == 0)
  {
    if (bound.ss_family == AF_INET)
      tcp_port_ = ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
    else if (bound.ss_family == AF_INET6)
      tcp_port_ = ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
  }
  tcp_fd_ = fd;
  return true;
}

void Daemon::close_transport()
{
  close_socket();
  if (signal_fd_ >= 0)
  {
    platform::close_signal_reader(signal_fd_);
    signal_fd_ = -1;
  }
  if (mask_saved_)
  {
    ::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
    mask_saved_ = false;
  }
}

std::size_t Daemon::connection_count(PeerKind peer) const
{
  std::size_t count = 0;
  for (const Connection& connection : clients_)
    count += connection.session.peer == peer ? 1u : 0u;
  return count;
}

std::size_t Daemon::max_message_bytes(PeerKind peer) const
{
  return peer == PeerKind::Remote ? config_.tcp_max_message_bytes : kMaxUnixMessageBytes;
}

void Daemon::refuse_connection(int fd, PeerKind peer, const char* code, const std::string& message)
{
  // Best effort, and deliberately so. The useful part of `too_many_connections` is that the client is
  // told why, but a refusal that cannot be delivered is still a refusal: the socket is non-blocking,
  // a refusal is one small message, and anything the kernel will not take now is not worth a queue for
  // a connection that is being dropped.
  const std::string reply = error_response("", code, message);
  std::vector<char> bytes;
  if (peer_is_framed(peer))
    append_frame(bytes, reply);
  else
    bytes.assign(reply.begin(), reply.end());
  (void)::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
  ::close(fd);
}

void Daemon::accept_connections(int listener, PeerKind peer)
{
  const std::size_t cap = peer == PeerKind::Remote ? config_.tcp_max_connections : kMaxLocalClients;
  for (;;)
  {
    const int fd = platform::accept_socket(listener);
    if (fd < 0)
    {
      if (errno == EINTR)
        continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK && config_.verbose)
        std::fprintf(stderr, "[namchaind] accept: %s\n", std::strerror(errno));
      return;
    }

    if (connection_count(peer) >= cap)
    {
      refuse_connection(fd, peer, "too_many_connections",
                        "this daemon serves at most " + std::to_string(cap) +
                          (peer == PeerKind::Remote ? " TCP control connections" : " local control connections"));
      continue;
    }

    Connection connection;
    connection.fd = fd;
    connection.session.peer = peer;
    connection.session.authorized = peer == PeerKind::Local;
    connection.accepted_at = std::chrono::steady_clock::now();
    connection.peer = peer == PeerKind::Remote ? config_.tcp_address : "unix";

    if (peer == PeerKind::Remote)
    {
      // A modest send buffer, for the two reasons in the header: bounded kernel memory, and a partial
      // write that shows up immediately rather than after megabytes of buffered reply.
      int send_buffer = kTcpSendBufferBytes;
      ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));

      // An unauthenticated connection is on a clock. Four idle connections would otherwise hold every
      // slot this transport has, which turns "the port needs a token" into "the port needs a token,
      // unless somebody else got there first".
      if (config_.tcp_auth_timeout_ms > 0)
      {
        connection.deadline =
          connection.accepted_at + std::chrono::milliseconds(config_.tcp_auth_timeout_ms);
        connection.has_auth_deadline = true;
      }
    }
    clients_.push_back(std::move(connection));
  }
}

void Daemon::queue_reply(Connection& connection, std::string reply)
{
  if (peer_is_framed(connection.session.peer))
    append_frame(connection.out, reply);
  else
    connection.out.insert(connection.out.end(), reply.begin(), reply.end());

  // A client that never reads would otherwise grow this buffer for as long as it keeps asking. One
  // reply's worth of undelivered bytes is the cap; past it the connection is finished, because a
  // peer that cannot absorb a single answer is not a peer this daemon can serve. It is the reply
  // limit, not the request limit: a reply lists what the daemon holds, and `engine.assets` alone is
  // about a megabyte for a real library.
  const std::size_t pending = connection.out.size() - connection.out_sent;
  const std::size_t cap = std::max(kMaxReplyBytes, max_message_bytes(connection.session.peer));
  if (pending > cap + kFrameHeaderBytes)
    connection.close_after_flush = true;
}

bool Daemon::write_connection(Connection& connection)
{
  while (connection.out_sent < connection.out.size())
  {
    const ssize_t sent = ::send(connection.fd, connection.out.data() + connection.out_sent,
                                connection.out.size() - connection.out_sent, MSG_NOSIGNAL);
    if (sent > 0)
    {
      connection.out_sent += static_cast<std::size_t>(sent);
      continue;
    }
    if (sent < 0 && errno == EINTR)
      continue;
    // EAGAIN is the whole point of the outbox: the kernel would not take the rest now, so the control
    // loop goes and does something else and comes back when poll says this socket is writable again.
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return true;
    return false; // the peer is gone
  }

  connection.out.clear();
  connection.out_sent = 0;
  return !connection.close_after_flush;
}

void Daemon::close_connection(std::size_t index)
{
  ::close(clients_[index].fd);
  clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
}

void Daemon::drop_expired_connections()
{
  const auto now = std::chrono::steady_clock::now();
  for (std::size_t index = clients_.size(); index-- > 0;)
  {
    const Connection& connection = clients_[index];
    const bool auth_expired = connection.has_auth_deadline && !connection.session.authorized &&
                              now >= connection.deadline;
    const bool flush_expired = connection.has_flush_deadline && now >= connection.deadline;
    if (!auth_expired && !flush_expired)
      continue;
    if (config_.verbose)
      std::fprintf(stderr, "[namchaind] closing %s: %s\n", connection.peer.c_str(),
                   auth_expired ? "no authentication within the window" : "the last reply never drained");
    close_connection(index);
  }
}

/// Read every whole SEQPACKET message waiting on \p connection and answer each. One datagram is one
/// message, so there is no framing here at all -- which is the reason the local transport kept it.
bool Daemon::read_datagrams(Connection& connection)
{
  // A local connection's `in` is a scratch buffer for the datagram being read, not an accumulation:
  // one datagram is one message. It lives on the connection so it is allocated once per connection
  // rather than once per request, which is what the previous single shared buffer did without the
  // reentrancy problem of sharing it across connections.
  if (connection.in.size() != kMaxUnixMessageBytes)
    connection.in.assign(kMaxUnixMessageBytes, '\0');
  std::vector<char>& buffer = connection.in;

  for (;;)
  {
    const ssize_t got = ::recv(connection.fd, buffer.data(), buffer.size(), 0);
    if (got == 0)
      return false; // an orderly close
    if (got < 0)
    {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return true;
      return false;
    }

    std::string reply;
    if (static_cast<std::size_t>(got) == buffer.size())
    {
      reply = error_response("", "message_too_large", "the request filled the message buffer");
    }
    else
    {
      reply = handle_request(std::string(buffer.data(), static_cast<std::size_t>(got)), connection.session);
      if (connection.session.auth_failures >= kMaxAuthFailures)
        connection.close_after_flush = true;
    }
    queue_reply(connection, std::move(reply));
    if (connection.close_after_flush)
    {
      connection.has_flush_deadline = true;
      connection.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kFlushDeadlineMs);
    }
    if (!write_connection(connection))
      return false;
    // Stop reading while a reply is still undelivered: that is backpressure, and it is what keeps a
    // client that asks faster than it reads from costing this daemon memory.
    if (!connection.out.empty())
      return true;
  }
}

/// Reassemble length-prefixed messages from a stream and answer each.
bool Daemon::read_framed(Connection& connection)
{
  // The cap follows the peer, not the transport. This read used to serve only TCP connections, so
  // naming tcp_max_message_bytes directly was the same thing; a local peer reaching it (which is
  // what a stream local socket does) would otherwise be measured against a limit meant for the
  // network, and queue_reply's matching check against max_message_bytes() would disagree with it.
  const std::size_t cap = max_message_bytes(connection.session.peer);
  for (;;)
  {
    // How many bytes the message being assembled needs. The length prefix is checked against the cap
    // *before* its body is read, so an oversized message costs a rejection instead of a buffer.
    std::size_t needed = kFrameHeaderBytes;
    if (connection.in.size() >= kFrameHeaderBytes)
    {
      const std::uint32_t declared = frame_length(connection.in.data());
      if (declared > cap)
      {
        queue_reply(connection, error_response("", "message_too_large",
                                               "the message declares " + std::to_string(declared) +
                                                 " bytes; the limit is " + std::to_string(cap)));
        connection.close_after_flush = true;
        connection.has_flush_deadline = true;
        connection.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kFlushDeadlineMs);
        connection.in.clear();
        // Deliberately not `true`: a stream cannot be resynchronised after a framing error, so the
        // refusal is the last thing this connection hears. Returning the write's own result closes it
        // as soon as that refusal has drained, and leaves it to the flush deadline if the peer will not
        // read even that.
        return write_connection(connection);
      }
      needed = kFrameHeaderBytes + declared;
    }

    if (connection.in.size() >= needed)
    {
      const std::uint32_t length = frame_length(connection.in.data());
      const std::string body(connection.in.data() + kFrameHeaderBytes, length);
      connection.in.erase(connection.in.begin(),
                          connection.in.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes + length));
      std::string reply = handle_request(body, connection.session);
      if (connection.session.auth_failures >= kMaxAuthFailures)
        connection.close_after_flush = true;
      queue_reply(connection, std::move(reply));
      if (connection.close_after_flush)
      {
        connection.has_flush_deadline = true;
        connection.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kFlushDeadlineMs);
      }
      if (!write_connection(connection))
        return false;
      if (!connection.out.empty())
        return true; // backpressure, as above
      continue;
    }

    // Read exactly what the message in flight still needs, which is what bounds this buffer at one
    // message plus its header rather than at whatever a peer feels like sending.
    const std::size_t before = connection.in.size();
    connection.in.resize(before + (needed - before));
    const ssize_t got = ::recv(connection.fd, connection.in.data() + before, needed - before, 0);
    if (got > 0)
    {
      connection.in.resize(before + static_cast<std::size_t>(got));
      continue;
    }
    connection.in.resize(before);
    if (got == 0)
      return false; // the peer closed, possibly mid-message: a client that vanishes is not an error
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return true;
    return false;
  }
}

bool Daemon::read_connection(Connection& connection)
{
  return peer_is_framed(connection.session.peer) ? read_framed(connection) : read_datagrams(connection);
}

bool Daemon::push_slot(uint32_t chain, uint32_t chain_count, PlanSlot* slot)
{
  PlanCommand command;
  command.kind = PlanCommand::Kind::PublishPlan;
  command.slot = slot;
  command.chain = chain;
  command.chain_count = chain_count;
  if (commands_.push(command))
    return true;
  // A full command ring means the render thread is not draining, which is an engine bug rather than
  // a load condition. Free the plan here rather than leaking it.
  delete slot;
  if (config_.verbose)
    std::fprintf(stderr, "[namchaind] publication ring is full; plan dropped\n");
  return false;
}

int Daemon::find_chain(const std::string& chain_id) const
{
  for (std::size_t i = 0; i < current_document_.chains.size(); ++i)
  {
    if (current_document_.chains[i].id == chain_id)
      return static_cast<int>(i);
  }
  return -1;
}

int Daemon::find_chain_with_node(const std::string& node) const
{
  int found = -1;
  for (std::size_t i = 0; i < published_slots_.size(); ++i)
  {
    const PlanSlot* slot = published_slots_[i];
    if (slot == nullptr || slot->plan == nullptr)
      continue;
    const std::vector<std::string>& ids = slot->plan->node_ids();
    if (std::find(ids.begin(), ids.end(), node) == ids.end())
      continue;
    if (found >= 0)
      return -2; // more than one chain holds it, and guessing would edit an arbitrary one
    found = static_cast<int>(i);
  }
  return found;
}

bool Daemon::publish_param(const std::string& chain_id, const std::string& node, int port,
                           const std::string& param, double value, std::string& error)
{
  int index = -1;
  if (!chain_id.empty())
  {
    index = find_chain(chain_id);
    if (index < 0)
    {
      error = "no chain '" + chain_id + "'";
      return false;
    }
  }
  else
  {
    index = find_chain_with_node(node);
    if (index == -2)
    {
      error = "node '" + node + "' is in more than one chain; name the chain";
      return false;
    }
    if (index < 0)
    {
      error = "no chain holds a node '" + node + "'";
      return false;
    }
  }

  PlanSlot* slot = published_slots_[static_cast<std::size_t>(index)];
  if (slot == nullptr || slot->plan == nullptr)
  {
    error = "no plan is loaded; rig.load first";
    return false;
  }
  if (!std::isfinite(value))
  {
    error = "'" + node + "." + param + "' is not finite";
    return false;
  }

  // resolve_param reads immutable plan structure only, so doing it on the control thread while the
  // render thread runs the same plan is safe. The *write* goes on the ring and happens on the render
  // thread; that is the whole point of the command.
  std::size_t node_index = 0;
  uint32_t param_id = 0;
  if (!slot->plan->resolve_param(node, param, port, node_index, param_id))
  {
    error = port < 0 ? "node '" + node + "' has no settable param '" + param + "'"
                     : "node '" + node + "' port " + std::to_string(port) + " has no settable param '" +
                         param + "'";
    return false;
  }

  PlanCommand command;
  command.kind = PlanCommand::Kind::SetParam;
  command.slot = slot; // compared on the render thread, never dereferenced
  command.chain = static_cast<uint32_t>(index);
  command.generation = slot->generation;
  command.node = static_cast<uint32_t>(node_index);
  command.param = param_id;
  command.value = static_cast<float>(value);
  if (!commands_.push(command))
  {
    error = "the command ring is full; the render thread is not consuming edits";
    return false;
  }
  return true;
}

bool Daemon::load_rig(const std::string& rig_path, std::string& error, std::string* error_code,
                      bool admit_over_budget)
{
  graph::Document document;
  if (!graph::load_document_json(rig_path, document, error))
    return false;
  return publish_document(document, rig_path, "file", std::filesystem::path(rig_path).parent_path().string(),
                          error, error_code, admit_over_budget);
}

void Daemon::republish_after_rescan()
{
  if (missing_assets_.empty() || current_document_.chains.empty())
    return;
  // A copy: publishing assigns current_document_, and must not read from what it is overwriting.
  const graph::Document document = current_document_;
  std::string error;
  std::string code;
  // Nobody asked for this publish, so nobody can be told "no": a model that has arrived and puts the
  // document over the model budget plays, and says so, rather than staying stood in for.
  if (publish_document(document, current_rig_, current_source_, current_base_dir_, error, &code,
                       /*admit_over_budget=*/true))
  {
    if (config_.verbose)
      std::fprintf(stderr, "[namchaind] library rescanned: %zu asset%s still missing\n", missing_assets_.size(),
                   missing_assets_.size() == 1 ? "" : "s");
  }
  else
    std::fprintf(stderr, "[namchaind] library rescanned, but the rig did not republish: %s\n", error.c_str());
}

bool Daemon::publish_document(const graph::Document& document, const std::string& path, const std::string& source,
                              const std::string& base_dir, std::string& error, std::string* error_code,
                              bool admit_over_budget)
{
  const graph::Validation validation = graph::validate(document);
  if (!validation.ok)
  {
    error = validation.message;
    return false;
  }
  if (document.chains.size() > static_cast<std::size_t>(kMaxChains))
  {
    error = "a document may hold at most " + std::to_string(kMaxChains) + " chains, this one has " +
            std::to_string(document.chains.size());
    return false;
  }

  // Model and IR references are resolved on a *copy*: the document the engine remembers and hands
  // back is the one the client wrote, so applying what `rig.get` returned is a fixed point rather
  // than a rewrite of every reference -- and a client keeps seeing its own ids rather than this
  // device's paths (docs/assets.md).
  //
  // The order is worth stating, because the two steps can both answer. A reference that names a file
  // relative to the document is resolved first, exactly as it was before the registry existed, so a
  // hand-written rig that names the model sitting beside it keeps meaning that model. What is left
  // is looked up in the library: an id, or a relative filename that matches one exactly. The
  // alternative -- the library first -- would let a library entry shadow the file a rig was written
  // against, and would make the daemon and the offline tools resolve the same rig differently.
  graph::Document resolved = document;
  graph::resolve_relative_paths(resolved, base_dir);
  std::vector<assets::MissingAsset> missing;
  if (!assets::resolve_document(resolved, assets_, base_dir, error, &missing))
  {
    if (error_code != nullptr)
      *error_code = "asset_missing";
    return false;
  }

  // The model budget, on the resolved copy too: what it slims is a different model compiled from the
  // same reference, so the chains it touches compare as changed below, and the document a client
  // wrote still names what it named (docs/daemon.md, "Model budget").
  //
  // Two ceilings, because the render path has two (docs/daemon.md, "Render threads"): one core's for
  // any single chain, since a chain is executed by one thread and cannot be split, and one core's per
  // core the chains can actually be spread over for the document. The spread is bounded by the chain
  // count: an idle core has no L2 to lend, so a four-core engine rendering a one-chain document still
  // has one core's budget.
  const std::size_t per_core_bytes = static_cast<std::size_t>(config_.model_budget_kb) * 1024u;
  const std::size_t spread =
    std::min<std::size_t>(std::max(1u, config_.render_threads), std::max<std::size_t>(1, resolved.chains.size()));
  BudgetOutcome budget = apply_model_budget(resolved, per_core_bytes, per_core_bytes * spread,
                                            static_cast<int>(config_.period_size), working_sets_);
  for (BudgetedModel& model : budget.models)
    model.reference = string_param(document.chains[model.chain_index].graph.nodes[model.node_index], "model");
  if (budget.over && !admit_over_budget)
  {
    // Which ceiling was hit decides what the user has to do about it, so the message names it: a chain
    // that is too heavy is one model to replace, a document that is too heavy is one chain too many.
    const bool chain_over = budget.heaviest_chain_bytes > budget.per_chain_bytes;
    error = "the models in this document read " + format_kib(budget.used_bytes) +
            " per block together, more than the model budget of " + format_kib(budget.budget_bytes);
    if (chain_over)
    {
      error += ", and chain '" + resolved.chains[budget.heaviest_chain].id + "' alone reads " +
               format_kib(budget.heaviest_chain_bytes) + " against " +
               format_kib(budget.per_chain_bytes) + " for one core";
    }
    error += ", with every one that can be slimmed slimmed: " + describe_models(budget, document.chains.size() > 1);
    if (error_code != nullptr)
      *error_code = "over_budget";
    return false;
  }

  render::CompileOptions options;
  options.max_quantum = static_cast<int>(config_.period_size);
  options.sample_rate = static_cast<double>(config_.sample_rate);

  // Which chains actually changed. An unchanged chain keeps the plan it has: republishing it would
  // recompile it -- reloading every model on this thread -- and crossfade it, for an edit somebody
  // made to a different chain.
  //
  // Compared as resolved, not as written: a chain whose model has since arrived in the library (or
  // gone from it) reads the same but compiles to something else.
  std::vector<bool> changed(document.chains.size(), true);
  std::size_t changed_count = document.chains.size();
  if (resolved.chains.size() == current_resolved_.chains.size())
  {
    changed_count = 0;
    for (std::size_t i = 0; i < resolved.chains.size(); ++i)
    {
      const graph::Chain& before = current_resolved_.chains[i];
      const graph::Chain& after = resolved.chains[i];
      changed[i] = before.id != after.id || before.name != after.name ||
                   before.input_channel != after.input_channel ||
                   before.output_channel != after.output_channel || !graph::same_graph(before.graph, after.graph);
      if (changed[i])
        ++changed_count;
    }
  }

  if (changed_count == 0 && published_slots_.size() == document.chains.size())
  {
    // Nothing about the running document would change. Where it came from is still worth recording --
    // it is what a later patch republishes under -- but the generation does not advance, because no
    // new plan exists and a client that polls for adoption should not see one.
    current_rig_ = path;
    current_source_ = source;
    current_base_dir_ = base_dir;
    current_name_ = document.name;
    current_document_ = document;
    missing_assets_ = std::move(missing);
    model_budget_ = std::move(budget);
    last_transitions_.clear();
    return true;
  }

  for (const graph::Chain& chain : document.chains)
  {
    if (chain.input_channel >= static_cast<int>(config_.capture_channels))
    {
      error = "chain '" + chain.id + "' reads capture channel " + std::to_string(chain.input_channel) +
              ", but capture_channels is " + std::to_string(config_.capture_channels);
      return false;
    }
    if (chain.output_channel >= static_cast<int>(config_.playback_channels))
    {
      error = "chain '" + chain.id + "' writes playback channel " + std::to_string(chain.output_channel) +
              ", but playback_channels is " + std::to_string(config_.playback_channels);
      return false;
    }
  }

  // A crossfade is the same length wherever it is: inside a chain for a local transition, or across the
  // whole chain for the render thread's own.
  unsigned rate = active_rate_.load();
  if (rate == 0)
    rate = config_.sample_rate;
  const int fade_frames = std::clamp(
    static_cast<int>(std::llround(config_.fade_ms * static_cast<double>(rate) / 1000.0)), 1, kMaxTransitionFrames);

  const uint64_t generation = generation_ + 1;
  const std::size_t count = document.chains.size();
  // Per chain: the slot to push (none for a chain that is unchanged, or that only ramps), and what the
  // chain's published plan becomes. Ramps are pushed after every plan, resolved against the plan whose
  // node indices they carry.
  std::vector<std::unique_ptr<PlanSlot>> built(count);
  std::vector<PlanSlot*> finals(count, nullptr);
  std::vector<PlanCommand> ramp_commands;
  std::vector<ChainTransition> transitions;
  std::size_t plans_built = 0;

  const auto resolve_ramps = [&](std::size_t index, PlanSlot* slot, const std::vector<graph::CarriedRamp>& ramps,
                                 std::vector<PlanCommand>& commands, std::string& why) {
    for (const graph::CarriedRamp& ramp : ramps)
    {
      std::size_t node_index = 0;
      uint32_t param_id = 0;
      if (!slot->plan->resolve_param(ramp.node, ramp.param, -1, node_index, param_id))
      {
        why = "'" + ramp.node + "." + ramp.param + "' cannot be ramped";
        return false;
      }
      PlanCommand command;
      command.kind = PlanCommand::Kind::SetParam;
      command.slot = slot;
      command.chain = static_cast<uint32_t>(index);
      command.generation = slot->generation;
      command.node = static_cast<uint32_t>(node_index);
      command.param = param_id;
      command.value = static_cast<float>(ramp.value);
      commands.push_back(command);
    }
    return true;
  };
  const auto resolve_bypasses = [&](std::size_t index, PlanSlot* slot,
                                    const std::vector<graph::CarriedBypass>& bypasses,
                                    std::vector<PlanCommand>& commands, std::string& why) {
    for (const graph::CarriedBypass& bypass : bypasses)
    {
      const int node_index = slot->plan->index_of(bypass.node);
      if (node_index < 0 || !slot->plan->node_bypassable(static_cast<std::size_t>(node_index)))
      {
        why = "'" + bypass.node + "' cannot be bypassed as a ramp";
        return false;
      }
      PlanCommand command;
      command.kind = PlanCommand::Kind::SetBypass;
      command.slot = slot;
      command.chain = static_cast<uint32_t>(index);
      command.generation = slot->generation;
      command.node = static_cast<uint32_t>(node_index);
      command.param = static_cast<uint32_t>(fade_frames);
      command.value = bypass.bypass ? 1.0f : 0.0f;
      commands.push_back(command);
    }
    return true;
  };

  for (std::size_t i = 0; i < count; ++i)
  {
    if (!changed[i])
      continue;
    const graph::Chain& chain = resolved.chains[i];
    ChainTransition record;
    record.chain = chain.id;

    // A local transition keeps what the edit did not touch and fades only what it changed
    // (docs/transitions.md, "Local transitions"). It shares instances with the plan the render thread is
    // playing for this chain, so it is planned only against that plan, settled, and only when it is the
    // same chain: a different chain in the same position is a different rig, and is crossfaded whole.
    PlanSlot* base = i < published_slots_.size() ? published_slots_[i] : nullptr;
    const bool same_chain = base != nullptr && base->plan != nullptr && i < current_resolved_.chains.size() &&
                            current_resolved_.chains[i].id == chain.id &&
                            base->input_channel == chain.input_channel &&
                            base->output_channel == chain.output_channel;
    bool planned = false;
    if (same_chain)
    {
      std::string why;
      graph::Graph before;
      graph::Graph after;
      graph::LocalPlan local;
      std::set<std::string> unusable;
      for (std::size_t n = 0; n < base->plan->node_ids().size(); ++n)
      {
        if (base->plan->node_failed(n))
          unusable.insert(base->plan->node_ids()[n]);
      }
      // Room is left for every other chain's plan, so a chain that ramps cannot crowd one out.
      const std::size_t room = commands_.capacity() - std::min(commands_.capacity(), commands_.size() + count);
      if (!wait_until_settled(i))
      {
        why = "the chain was still changing";
      }
      else if (!graph::lower_graph(current_resolved_.chains[i].graph, before, why) ||
               !graph::lower_graph(chain.graph, after, why))
      {
        // The whole-chain compile below meets the same problem and reports it.
      }
      else if (graph::plan_local_transition(before, after, unusable, fade_frames, local, why))
      {
        if (local.ramps_only)
        {
          std::vector<PlanCommand> commands;
          if (ramp_commands.size() + local.ramps.size() + local.bypasses.size() > room)
            why = "too many values moved at once to ramp them";
          else if (resolve_ramps(i, base, local.ramps, commands, why) &&
                   resolve_bypasses(i, base, local.bypasses, commands, why))
          {
            ramp_commands.insert(ramp_commands.end(), commands.begin(), commands.end());
            finals[i] = base;
            record.kind = "ramps";
            record.ramps = static_cast<int>(local.ramps.size());
            record.bypasses = static_cast<int>(local.bypasses.size());
            planned = true;
          }
        }
        else
        {
          auto first = std::make_unique<PlanSlot>(kArenaBytes);
          std::unique_ptr<PlanSlot> second = local.instant ? nullptr : std::make_unique<PlanSlot>(kArenaBytes);
          graph::CarriedPlans plans;
          if (!graph::build_carried_plans(*base->plan, local, options, first->arena,
                                          local.instant ? first->arena : second->arena, plans, why))
          {
            why = "the local transition did not compile: " + why;
          }
          else
          {
            first->local = true;
            first->base = base;
            first->generation = generation;
            first->input_channel = chain.input_channel;
            first->output_channel = chain.output_channel;
            first->on_adopt = std::move(plans.on_adopt);
            PlanSlot* final_slot = first.get();
            if (local.instant)
            {
              first->plan = std::move(plans.final_plan);
            }
            else
            {
              first->plan = std::move(plans.transitional);
              first->fade_frames = fade_frames;
              first->on_then = std::move(plans.on_final);
              second->plan = std::move(plans.final_plan);
              second->generation = generation;
              second->input_channel = chain.input_channel;
              second->output_channel = chain.output_channel;
              final_slot = second.get();
              first->then = second.release();
            }
            std::vector<PlanCommand> commands;
            if (ramp_commands.size() + local.ramps.size() > room)
              why = "too many values moved at once to ramp them";
            else if (resolve_ramps(i, final_slot, local.ramps, commands, why))
            {
              ramp_commands.insert(ramp_commands.end(), commands.begin(), commands.end());
              finals[i] = final_slot;
              built[i] = std::move(first);
              ++plans_built;
              record.kind = local.instant ? "instant" : "local";
              record.fades = local.fades;
              record.kept = local.kept;
              record.created = local.created;
              record.retiring = local.retiring;
              record.ramps = static_cast<int>(local.ramps.size());
              planned = true;
            }
          }
        }
      }
      if (!planned)
        record.reason = why;
    }

    if (!planned)
    {
      // The whole chain, as it always was: a fresh plan, dual-rendered and crossfaded by the render thread.
      auto slot = std::make_unique<PlanSlot>(kArenaBytes);
      slot->plan = graph::RenderPlan::compile(chain.graph, options, slot->arena, error);
      if (slot->plan == nullptr)
      {
        error = "chain '" + chain.id + "': " + error;
        return false;
      }
      // The first process() call on a freshly built node is tens of times its steady-state cost, and a
      // stall that long can drain the device ring. It is control-thread work, before publication (§3.3).
      slot->plan->prewarm();
      slot->input_channel = chain.input_channel;
      slot->output_channel = chain.output_channel;
      slot->generation = generation;
      finals[i] = slot.get();
      built[i] = std::move(slot);
      ++plans_built;
      record.kind = "crossfade";
    }
    transitions.push_back(std::move(record));
  }

  // Room for every chain's command before any of them is pushed: a document publish is all-or-nothing,
  // and half a document installed is the one outcome worse than a rejected one.
  if (commands_.size() + plans_built + ramp_commands.size() > commands_.capacity())
  {
    error = "the command ring is full; the render thread is not consuming plans";
    return false;
  }

  // Every plan exists, is prewarmed and is bound before any of them becomes visible. The pushes cannot
  // fail: the room for all of them was checked above and this thread is the only producer.
  std::vector<PlanSlot*> raw;
  raw.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
  {
    if (built[i] == nullptr)
    {
      // Unchanged, or only ramped: the plan it has, still owned by the render thread or the reclaimer.
      raw.push_back(changed[i] ? finals[i] : published_slots_[i]);
      continue;
    }
    if (!push_slot(static_cast<uint32_t>(i), static_cast<uint32_t>(count), built[i].release()))
    {
      error = "the publication ring is full; the render thread is not consuming plans";
      return false;
    }
    raw.push_back(finals[i]);
  }
  // The ramps go after the plans, so one resolved against a local transition's final plan finds that
  // plan already taken up (or queued) when the render thread reaches it.
  for (const PlanCommand& command : ramp_commands)
    commands_.push(command);

  // Only now does anything about the running engine change. A failed compile, a chain whose channels
  // are not available, or a full ring leaves the previous document, plans and generation as they were.
  // A publish that only ramped built no plan, so the generation does not move: there is nothing new for
  // a client that polls for adoption to wait for.
  if (plans_built > 0)
    generation_ = generation;
  current_rig_ = path;
  current_source_ = source;
  current_base_dir_ = base_dir;
  current_name_ = document.name;
  current_document_ = document;
  current_resolved_ = std::move(resolved);
  missing_assets_ = std::move(missing);
  model_budget_ = std::move(budget);
  published_slots_ = std::move(raw);
  last_published_ = &current_document_;
  last_transitions_ = std::move(transitions);

  if (config_.verbose)
  {
    const std::string what = path.empty() ? (document.name.empty() ? "(applied document)" : document.name) : path;
    std::string how;
    for (const ChainTransition& transition : last_transitions_)
    {
      how += how.empty() ? "" : ", ";
      how += transition.kind;
      if (transition.kind == "local")
        how += " (" + std::to_string(transition.fades) + " fade" + (transition.fades == 1 ? "" : "s") + ", " +
               std::to_string(transition.kept) + " kept, " + std::to_string(transition.created) + " new, " +
               std::to_string(transition.retiring) + " retiring)";
      if (!transition.reason.empty())
        how += " [" + transition.reason + "]";
    }
    std::fprintf(stderr, "[namchaind] published %s (generation %llu, %zu chain%s: %s)\n", what.c_str(),
                 static_cast<unsigned long long>(generation_), count, count == 1 ? "" : "s", how.c_str());
  }
  // What the budget did is logged whether or not the daemon is verbose: a slimmed model sounds
  // different from the one the rig names, and a document over the budget may miss deadlines.
  if (model_budget_.over)
  {
    std::fprintf(stderr,
                 "[namchaind] model budget: the document reads %s per block, over the budget of %s, and plays "
                 "anyway: %s\n",
                 format_kib(model_budget_.used_bytes).c_str(), format_kib(model_budget_.budget_bytes).c_str(),
                 describe_models(model_budget_, count > 1).c_str());
  }
  else if (budget_slimmed(model_budget_))
  {
    std::fprintf(stderr, "[namchaind] model budget: %s of %s, as slimmed: %s\n",
                 format_kib(model_budget_.used_bytes).c_str(), format_kib(model_budget_.budget_bytes).c_str(),
                 describe_models(model_budget_, count > 1).c_str());
  }
  return true;
}

bool Daemon::chain_settled(std::size_t index) const
{
  if (index >= published_slots_.size() || index >= static_cast<std::size_t>(kMaxChains) ||
      published_slots_[index] == nullptr)
    return false;
  const uint64_t generation = published_slots_[index]->generation;
  const ChainRuntime& chain = chains_[index];
  return !chain.transition_active.load() && chain.audible_generation.load() == generation &&
         chain.target_generation.load() == generation;
}

bool Daemon::wait_until_settled(std::size_t index)
{
  if (chain_settled(index))
    return true;
  // Only a running render thread moves a chain along: with no device there is nothing to wait for.
  if (device_state() != DeviceState::Running)
    return false;
  // Long enough for every chain of a document to finish its fade in turn, short enough that a client is
  // not kept waiting for a reply when something is wrong.
  const auto limit = std::chrono::milliseconds(
    50 + static_cast<long long>(std::ceil(config_.fade_ms)) * 2 * static_cast<long long>(kMaxChains));
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // This thread is also the one that frees retired plans, and a fade retires them.
    reclaimer_.collect();
    if (chain_settled(index))
      return true;
  }
  return false;
}

bool Daemon::start(std::string& error)
{
  if (started_)
  {
    error = "the daemon is already started";
    return false;
  }
  if (config_.sample_rate == 0 || config_.period_size == 0 || config_.periods == 0)
  {
    error = "sample_rate, period_size and periods must all be non-zero";
    return false;
  }
  if (config_.capture_channels == 0 || config_.playback_channels == 0)
  {
    error = "capture_channels and playback_channels must both be non-zero";
    return false;
  }
  if (config_.rt_priority < 0 || config_.rt_priority > 99)
  {
    error = "rt_priority must be in [0, 99]";
    return false;
  }
  if (config_.render_threads > 1)
  {
    // Refused here rather than discovered by a worker that cannot pin itself: a pool that runs on
    // fewer cores than it was asked for is a different claim from one that runs on the cores the
    // configuration names, and the timing of the two is not comparable (docs/multi-core.md).
    if (config_.cpu < 0)
    {
      error = "render_threads above 1 needs cpu: the workers take the cores above it";
      return false;
    }
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (::sched_getaffinity(0, sizeof(allowed), &allowed) == 0)
    {
      std::string missing;
      for (unsigned i = 0; i < config_.render_threads; ++i)
      {
        const int cpu = config_.cpu + static_cast<int>(i);
        if (cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &allowed))
          missing += (missing.empty() ? "" : ", ") + std::to_string(cpu);
      }
      if (!missing.empty())
      {
        error = "render_threads " + std::to_string(config_.render_threads) + " from cpu " +
                std::to_string(config_.cpu) + " needs cpus this process may not run on: " + missing +
                " (cgroup cpuset, or a cpu the kernel does not have)";
        return false;
      }
    }
  }
  if (!(config_.fade_ms >= 0.0))
  {
    error = "fade_ms must be >= 0";
    return false;
  }
  if (config_.output_queue_floor_periods > config_.output_queue_periods)
  {
    error = "output_queue_floor_periods cannot exceed output_queue_periods";
    return false;
  }
  if (config_.output_queue_periods != 0 && config_.output_queue_periods >= config_.periods)
  {
    // A cap at or above the ring's capacity can never bind, so it is a configuration mistake rather
    // than a no-op: the ring itself becomes the limit and the number means nothing.
    error = "output_queue_periods must be less than periods";
    return false;
  }
  if (config_.tcp_listen)
  {
    if (config_.tcp_max_connections == 0)
    {
      error = "tcp_max_connections must be at least 1";
      return false;
    }
    // A cap small enough to be a mistake is refused here rather than accepted and then never met: a
    // message has to hold a document, and one much over a few megabytes is a client bug that would
    // otherwise cost every connection a buffer of that size.
    if (config_.tcp_max_message_bytes < 1024 || config_.tcp_max_message_bytes > (64u << 20))
    {
      error = "tcp_max_message_bytes must be between 1024 and 67108864";
      return false;
    }
    if (config_.tcp_port > 65535)
    {
      error = "tcp_port must be a valid port number (0 picks one)";
      return false;
    }
  }
  // The token is resolved before anything is bound, so a daemon asked for a listener it cannot
  // authenticate never gets as far as opening one.
  if (!resolve_tcp_token(config_, error))
    return false;

  // The library is scanned before the startup rig is compiled, because compiling that rig is the
  // first thing that may need it. A root that is missing is not fatal here: it is an empty library,
  // and engine.status says so through `assets`.
  assets_.configure(config_.models_root, config_.irs_root);
  assets_.refresh();

  if (!setup_signals(error))
    return false;
  if (!bind_tcp(error))
  {
    close_transport();
    return false;
  }
  if (!bind_socket(error))
  {
    close_transport();
    return false;
  }

  // The startup rig is compiled before the clock exists, so the first plan is there when the first
  // block runs (rig-pipeline.md §5.1 item 2). A failure here is fatal: the operator named a rig.
  if (!config_.rig.empty())
  {
    std::string path;
    // Admitted over the model budget, with a warning: refusing the configured rig would leave a pedal
    // with no sound at all, which no budget is worth.
    if (!resolve_startup_rig(config_, config_.rig, path, error) ||
        !load_rig(path, error, nullptr, /*admit_over_budget=*/true))
    {
      // Both listeners are already up. Leaving them bound would mean the next start() -- the test
      // suite does exactly this -- trips over its own socket, which turns one clear failure into two
      // confusing ones.
      close_transport();
      return false;
    }
  }

  running_.store(true);
  render_thread_ = std::thread([this] { render_loop(); });
  started_ = true;
  return true;
}

void Daemon::stop()
{
  stop_requested_.store(true);
  running_.store(false);
  if (render_thread_.joinable())
    render_thread_.join();
  // After the join, so nothing can be pushing; the files are closed properly on a clean shutdown.
  end_recording();

  // Anything published but never adopted (a plan loaded before the render thread drained it) is
  // still owned by the command ring. Free it here; the loop has stopped, so nothing else can.
  PlanCommand command;
  while (commands_.pop(command))
    if (command.kind == PlanCommand::Kind::PublishPlan)
      delete command.slot;

  for (int i = 0; i < kMaxChains; ++i)
  {
    for (PlanSlot** slot : {&chains_[i].current, &chains_[i].next, &chains_[i].pending})
    {
      if (*slot == nullptr)
        continue;
      delete *slot;
      *slot = nullptr;
    }
  }
  chain_count_ = 0;
  transitioning_chain_ = -1;
  transition_active_.store(false);
  audible_generation_.store(0);
  target_generation_.store(0);
  adopted_generation_.store(0);
  failed_nodes_.store(0);
  guard_trips_.store(0);
  last_published_ = nullptr;
  close_transport();
  reclaimer_.collect();
  started_ = false;
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------

void Daemon::set_device_state(DeviceState state)
{
  const int prior = device_state_.exchange(static_cast<int>(state));
  if (prior == static_cast<int>(state))
    return;
  if (state == DeviceState::Lost)
    device_lost_.fetch_add(1);
  else if (state == DeviceState::Running && prior == static_cast<int>(DeviceState::Lost))
    device_restored_.fetch_add(1);
}

bool Daemon::open_device(std::string& error)
{
  BackendConfig backend;
  backend.sample_rate = config_.sample_rate;
  backend.period_size = config_.period_size;
  backend.periods = config_.periods;
  backend.capture_channels = config_.capture_channels;
  backend.playback_channels = config_.playback_channels;
  backend.device = config_.device;
  backend.require_exact_period = true; // §9.5: a silently rounded quantum is not acceptable
  backend.output_queue_periods = config_.output_queue_periods;
  backend.output_queue_floor_periods = config_.output_queue_floor_periods;
  backend.input_path = config_.capture_path;
  backend.output_path = config_.playback_path;

  device_ = open_backend(backend, config_.backend, error);
  if (device_ == nullptr)
    return false;

  const BackendInfo& info = device_->info();
  // The plan was compiled for the configured quantum and rate. A backend that got something else
  // would run frames past every node's buffer, so refuse it rather than adopt it.
  if (info.period_size != config_.period_size || info.sample_rate != config_.sample_rate)
  {
    error = "device gave " + std::to_string(info.period_size) + " frames at " +
            std::to_string(info.sample_rate) + " Hz, configured for " + std::to_string(config_.period_size) +
            " at " + std::to_string(config_.sample_rate) + " Hz";
    close_device();
    return false;
  }
  if (info.capture_bytes_per_frame == 0 || info.playback_bytes_per_frame == 0)
  {
    error = "device reports a zero bytes-per-frame";
    close_device();
    return false;
  }
  // A chain may read any channel the *configuration* promised, and each chain's channel was checked
  // against that at publish time. A device that gives fewer would make those reads read past the end
  // of a frame, so it is refused rather than silently remapped (the same argument as the period size).
  if (info.capture_channels < config_.capture_channels)
  {
    error = "device gave " + std::to_string(info.capture_channels) + " capture channels, configured for " +
            std::to_string(config_.capture_channels);
    close_device();
    return false;
  }
  if (info.playback_channels < config_.playback_channels)
  {
    error = "device gave " + std::to_string(info.playback_channels) + " playback channels, configured for " +
            std::to_string(config_.playback_channels);
    close_device();
    return false;
  }

  // Sized before the stream starts, and before mlockall covers them.
  device_finished_ = false;
  capture_buffer_.assign(static_cast<std::size_t>(config_.period_size) * info.capture_bytes_per_frame, 0);
  playback_buffer_.assign(static_cast<std::size_t>(config_.period_size) * info.playback_bytes_per_frame, 0);
  bus_.assign(static_cast<std::size_t>(config_.period_size) * info.playback_channels, 0.0f);
  for (ChainRuntime& chain : chains_)
  {
    chain.in.assign(config_.period_size, 0.0f);
    chain.out.assign(config_.period_size, 0.0f);
    chain.old.assign(config_.period_size, 0.0f);
    chain.incoming.assign(config_.period_size, 0.0f);
  }

  active_rate_.store(info.sample_rate);
  active_period_.store(info.period_size);
  active_capture_channels_.store(info.capture_channels);
  active_playback_channels_.store(info.playback_channels);
  active_format_.store(static_cast<int>(info.format));
  // A new backend counts from zero, as its xruns do.
  last_queue_ = StreamQueueStats{};
  return true;
}

void Daemon::account_period()
{
  const StreamQueueStats queue = device_->queue_stats();
  output_drops_.store(queue.output_drops);
  output_target_drops_.store(queue.output_target_drops);
  floor_insertions_.store(queue.floor_insertions);
  capture_bursts_.store(queue.capture_bursts);

  // Busy before the load, both sequentially consistent: if this thread saw the recorder, then
  // end_recording's store of null came later and its wait will see busy (recorder.h).
  recorder_busy_.store(true);
  Recorder* recorder = recorder_live_.load();
  if (recorder != nullptr)
  {
    const BackendInfo& info = device_->info();
    RecorderEvents events;
    events.dropped = static_cast<uint32_t>(queue.output_drops - last_queue_.output_drops);
    events.inserted = static_cast<uint32_t>(queue.floor_insertions - last_queue_.floor_insertions);
    events.bursts = static_cast<uint32_t>(queue.capture_bursts - last_queue_.capture_bursts);
    events.xruns = static_cast<uint32_t>(device_->driver_xruns() - last_xruns_);
    recorder->push(capture_buffer_.data(), static_cast<std::size_t>(info.period_size) * info.capture_bytes_per_frame,
                   playback_buffer_.data(),
                   static_cast<std::size_t>(info.period_size) * info.playback_bytes_per_frame, events);
  }
  recorder_busy_.store(false);
  last_queue_ = queue;
}

void Daemon::account_render_time(std::chrono::steady_clock::duration elapsed, const BackendInfo& info) noexcept
{
  if (info.sample_rate == 0)
    return;
  const uint64_t deadline = static_cast<uint64_t>(info.period_size) * 1'000'000'000ull / info.sample_rate;
  if (deadline == 0)
    return;
  deadline_ns_.store(deadline, std::memory_order_relaxed);
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  const uint64_t took = ns > 0 ? static_cast<uint64_t>(ns) : 0;
  const std::size_t bucket = std::min<uint64_t>(took * 100 / deadline, kLoadBuckets - 1);
  load_histogram_[bucket].fetch_add(1, std::memory_order_relaxed);
  if (took > worst_block_ns_.load(std::memory_order_relaxed))
    worst_block_ns_.store(took, std::memory_order_relaxed);
}

void Daemon::end_recording()
{
  if (recorder_ == nullptr || recorder_live_.load() == nullptr)
    return;
  recorder_live_.store(nullptr);
  while (recorder_busy_.load())
    std::this_thread::yield();
  // Stopped but kept: `engine.record` and `engine.status` still describe the last recording until the
  // next one starts.
  recorder_->stop();
  if (config_.verbose)
    std::fprintf(stderr, "[namchaind] recording '%s' finished: %llu periods, %llu dropped, %llu inserted\n",
                 recording_name_.c_str(), static_cast<unsigned long long>(recorder_->periods()),
                 static_cast<unsigned long long>(recorder_->dropped()),
                 static_cast<unsigned long long>(recorder_->inserted()));
}

void Daemon::finish_device(std::string& /*error*/)
{
  if (device_ == nullptr || device_finished_)
    return;
  device_finished_ = true;
  std::string reason;
  if (!device_->finish(reason) && config_.verbose)
    std::fprintf(stderr, "[namchaind] finishing the device failed: %s\n", reason.c_str());
}

void Daemon::close_device()
{
  if (device_ == nullptr)
    return;
  if (device_started_)
  {
    device_->stop();
    device_started_ = false;
  }
  device_.reset();
}

void Daemon::fail_device(const std::string& reason)
{
  if (config_.verbose)
    std::fprintf(stderr, "[namchaind] device: %s\n", reason.c_str());
  render_errors_.fetch_add(1);
  recent_render_errors_.fetch_add(1);
  close_device();
  set_device_state(DeviceState::Lost);
}

void Daemon::start_transition(int index, PlanSlot* next)
{
  ChainRuntime& chain = chains_[index];
  chain.next = next;
  unsigned rate = active_rate_.load();
  if (rate == 0)
    rate = config_.sample_rate;
  const int fade = static_cast<int>(std::llround(config_.fade_ms * static_cast<double>(rate) / 1000.0));
  chain.crossfade.start(fade);
  chain.target_generation.store(next->generation);
  // "Adopted" means the render thread has taken the plan up, which for a structural change is the
  // moment it starts fading in, not the moment the fade closes.
  chain.adopted_generation.store(next->generation);
  chain.transition_active.store(true);
  transitioning_chain_ = index;
  transitions_started_.fetch_add(1);
  update_aggregate_state();
}

void Daemon::finish_transition(int index)
{
  ChainRuntime& chain = chains_[index];
  if (chain.current != nullptr)
    reclaimer_.retire(chain.current, &delete_plan_slot);
  chain.current = chain.next;
  chain.next = nullptr;
  chain.transition_active.store(false);
  if (chain.current != nullptr)
    chain.audible_generation.store(chain.current->generation);
  if (transitioning_chain_ == index)
    transitioning_chain_ = -1;

  // A publish that arrived while this fade was in flight starts now, from the plan that has just
  // become audible. Nothing is lost and nothing nests: one structural transition at a time, so a
  // document apply works through its chains one after another rather than all at once.
  start_queued_transition();
  update_aggregate_state();
}

void Daemon::start_local_transition(int index, PlanSlot* slot)
{
  ChainRuntime& chain = chains_[index];
  chain.target_generation.store(slot->generation);
  chain.adopted_generation.store(slot->generation);
  transitions_started_.fetch_add(1);

  if (slot->base != chain.current)
  {
    // Planned against a plan that is not the one playing. The control thread plans a local transition
    // only against a settled chain, so this is not expected; but the slot shares instances with the plan
    // it was planned against, and running it beside anything else could run one of them twice. So go
    // straight to its final plan, which is safe whatever is playing, and count it.
    local_base_mismatches_.fetch_add(1);
    PlanSlot* final_slot = slot;
    if (slot->then != nullptr)
    {
      final_slot = slot->then;
      slot->then = nullptr;
      reclaimer_.retire(slot, &delete_plan_slot);
    }
    if (chain.current != nullptr)
      reclaimer_.retire(chain.current, &delete_plan_slot);
    chain.current = final_slot;
    chain.local_remaining = 0;
    chain.transition_active.store(false);
    chain.audible_generation.store(final_slot->generation);
    update_aggregate_state();
    return;
  }

  // The plan this one shares its instances with stops in the block this one starts: nothing runs twice.
  // A block-delay node's state is its input buffer, which lives in the plan, so that is copied across.
  const std::size_t floats = static_cast<std::size_t>(slot->plan->max_quantum());
  for (const auto& [from, to] : slot->on_adopt)
    std::copy(from, from + floats, to);
  if (chain.current != nullptr)
    reclaimer_.retire(chain.current, &delete_plan_slot);
  chain.current = slot;
  local_transitions_.fetch_add(1);

  if (slot->then != nullptr)
  {
    chain.local_remaining = std::max(1, slot->fade_frames);
    chain.transition_active.store(true);
    transitioning_chain_ = index;
  }
  else
  {
    // An instant one: nothing to fade, so this plan is already the final one.
    chain.local_remaining = 0;
    chain.transition_active.store(false);
    chain.audible_generation.store(slot->generation);
  }
  update_aggregate_state();
}

void Daemon::finish_local_transition(int index)
{
  ChainRuntime& chain = chains_[index];
  chain.local_remaining = 0;
  PlanSlot* finished = chain.current;
  if (finished != nullptr && finished->then != nullptr)
  {
    PlanSlot* final_slot = finished->then;
    finished->then = nullptr;
    const std::size_t floats = static_cast<std::size_t>(finished->plan->max_quantum());
    for (const auto& [from, to] : finished->on_then)
      std::copy(from, from + floats, to);
    chain.current = final_slot;
    // What was going away goes with the transitional plan; what was kept lives on in the final one.
    reclaimer_.retire(finished, &delete_plan_slot);
  }
  chain.transition_active.store(false);
  if (chain.current != nullptr)
    chain.audible_generation.store(chain.current->generation);
  if (transitioning_chain_ == index)
    transitioning_chain_ = -1;
  start_queued_transition();
  update_aggregate_state();
}

void Daemon::start_queued_transition()
{
  if (transitioning_chain_ >= 0)
    return;
  for (int i = 0; i < chain_count_; ++i)
  {
    ChainRuntime& chain = chains_[i];
    if (chain.pending == nullptr)
      continue;
    PlanSlot* pending = chain.pending;
    chain.pending = nullptr;
    if (pending->local)
      start_local_transition(i, pending);
    else
      start_transition(i, pending);
    return;
  }
  update_aggregate_state();
}

void Daemon::drop_chain(int index)
{
  ChainRuntime& chain = chains_[index];
  if (chain.current == nullptr && chain.next == nullptr && chain.pending == nullptr)
    return;

  // A chain the new document does not have stops now. Its plans are retired rather than faded: a
  // removal is not a transition from one plan to another, and fading it would mean inventing a
  // fade-to-silence path that nothing else needs (docs/ui-integration-plan.md §1.1). A client that
  // wants the removal to be gradual mutes the chain first, which is a ramp.
  if (chain.next != nullptr)
  {
    reclaimer_.retire(chain.next, &delete_plan_slot);
    chain.next = nullptr;
  }
  if (chain.pending != nullptr)
  {
    reclaimer_.retire(chain.pending, &delete_plan_slot);
    chain.pending = nullptr;
  }
  if (chain.current != nullptr)
  {
    reclaimer_.retire(chain.current, &delete_plan_slot);
    chain.current = nullptr;
  }
  chain.local_remaining = 0;
  chain.transition_active.store(false);
  chain.audible_generation.store(0);
  chain.target_generation.store(0);
  chain.adopted_generation.store(0);
  if (transitioning_chain_ == index)
    transitioning_chain_ = -1;
  start_queued_transition();
}

void Daemon::update_aggregate_state()
{
  // The document-level fields answer one question each: is anything still moving, and what is the
  // device actually hearing. `audible_generation` is therefore the document generation only once
  // every chain has settled at it, and the previous one while a staggered apply is still working.
  bool moving = false;
  unsigned fading = 0;
  for (int i = 0; i < chain_count_; ++i)
  {
    if (chains_[i].transition_active.load())
      ++fading;
    if (chains_[i].transition_active.load() || chains_[i].pending != nullptr)
      moving = true;
  }
  if (fading > max_concurrent_transitions_.load())
    max_concurrent_transitions_.store(fading);
  transition_active_.store(moving);
  audible_generation_.store(moving ? previous_generation_ : rendered_generation_);
  target_generation_.store(rendered_generation_);

  // "Adopted" is the document the render thread has taken up: every chain's command has been
  // processed, whether it started a fade or is waiting its turn.
  adopted_generation_.store(rendered_generation_);
}

void Daemon::drain_commands()
{
  bool published = false;
  PlanCommand command;
  while (commands_.pop(command))
  {
    if (command.kind == PlanCommand::Kind::SetParam || command.kind == PlanCommand::Kind::SetBypass)
    {
      // The command names the chain and the generation it was resolved against. During a transition
      // two plans are live for that chain, so the edit goes to whichever of them it names; a plan a
      // structural publish has already superseded is counted as stale rather than applied to the
      // wrong chain.
      if (command.chain >= static_cast<uint32_t>(kMaxChains))
      {
        params_stale_.fetch_add(1);
        continue;
      }
      // Matched by the slot it was resolved against, not only by generation: a local transition's two
      // plans share a generation, and a node's index differs between them. An edit resolved against a
      // local transition's final plan reaches it while the transitional plan is the one playing, which
      // is right -- the node is the same instance in both.
      ChainRuntime& chain = chains_[command.chain];
      const auto named = [&command](const PlanSlot* slot) {
        return slot != nullptr && slot == command.slot && slot->generation == command.generation;
      };
      graph::RenderPlan* plan = nullptr;
      if (named(chain.current))
        plan = chain.current->plan.get();
      else if (chain.current != nullptr && named(chain.current->then))
        plan = chain.current->then->plan.get();
      else if (named(chain.next))
        plan = chain.next->plan.get();
      else if (named(chain.pending))
        plan = chain.pending->plan.get();
      else if (chain.pending != nullptr && named(chain.pending->then))
        plan = chain.pending->then->plan.get();

      const bool applied =
        plan != nullptr && (command.kind == PlanCommand::Kind::SetParam
                              ? plan->set_param(command.node, command.param, command.value)
                              : plan->set_bypass(command.node, command.value > 0.5f, static_cast<int>(command.param)));
      if (applied)
        params_applied_.fetch_add(1);
      else
        params_stale_.fetch_add(1); // an unknown param is a control-side bug; counted, not fatal
      continue;
    }

    PlanSlot* next = command.slot;
    if (next == nullptr || command.chain >= static_cast<uint32_t>(kMaxChains))
      continue;

    // The document's shape comes from the commands themselves, so the render thread never reads a
    // count the control thread could move under it mid-drain. A slot from a generation this thread has
    // not seen is a new document: remember where the last settled one was, because
    // `audible_generation` reports that until every chain has moved (see update_aggregate_state).
    if (next->generation != rendered_generation_)
    {
      previous_generation_ = rendered_generation_;
      rendered_generation_ = next->generation;
    }
    if (command.chain_count > 0 && command.chain_count <= static_cast<uint32_t>(kMaxChains))
      chain_count_ = static_cast<int>(command.chain_count);
    published = true;

    const int index = static_cast<int>(command.chain);
    ChainRuntime& chain = chains_[index];

    if (chain.current == nullptr)
    {
      // Nothing to fade from: adopt it. This is the startup rig, and the first appearance of a chain
      // added by a document -- the device has not produced a block, or the chain is new. A local slot is
      // never planned against nothing, but if one arrives here its final plan is what to adopt.
      if (next->then != nullptr)
      {
        PlanSlot* final_slot = next->then;
        next->then = nullptr;
        reclaimer_.retire(next, &delete_plan_slot);
        next = final_slot;
      }
      chain.next = nullptr;
      chain.current = next;
      chain.audible_generation.store(next->generation);
      chain.adopted_generation.store(next->generation);
      chain.target_generation.store(next->generation);
      chain.transition_active.store(false);
      continue;
    }

    if (chain.next != nullptr || chain.local_remaining > 0 || transitioning_chain_ >= 0)
    {
      // This chain is fading, or another one is and this daemon fades one chain at a time. A burst
      // coalesces to the newest request (the superseded plan is retired here) rather than nesting
      // fades, which is what rig-pipeline.md §5.3 asks for -- and staggering is what keeps the peak
      // cost of a document apply at two of the largest chain instead of two of the whole document,
      // with no admission control to lean on.
      if (chain.pending != nullptr)
        reclaimer_.retire(chain.pending, &delete_plan_slot);
      chain.pending = next;
      chain.target_generation.store(next->generation);
      continue;
    }

    if (next->local)
      start_local_transition(index, next);
    else
      start_transition(index, next);
  }

  // A document with fewer chains than the last one: the chains that are gone stop now. Done after the
  // drain rather than per command, so a document is never half-dropped while its own commands are
  // still arriving.
  if (published)
  {
    for (int i = chain_count_; i < kMaxChains; ++i)
      drop_chain(i);
  }
  update_aggregate_state();
}

void Daemon::render_loop()
{
  const rt::SchedulingState scheduling = rt::apply_scheduling(config_.rt_priority, config_.cpu);
  rt_policy_.store(scheduling.policy);
  rt_priority_.store(scheduling.priority);
  rt_realtime_.store(scheduling.realtime);
  if (config_.verbose)
    std::fprintf(stderr, "[namchaind] render thread: %s\n", rt::describe_scheduling(scheduling).c_str());

  // The workers, before the first block: pinned, counted and warm. A pool that came up smaller than
  // asked for is reported here, in `engine.status` and in the report of whatever ran, never by
  // silence.
  std::string pool_error;
  if (!start_render_pool(pool_error))
  {
    std::fprintf(stderr, "[namchaind] %s\n", pool_error.c_str());
    render_pool_degraded_.store(true);
  }

  std::string error;
  bool memory_locked = false;
  constexpr int kDeviceWaitMs = 1000;

  while (running_.load())
  {
    if (device_ == nullptr)
    {
      if (!open_device(error))
      {
        set_device_state(DeviceState::Lost);
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1u, config_.device_retry_ms)));
        continue;
      }
      if (!device_->start(error))
      {
        close_device();
        set_device_state(DeviceState::Lost);
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1u, config_.device_retry_ms)));
        continue;
      }
      device_started_ = true;
      set_device_state(DeviceState::Running);
      // Last, so that the plan, the model and every stream buffer are covered by MCL_CURRENT.
      if (config_.lock_memory && !memory_locked)
      {
        memory_locked = lock_process_memory();
        memory_locked_.store(memory_locked);
        if (!memory_locked && config_.verbose)
          std::fprintf(stderr, "[namchaind] mlockall(MCL_CURRENT) refused; the run is not pinned\n");
      }
    }

    const BackendInfo& info = device_->info();
    if (info.capture_exhausted)
    {
      // A file backend that ran out of input. Not a device loss; there is simply no more audio.
      finish_device(error);
      close_device();
      set_device_state(DeviceState::Closed);
      break;
    }

    if (!device_->wait_capture(kDeviceWaitMs, error))
    {
      if (!running_.load())
        break;
      fail_device(error);
      continue;
    }
    const int captured = device_->read_capture(capture_buffer_.data(), info.period_size, 1000, error);
    if (captured < 0)
    {
      if (!running_.load())
        break;
      fail_device(error);
      continue;
    }
    if (captured == 0 && info.capture_exhausted)
    {
      // No frames, and there will be no more: rendering this period would write one period of
      // silence past the end of the input. That is the same artefact BackendInfo::needs_bringup_cycle
      // describes at the other end of the file -- "an output one period longer than the renderer's",
      // which would be blamed on the DSP -- and it is the reason a `--backend file` run could not be
      // compared with namchain-render without trimming it first. A short read is still rendered: its
      // tail is defined silence and it is the last real audio there is.
      continue;
    }
    // The block's clock starts once its input is in hand: waiting for the device is not render time.
    const auto block_start = std::chrono::steady_clock::now();

    {
      // The render path proper: everything here was bound before publication, so it must not touch
      // the allocator, take a lock or make a syscall (namchain/rt_check.h).
      RtScope scope;
      drain_commands();

      const std::size_t width = info.bytes_per_sample;
      const int frames = static_cast<int>(info.period_size);
      std::fill(bus_.begin(), bus_.end(), 0.0f);

      // 1. Every chain's input, on the coordinator. The decode is a per-sample conversion and a
      //    chain's plan cannot start before its input exists, so this is not worth sharing out.
      for (int c = 0; c < chain_count_; ++c)
      {
        ChainRuntime& chain = chains_[c];
        if (chain.current == nullptr)
          continue;

        // Which channel a chain reads is the *incoming* plan's business when a transition is running:
        // a document that moved a chain to another input channel means the new plan hears the new
        // channel from the moment it starts fading in, rather than a fade between two inputs.
        const PlanSlot* source = chain.next != nullptr ? chain.next : chain.current;
        const unsigned channel = static_cast<unsigned>(source->input_channel);
        for (unsigned i = 0; i < info.period_size; ++i)
          chain.in[i] = pcm_decode(capture_buffer_.data() +
                                     (static_cast<std::size_t>(i) * info.capture_channels + channel) * width,
                                   info.format);
      }

      // 2. The plans, on the pool: one participant per core, each taking every `participants`-th
      //    chain. With `render_threads` at 1 this is the same call it has always been, made inline.
      render_pool_.run(this, &Daemon::run_chains_entry, frames);

      // 3. Everything that must keep one owner, in the order it has always run: the crossfade mixes,
      //    the transition bookkeeping and the output bus. This is what makes a parallel render
      //    identical to a serial one rather than merely similar.
      for (int c = 0; c < chain_count_; ++c)
      {
        ChainRuntime& chain = chains_[c];
        if (chain.current == nullptr)
          continue;
        const PlanSlot* source = chain.next != nullptr ? chain.next : chain.current;

        if (chain.next != nullptr)
        {
          // Dual-render: both plans ran on the same participant, and their chain outputs are
          // equal-power crossfaded here. mix() has finished reading old, so the outgoing plan can be
          // retired; the control thread frees it after the reclaimer's grace window.
          chain.crossfade.mix(chain.old.data(), chain.incoming.data(), chain.out.data(), frames);
          if (!chain.crossfade.active())
            finish_transition(c);
        }
        else if (chain.local_remaining > 0)
        {
          // One plan, also during a local transition: its crossfades sit inside it, at the inputs the
          // edit changed, and everything the edit did not touch runs once.
          chain.local_remaining -= frames;
          if (chain.local_remaining <= 0)
            finish_local_transition(c);
        }

        // Mute is a zeroing of the *emitted* copy, applied after execute so the chain keeps running:
        // a muted chain whose inference is skipped freezes state and clicks on unmute (G14). It is
        // also why this is a skip rather than a reason not to execute at all.
        if (chain.mute.load())
          continue;

        float* destination = bus_.data() + static_cast<std::size_t>(source->output_channel) * info.period_size;
        for (int i = 0; i < frames; ++i)
          destination[i] += chain.out[static_cast<std::size_t>(i)];
      }

      // Global mute, after every chain has run, for the same reason a per-chain mute is a skip.
      if (mute_.load())
        std::fill(bus_.begin(), bus_.end(), 0.0f);

      // One encode for the whole bus: two chains writing one channel are summed, and pcm_encode
      // clamps, so a sum that exceeds full scale saturates rather than wrapping.
      std::fill(playback_buffer_.begin(), playback_buffer_.end(), 0);
      for (unsigned i = 0; i < info.period_size; ++i)
      {
        for (unsigned c = 0; c < info.playback_channels; ++c)
        {
          pcm_encode(playback_buffer_.data() +
                       (static_cast<std::size_t>(i) * info.playback_channels + c) * width,
                     info.format, bus_[static_cast<std::size_t>(c) * info.period_size + i]);
        }
      }
    }

    // Outside the RT scope: the guard's counters are read for a report, not needed to render a block,
    // and the scope is kept to the work that has a deadline. Summed across the audible chains, because
    // "a node is failed" is a fact about the rig rather than about one chain.
    {
      unsigned failed = 0;
      uint64_t trips = 0;
      for (int c = 0; c < chain_count_; ++c)
      {
        if (chains_[c].current == nullptr)
          continue;
        failed += chains_[c].current->plan->failed_node_count();
        trips += chains_[c].current->plan->guard_trips();
        chains_[c].failed_nodes.store(static_cast<unsigned>(chains_[c].current->plan->failed_node_count()));
        chains_[c].guard_trips.store(chains_[c].current->plan->guard_trips());
      }
      failed_nodes_.store(failed);
      guard_trips_.store(trips);
    }

    if (device_->try_write_playback(playback_buffer_.data(), info.period_size, error) < 0)
    {
      if (!running_.load())
        break;
      fail_device(error);
      continue;
    }
    account_render_time(std::chrono::steady_clock::now() - block_start, info);
    account_period();

    reclaimer_.advance_epoch();
    blocks_.fetch_add(1);
    const uint64_t xruns = device_->driver_xruns();
    xruns_.store(xruns);

    // The recent-trouble buckets: what arrived since the last block, drained periodically so the
    // number means "lately" rather than "ever".
    if (xruns > last_xruns_)
      recent_xruns_.fetch_add(xruns - last_xruns_);
    last_xruns_ = xruns;
    if (blocks_.load() % kRecentDecayBlocks == 0)
    {
      recent_xruns_.store(decay_recent_trouble(recent_xruns_.load()));
      recent_render_errors_.store(decay_recent_trouble(recent_render_errors_.load()));
    }
  }

  // Give the backend its chance to write what it accumulated (a `file` backend's output WAV) before
  // the object is destroyed. A device backend has nothing to finish, and says so by doing nothing.
  finish_device(error);
  close_device();
  render_pool_.stop();
  render_threads_active_.store(1);
}

bool Daemon::start_render_pool(std::string& error)
{
  error.clear();
  render_threads_active_.store(1);
  render_cores_mask_.store(0);

  if (config_.render_threads <= 1)
    return true; // one participant: `run` calls the work inline, and nothing is started

  std::string pool_error;
  if (!render_pool_.start(config_.render_threads, config_.cpu, config_.rt_priority, pool_error))
  {
    error = pool_error;
    return false;
  }

  uint64_t mask = 0;
  for (const RenderParticipant& participant : render_pool_.participants())
  {
    if (config_.verbose)
      std::fprintf(stderr, "[namchaind] render participant %u: %s\n", participant.index,
                   rt::describe_scheduling(participant.scheduling).c_str());
    if (participant.scheduling.cpu >= 0 && participant.scheduling.cpu < 64)
      mask |= (uint64_t{1} << participant.scheduling.cpu);
  }
  render_threads_active_.store(render_pool_.threads());
  render_cores_mask_.store(mask);
  if (!render_pool_.degraded_reason().empty())
  {
    error = render_pool_.degraded_reason();
    render_pool_degraded_.store(true);
    std::fprintf(stderr, "[namchaind] %s\n", error.c_str());
    return false;
  }
  render_pool_degraded_.store(false);
  return true;
}

void Daemon::run_chains(unsigned participant, unsigned participants, int frames) noexcept
{
  // Every `participants`-th chain. Nothing here may touch another participant's chain, and nothing
  // here may touch the transition bookkeeping: `current` and `next` are read, never written, and the
  // coordinator does not move them until every participant has finished this block.
  for (int c = static_cast<int>(participant); c < chain_count_; c += static_cast<int>(participants))
  {
    ChainRuntime& chain = chains_[c];
    if (chain.current == nullptr)
      continue;

    if (chain.next != nullptr)
    {
      // Dual-render: both plans run on the same input, in the order they always have (the outgoing
      // one first), and their chain outputs are equal-power crossfaded later on the coordinator.
      chain.current->plan->execute(chain.in.data(), chain.old.data(), frames);
      chain.next->plan->execute(chain.in.data(), chain.incoming.data(), frames);
    }
    else
    {
      chain.current->plan->execute(chain.in.data(), chain.out.data(), frames);
    }
  }
}

void Daemon::run_chains_entry(void* context, unsigned participant, unsigned participants, int frames) noexcept
{
  static_cast<Daemon*>(context)->run_chains(participant, participants, frames);
}

// ---------------------------------------------------------------------------
// Control loop and protocol
// ---------------------------------------------------------------------------

std::string Daemon::handle_request(const std::string& request)
{
  Session session;
  return handle_request(request, session);
}

std::string Daemon::handle_request(const std::string& request, Session& session)
{
  // The health summary: one field for a light to bind to, and the reasons behind it as stable codes a
  // client maps to its own words rather than parsing (docs/ui-integration-plan.md §1.6). Issues are
  // *current state*: one that stops being true disappears, which is why the cumulative counters are
  // not what this reads.
  auto issues_json = [this]() {
    nlohmann::json issues = nlohmann::json::array();
    const auto add = [&issues](const char* code, const char* severity, std::string message, uint64_t count,
                               const std::string& chain = std::string(), const std::string& node = std::string()) {
      nlohmann::json issue;
      issue["code"] = code;
      issue["severity"] = severity;
      issue["message"] = std::move(message);
      issue["count"] = count;
      if (!chain.empty())
        issue["chain"] = chain;
      if (!node.empty())
        issue["node"] = node;
      issues.push_back(std::move(issue));
    };

    if (device_state() == DeviceState::Lost)
    {
      add("device_lost", "error",
          "the audio device is not open; the render thread is retrying every " +
            std::to_string(config_.device_retry_ms) + " ms",
          device_lost_.load());
    }
    for (std::size_t i = 0; i < current_document_.chains.size() && i < static_cast<std::size_t>(kMaxChains); ++i)
    {
      if (chains_[i].failed_nodes.load() == 0)
        continue;
      std::string node;
      if (published_slots_[i] != nullptr && published_slots_[i]->plan != nullptr)
      {
        const std::vector<std::string>& ids = published_slots_[i]->plan->node_ids();
        for (std::size_t n = 0; n < ids.size() && n < chains_[i].failed_nodes.load(); ++n)
        {
          // The first failed node is the useful one to name; the counters carry the rest.
          if (published_slots_[i]->plan->node_failed(n))
          {
            node = ids[n];
            break;
          }
        }
      }
      add("node_failed", "error",
          "a node returned a non-finite sample and has been taken out of the signal path",
          chains_[i].guard_trips.load(), current_document_.chains[i].id, node);
    }
    if (recent_xruns_.load() > 0)
      add("xruns", "warn", "the device has missed deadlines recently", recent_xruns_.load());
    if (recent_render_errors_.load() > 0)
      add("render_errors", "warn", "the render thread has reported errors recently", recent_render_errors_.load());
    if (!rt_realtime_.load() && config_.rt_priority > 0)
    {
      add("not_realtime", "warn",
          "the render thread is not running under the real-time policy that was asked for", 1);
    }
    if (config_.lock_memory && !memory_locked_.load() && device_state() != DeviceState::Closed)
      add("memory_not_locked", "warn", "mlockall was refused, so the run is not pinned in memory", 1);
    if (transition_active_.load())
      add("transition", "info", "a structural change is being crossfaded", 1);

    // A block whose model or IR is not in the library plays as if bypassed. A warning, one per block,
    // naming the block's element so a client can mark it: the rig plays, just not as written.
    for (const assets::MissingAsset& miss : missing_assets_)
    {
      add("asset_missing", "warn", "node '" + miss.node + "' plays as if bypassed: " + miss.message, 1, miss.chain,
          miss.node);
      nlohmann::json& issue = issues.back();
      issue["element"] = miss.element;
      issue["param"] = miss.param;
      issue["kind"] = assets::kind_name(miss.kind);
      issue["reference"] = miss.reference;
    }

    // The model budget (docs/daemon.md, "Model budget"). A slimmed block is the budget doing what it
    // was configured to do -- a two-amp rig on a small core is not a fault -- so it is informational,
    // but it names the block, because it sounds different from the model the rig names. A document
    // admitted over the budget, and a model the budget cannot see into, are warnings: either may miss
    // deadlines.
    for (const BudgetedModel& model : model_budget_.models)
    {
      if (!model.slimmed && model.estimated)
        continue;
      if (model.slimmed)
      {
        add("model_slimmed", "info",
            "node '" + model.node + "' plays its smallest model to fit the model budget: " +
              format_kib(model.slim_bytes) + " per block instead of " + format_kib(model.full_bytes),
            1, model.chain, model.node);
      }
      else
      {
        add("model_budget_unknown", "warn",
            "node '" + model.node + "' is not counted against the model budget: " + model.why, 1, model.chain,
            model.node);
      }
      issues.back()["element"] = model.element;
      issues.back()["reference"] = model.reference;
    }
    if (model_budget_.over)
    {
      add("model_budget_exceeded", "warn",
          "the document's models read " + format_kib(model_budget_.used_bytes) +
            " per block together, over the model budget of " + format_kib(model_budget_.budget_bytes) +
            ", so the render may miss deadlines: " + describe_models(model_budget_, current_document_.chains.size() > 1),
          1);
    }

    // A library root that is configured and not there is worth saying out loud: the palette is empty,
    // and "no amps installed" and "the amps directory moved" look identical from a client otherwise.
    // Informational, not a warning: a device with no IRs is a device with no IRs.
    for (const assets::Kind kind : {assets::Kind::Model, assets::Kind::Ir})
    {
      const assets::Root& root = assets_.root(kind);
      if (root.readable || root.path.empty())
        continue;
      add(kind == assets::Kind::Model ? "models_root_missing" : "irs_root_missing", "info",
          "the " + std::string(assets::kind_name(kind)) +
            " library is empty because its directory is not there: " + root.error + " (" + root.path + ")",
          0);
    }
    return issues;
  };

  // The recording, as `engine.record` and `engine.status` both describe it.
  auto recording_json = [this]() {
    nlohmann::json out;
    out["active"] = recorder_live_.load() != nullptr;
    out["enabled"] = !config_.record_dir.empty();
    if (recorder_ == nullptr)
      return out;
    const RecorderFormat& format = recorder_->format();
    out["name"] = recording_name_;
    out["path"] = recorder_->directory();
    out["periods"] = recorder_->periods();
    out["seconds"] = static_cast<double>(recorder_->periods()) * format.period_size / format.sample_rate;
    out["overflows"] = recorder_->overflows();
    out["dropped"] = recorder_->dropped();
    out["inserted"] = recorder_->inserted();
    out["xruns"] = recorder_->xruns();
    const std::string failure = recorder_->write_error();
    if (!failure.empty())
      out["error"] = failure;
    return out;
  };

  // The blocks standing in as a bypass for an asset the library lacks, in every reply that says which
  // generation is running: the client that sent the rig learns it at once, not at the next poll.
  auto missing_json = [this]() {
    nlohmann::json list = nlohmann::json::array();
    for (const assets::MissingAsset& miss : missing_assets_)
      list.push_back({{"chain", miss.chain}, {"node", miss.node}, {"element", miss.element}, {"param", miss.param},
                      {"kind", assets::kind_name(miss.kind)}, {"reference", miss.reference},
                      {"message", miss.message}});
    return list;
  };

  // The blocks the model budget slimmed, beside the missing assets and for the same reason: the client
  // that sent the rig learns at once that a block plays a smaller model than it names.
  auto slimmed_json = [this]() {
    nlohmann::json list = nlohmann::json::array();
    for (const BudgetedModel& model : model_budget_.models)
    {
      if (!model.slimmed)
        continue;
      list.push_back({{"chain", model.chain}, {"node", model.node}, {"element", model.element},
                      {"reference", model.reference}, {"full_bytes", model.full_bytes},
                      {"slim_bytes", model.slim_bytes}});
    }
    return list;
  };

  // One shape for the per-chain array wherever it appears, so a client can read the same entry from
  // `engine.status` and from `rig.get` and not learn two vocabularies for one thing.
  auto chains_json = [this]() {
    nlohmann::json list = nlohmann::json::array();
    for (std::size_t i = 0; i < current_document_.chains.size() && i < static_cast<std::size_t>(kMaxChains); ++i)
    {
      const graph::Chain& chain = current_document_.chains[i];
      const ChainRuntime& runtime = chains_[i];
      nlohmann::json entry;
      entry["id"] = chain.id;
      entry["name"] = chain.name;
      entry["input_channel"] = chain.input_channel;
      entry["output_channel"] = chain.output_channel;
      entry["generation"] = published_slots_[i] == nullptr ? 0 : published_slots_[i]->generation;
      entry["audible_generation"] = runtime.audible_generation.load();
      entry["adopted_generation"] = runtime.adopted_generation.load();
      entry["target_generation"] = runtime.target_generation.load();
      entry["transition_active"] = runtime.transition_active.load();
      entry["muted"] = runtime.mute.load();
      entry["failed_nodes"] = runtime.failed_nodes.load();
      entry["guard_trips"] = runtime.guard_trips.load();
      // The blocks of this chain standing in as a bypass for a model or IR the library lacks.
      nlohmann::json missing = nlohmann::json::array();
      for (const assets::MissingAsset& miss : missing_assets_)
      {
        if (miss.chain != chain.id)
          continue;
        missing.push_back({{"node", miss.node}, {"element", miss.element}, {"param", miss.param},
                           {"kind", assets::kind_name(miss.kind)}, {"reference", miss.reference}});
      }
      entry["missing_assets"] = std::move(missing);
      // And the blocks the model budget slimmed.
      nlohmann::json slimmed = nlohmann::json::array();
      for (const BudgetedModel& model : model_budget_.models)
      {
        if (model.slimmed && model.chain == chain.id)
          slimmed.push_back({{"node", model.node}, {"element", model.element}, {"reference", model.reference},
                             {"full_bytes", model.full_bytes}, {"slim_bytes", model.slim_bytes}});
      }
      entry["slimmed_models"] = std::move(slimmed);
      list.push_back(std::move(entry));
    }
    return list;
  };

  // What the newest publish did with each chain it changed: kept what it could and faded locally, kept
  // everything, only ramped, or crossfaded the whole chain -- and why, when it had to be the last.
  auto transitions_json = [this]() {
    nlohmann::json list = nlohmann::json::array();
    for (const ChainTransition& transition : last_transitions_)
    {
      nlohmann::json entry{{"chain", transition.chain}, {"kind", transition.kind}};
      if (transition.kind == "local" || transition.kind == "instant")
      {
        entry["fades"] = transition.fades;
        entry["kept"] = transition.kept;
        entry["created"] = transition.created;
        entry["retiring"] = transition.retiring;
      }
      if (transition.ramps > 0)
        entry["ramps"] = transition.ramps;
      if (transition.bypasses > 0)
        entry["bypasses"] = transition.bypasses;
      if (!transition.reason.empty())
        entry["reason"] = transition.reason;
      list.push_back(std::move(entry));
    }
    return list;
  };

  nlohmann::json doc;
  try
  {
    doc = nlohmann::json::parse(request);
  }
  catch (const std::exception& e)
  {
    return error_response("", "bad_json", std::string("not valid JSON: ") + e.what());
  }
  if (!doc.is_object())
    return error_response("", "bad_request", "the request must be a JSON object");

  std::string id;
  if (doc.contains("id"))
  {
    if (!doc["id"].is_string())
      return error_response("", "bad_request", "'id' must be a string");
    id = doc["id"].get<std::string>();
  }
  if (!doc.contains("method") || !doc["method"].is_string())
    return error_response(id, "bad_request", "the request needs a string 'method'");
  const std::string method = doc["method"].get<std::string>();

  // --- authorisation, before anything else ---------------------------------
  //
  // A remote connection that has not authenticated may say `engine.hello` and nothing else. Checking
  // it here rather than in each method is the difference between a rule and a habit: a method added
  // later is covered by construction, and the transport closes the connection after
  // \ref kMaxAuthFailures of these regardless of which method was tried.
  if (session.peer == PeerKind::Remote && !session.authorized && method != "engine.hello")
  {
    ++session.auth_failures;
    return error_response(id, "unauthorized",
                          "'" + method + "' needs an authenticated session: send `engine.hello` with the "
                                          "transport token first");
  }

  // --- engine.hello: authenticate, then negotiate the protocol version -----
  if (method == "engine.hello")
  {
    if (session.peer == PeerKind::Remote && !session.authorized)
    {
      // The token is checked first, and the answer says only that it was wrong: a peer that does not
      // hold the token learns nothing about which protocol versions this daemon speaks, whether the
      // token was missing or merely incorrect, or how close it was.
      std::string presented;
      if (doc.contains("token"))
      {
        if (!doc["token"].is_string())
          return error_response(id, "bad_request", "'token' must be a string");
        presented = doc["token"].get<std::string>();
      }
      if (!constant_time_equal(presented, config_.tcp_token))
      {
        ++session.auth_failures;
        return error_response(id, "unauthorized", "the transport token is missing or wrong");
      }
      session.authorized = true;
      session.auth_failures = 0;
    }

    int protocol = kProtocolVersion;
    if (doc.contains("protocol"))
    {
      if (!doc["protocol"].is_number_integer())
        return error_response(id, "bad_request", "'protocol' must be an integer");
      protocol = doc["protocol"].get<int>();
    }
    if (protocol < kOldestProtocolVersion || protocol > kProtocolVersion)
    {
      return error_response(id, "unsupported_protocol",
                            "namchaind speaks protocol " + std::to_string(kOldestProtocolVersion) + " to " +
                              std::to_string(kProtocolVersion) + ", the client asked for " +
                              std::to_string(protocol));
    }
    nlohmann::json body;
    body["protocol"] = kProtocolVersion;
    body["role"] = "namchaind";
    body["version"] = NAMCHAIN_VERSION_STRING;
    body["features"] = protocol_features();
    // Which transport answered, and whether this session is authenticated. On the local transport the
    // second is always true (filesystem permissions decided it); on TCP it becomes true here and
    // nowhere else, which is what makes a client able to tell "wrong token" from "wrong method".
    body["transport"] = session.peer == PeerKind::Remote ? "tcp" : "unix";
    body["authenticated"] = session.authorized;
    return ok_response(id, body);
  }

  // --- engine.status ------------------------------------------------------
  if (method == "engine.schema")
  {
    // The catalogue. A palette, an inspector and a validator all come from this one answer, so a
    // node type cannot be invisible to a client or described differently by two of them.
    nlohmann::json nodes = nlohmann::json::array();
    for (const render::NodeType* type : render::all_node_types())
      nodes.push_back(node_type_schema(*type, assets_));
    // Macros in the same array, told apart by `kind`: a palette wants one list in one order, and a
    // client that does not know what a macro is can still show it.
    for (const graph::MacroType* macro : graph::all_macro_types())
      nodes.push_back(macro_type_schema(*macro, assets_));
    nlohmann::json body;
    body["protocol"] = kProtocolVersion;
    body["nodes"] = std::move(nodes);
    body["max_chains"] = kMaxChains;
    body["stereo"] = false; // declared and refused: see NodeType::channels
    return ok_response(id, body);
  }
  // --- engine.assets: what is in the device's library ----------------------
  if (method == "engine.assets")
  {
    // Answered from the scan rather than from the filesystem, so a client can diff two answers and
    // know that a difference is a difference on disk. `refresh` before the answer is the one way to
    // ask for the disk again, and it is cheap when nothing changed: an unchanged file costs a stat,
    // not a read (docs/assets.md).
    if (doc.contains("refresh"))
    {
      if (!doc["refresh"].is_boolean())
        return error_response(id, "bad_request", "'refresh' must be a boolean");
      if (doc["refresh"].get<bool>())
      {
        assets_.refresh();
        republish_after_rescan();
      }
    }

    nlohmann::json models = nlohmann::json::array();
    for (const assets::Asset* asset : assets_.of_kind(assets::Kind::Model))
      models.push_back(asset_json(*asset));
    nlohmann::json irs = nlohmann::json::array();
    for (const assets::Asset* asset : assets_.of_kind(assets::Kind::Ir))
      irs.push_back(asset_json(*asset));

    nlohmann::json body;
    body["models_root"] = assets_.models_root();
    body["irs_root"] = assets_.irs_root();
    body["roots"] = {{"models", root_json(assets_.root(assets::Kind::Model))},
                     {"irs", root_json(assets_.root(assets::Kind::Ir))}};
    body["scan"] = assets_.scans();
    body["files_parsed"] = assets_.last_scan().parsed;
    body["models"] = std::move(models);
    body["irs"] = std::move(irs);
    return ok_response(id, body);
  }
  if (method == "engine.status")
  {
    nlohmann::json status;
    status["role"] = "namchaind";
    status["version"] = NAMCHAIN_VERSION_STRING;
    status["protocol"] = kProtocolVersion;
    status["state"] = device_state_name(device_state());
    status["backend"] = backend_kind_name(config_.backend);
    status["device"] = config_.device;
    status["sample_rate"] = active_rate_.load() == 0 ? config_.sample_rate : active_rate_.load();
    status["period_size"] = active_period_.load() == 0 ? config_.period_size : active_period_.load();
    status["capture_channels"] = active_capture_channels_.load();
    status["playback_channels"] = active_playback_channels_.load();
    status["rig"] = current_rig_;
    status["rig_source"] = current_source_;
    status["rig_name"] = current_name_;
    status["generation"] = generation_;
    status["adopted_generation"] = adopted_generation_.load();
    status["blocks"] = blocks_.load();
    status["xruns"] = xruns_.load();
    status["device_lost"] = device_lost_.load();
    status["device_restored"] = device_restored_.load();
    status["render_errors"] = render_errors_.load();
    status["mute"] = mute_.load();
    status["memory_locked"] = memory_locked_.load();
    status["realtime"] = rt_realtime_.load();
    status["rt_policy"] = rt_policy_.load();
    status["rt_priority"] = rt_priority_.load();
    // How many cores the render path is actually using, which core each participant was confined to
    // (a bit per CPU, read back from the kernel rather than remembered), and whether the pool fell
    // short of what was configured. A run whose `render_threads` is below `render_threads_configured`
    // is not a multi-core measurement.
    status["render_threads"] = render_threads_active_.load();
    status["render_threads_configured"] = config_.render_threads;
    status["render_cores"] = render_cores_mask_.load();
    status["render_degraded"] = render_pool_degraded_.load();
    status["retired_pending"] = static_cast<std::uint64_t>(reclaimer_.pending());
    status["params_applied"] = params_applied_.load();
    status["params_stale"] = params_stale_.load();
    status["failed_nodes"] = failed_nodes_.load();
    status["guard_trips"] = guard_trips_.load();
    // Two honest labels: parameter edits are ramped inside the node (no recompile, no step); a
    // structural change is equal-power crossfaded for fade_ms -- where the edit is, when what it did not
    // touch can be kept (`local_transitions`), and across the whole chain when it cannot.
    status["param_edits"] = "ramped";
    status["structural_transitions"] = "crossfade";
    status["transition_active"] = transition_active_.load();
    status["audible_generation"] = audible_generation_.load();
    status["target_generation"] = target_generation_.load();
    status["transitions_started"] = transitions_started_.load();
    // Of those, the ones that kept what the edit did not touch and faded only what it changed
    // (docs/transitions.md, "Local transitions"); and what the newest publish did, chain by chain.
    status["local_transitions"] = local_transitions_.load();
    status["local_base_mismatches"] = local_base_mismatches_.load();
    status["last_transitions"] = transitions_json();
    status["max_concurrent_transitions"] = max_concurrent_transitions_.load();
    status["fade_ms"] = config_.fade_ms;
    // The output queue's own losses. A dropped or an inserted period is a click, and neither is an xrun.
    status["queue"] = {{"output_queue_periods", config_.output_queue_periods},
                       {"output_queue_floor_periods", config_.output_queue_floor_periods},
                       {"output_drops", output_drops_.load()},
                       {"output_target_drops", output_target_drops_.load()},
                       {"floor_insertions", floor_insertions_.load()},
                       {"capture_bursts", capture_bursts_.load()}};
    status["record"] = recording_json();
    // Each block's time from its capture read to its playback write, against the period's deadline.
    // `load_histogram` is [percent, blocks] for the buckets that have any; 100 is "at or past it".
    {
      nlohmann::json histogram = nlohmann::json::array();
      uint64_t over_90 = 0;
      for (std::size_t b = 0; b < kLoadBuckets; ++b)
      {
        const uint64_t count = load_histogram_[b].load(std::memory_order_relaxed);
        if (count == 0)
          continue;
        histogram.push_back({b, count});
        if (b >= 90)
          over_90 += count;
      }
      const uint64_t deadline = deadline_ns_.load(std::memory_order_relaxed);
      const uint64_t worst = worst_block_ns_.load(std::memory_order_relaxed);
      status["render"] = {{"deadline_us", static_cast<double>(deadline) / 1000.0},
                          {"worst_us", static_cast<double>(worst) / 1000.0},
                          {"worst_percent", deadline == 0 ? 0.0 : 100.0 * static_cast<double>(worst) / static_cast<double>(deadline)},
                          {"over_90_percent", over_90},
                          {"over_deadline", load_histogram_[kLoadBuckets - 1].load(std::memory_order_relaxed)},
                          {"load_histogram", std::move(histogram)}};
    }

    // One entry per chain, in the document's order. This is where a client learns what its document
    // became: the chain ids it owns, the channels it was bound to, and how far each chain has got
    // through its own transition. The document-level fields above are derived from these, so a client
    // can read either the whole picture or one chain's.
    status["chains"] = chains_json();

    // The model budget as configured, and what the running document reads against it: 0 throughout when
    // it is off. `used_bytes` is as published, slimmed models at their slim size; `full_bytes` is what
    // the same document would read with none of them slimmed.
    status["model_budget"] = {{"budget_bytes", model_budget_.budget_bytes},
                              {"per_chain_bytes", model_budget_.per_chain_bytes},
                              {"configured_per_core_bytes", static_cast<std::size_t>(config_.model_budget_kb) * 1024u},
                              {"heaviest_chain", model_budget_.heaviest_chain},
                              {"heaviest_chain_bytes", model_budget_.heaviest_chain_bytes},
                              {"used_bytes", model_budget_.used_bytes},
                              {"full_bytes", model_budget_.full_bytes},
                              {"over", model_budget_.over}};

    // The library at a glance. The listing is `engine.assets`; this is the size of it, so a client
    // that shows a palette can tell "no models installed" from "not asked yet".
    status["assets"] ={{"models_root", assets_.models_root()},
                        {"irs_root", assets_.irs_root()},
                        {"models", assets_.of_kind(assets::Kind::Model).size()},
                        {"irs", assets_.of_kind(assets::Kind::Ir).size()},
                        {"scan", assets_.scans()}};

    // The control transports as they actually are, not as configured: whether the TCP listener came up
    // and on which port (a configured 0 means the kernel chose), and how many connections each
    // transport is holding. A client that cannot connect gets a better answer from this over the Unix
    // socket than from a failed `connect`.
    nlohmann::json control;
    control["unix_socket"] = config_.socket_path;
    control["unix_connections"] = connection_count(PeerKind::Local);
    control["tcp_listening"] = tcp_fd_ >= 0;
    control["tcp_address"] = config_.tcp_address;
    control["tcp_port"] = tcp_port_;
    control["tcp_token_required"] = config_.tcp_listen;
    control["tcp_connections"] = connection_count(PeerKind::Remote);
    control["tcp_max_connections"] = config_.tcp_max_connections;
    control["tcp_max_message_bytes"] = config_.tcp_max_message_bytes;
    status["control"] = std::move(control);

    // `health` is derived from the issues rather than tracked beside them, so the two cannot disagree:
    // error beats warn beats ok, and informational issues do not colour it.
    nlohmann::json issues = issues_json();
    const char* health = "ok";
    for (const auto& issue : issues)
    {
      if (issue["severity"] == "error")
      {
        health = "error";
        break;
      }
      if (issue["severity"] == "warn")
        health = "warn";
    }
    status["health"] = health;
    status["issues"] = std::move(issues);
    status["max_chains"] = kMaxChains;
    status["features"] = protocol_features();
    nlohmann::json body;
    body["status"] = status;
    return ok_response(id, body);
  }

  // --- engine.record -------------------------------------------------------
  if (method == "engine.record")
  {
    std::string action = "status";
    if (doc.contains("action"))
    {
      if (!doc["action"].is_string())
        return error_response(id, "bad_request", "'action' must be a string");
      action = doc["action"].get<std::string>();
    }
    if (action == "status")
      return ok_response(id, {{"record", recording_json()}});
    if (action == "stop")
    {
      if (recorder_live_.load() == nullptr)
        return error_response(id, "not_recording", "nothing is being recorded");
      end_recording();
      return ok_response(id, {{"record", recording_json()}});
    }
    if (action != "start")
      return error_response(id, "bad_request", "'action' is start, stop or status, not '" + action + "'");

    if (config_.record_dir.empty())
      return error_response(id, "record_disabled", "recording is off: record_dir is empty");
    if (recorder_live_.load() != nullptr)
      return error_response(id, "record_busy", "already recording '" + recording_name_ + "'");
    if (!doc.contains("name") || !doc["name"].is_string() ||
        !valid_recording_name(doc["name"].get<std::string>()))
      return error_response(id, "bad_request",
                            "'name' must be 1-64 of letters, digits, '.', '_' and '-', not starting with '.'");
    const std::string name = doc["name"].get<std::string>();
    double seconds = kDefaultRecordSeconds;
    if (doc.contains("seconds"))
    {
      if (!doc["seconds"].is_number() || !(doc["seconds"].get<double>() > 0.0))
        return error_response(id, "bad_request", "'seconds' must be a number greater than zero");
      seconds = std::min(doc["seconds"].get<double>(), kMaxRecordSeconds);
    }
    if (device_state() != DeviceState::Running)
      return error_response(id, "device_not_running", "the device is not running, so there is nothing to record");

    RecorderFormat format;
    format.sample_rate = active_rate_.load();
    format.period_size = active_period_.load();
    format.capture_channels = active_capture_channels_.load();
    format.playback_channels = active_playback_channels_.load();
    format.format = static_cast<PcmFormat>(active_format_.load());

    std::error_code ignored;
    std::filesystem::create_directories(config_.record_dir, ignored);

    // Enough to read the files without this conversation: what the channels are and what was
    // running when the recording started.
    nlohmann::json meta;
    meta["name"] = name;
    meta["started"] = utc_now_iso8601();
    meta["device"] = config_.device;
    meta["backend"] = backend_kind_name(config_.backend);
    meta["sample_rate"] = format.sample_rate;
    meta["period_size"] = format.period_size;
    meta["format"] = pcm_format_name(format.format);
    meta["capture_channels"] = format.capture_channels;
    meta["playback_channels"] = format.playback_channels;
    meta["output_queue_periods"] = config_.output_queue_periods;
    meta["output_queue_floor_periods"] = config_.output_queue_floor_periods;
    meta["rig_name"] = current_name_;
    meta["generation"] = generation_;
    meta["chains"] = chains_json();
    meta["files"] = {{"capture.wav", "every capture channel, as the device delivered it"},
                     {"playback.wav", "every playback period the render loop offered, dropped or not"},
                     {"events.tsv", "per period: dropped, inserted (after it), xrun, burst; gap = ring overflow"}};

    std::string error;
    recorder_.reset();
    recorder_ = Recorder::start(config_.record_dir + "/" + name, format, seconds, meta.dump(2), error);
    if (recorder_ == nullptr)
      return error_response(id, "record_failed", error);
    recording_name_ = name;
    recorder_live_.store(recorder_.get());
    if (config_.verbose)
      std::fprintf(stderr, "[namchaind] recording '%s' for up to %.0f s in %s\n", name.c_str(), seconds,
                   recorder_->directory().c_str());
    return ok_response(id, {{"record", recording_json()}});
  }

  // --- engine.mute / engine.panic ----------------------------------------
  if (method == "engine.mute")
  {
    bool muted = true;
    if (doc.contains("mute"))
    {
      if (!doc["mute"].is_boolean())
        return error_response(id, "bad_request", "'mute' must be a boolean");
      muted = doc["mute"].get<bool>();
    }
    nlohmann::json body;
    if (doc.contains("chain"))
    {
      // Per-chain mute: the chain keeps running and keeps its state warm, and only its contribution
      // to the output bus is dropped (G14). Resolved here, so a chain that does not exist is an error
      // rather than a silently ignored mute.
      if (!doc["chain"].is_string())
        return error_response(id, "bad_request", "'chain' must be a string");
      const int index = find_chain(doc["chain"].get<std::string>());
      if (index < 0)
        return error_response(id, "unknown_chain", "no chain '" + doc["chain"].get<std::string>() + "'");
      chains_[static_cast<std::size_t>(index)].mute.store(muted);
      body["mute"] = muted;
      body["chain"] = doc["chain"];
      return ok_response(id, body);
    }
    mute_.store(muted);
    body["mute"] = muted;
    return ok_response(id, body);
  }
  if (method == "engine.panic")
  {
    // The other route to silence, and the only one that is unconditional. There is no transition
    // machinery to abort yet (M2), so panic is mute plus a marker in the response.
    mute_.store(true);
    nlohmann::json body;
    body["mute"] = true;
    body["panic"] = true;
    return ok_response(id, body);
  }

  // --- rig.load / rig.get -------------------------------------------------
  if (method == "rig.load")
  {
    if (!doc.contains("rig") || !doc["rig"].is_string())
      return error_response(id, "bad_request", "rig.load needs a string 'rig'");
    const std::string name = doc["rig"].get<std::string>();
    std::string path;
    std::string error;
    if (!resolve_rig_name(config_, name, path, error))
      return error_response(id, "rig_not_found", error);
    std::string code = "compile_failed";
    if (!load_rig(path, error, &code))
      return error_response(id, code, error);
    nlohmann::json body;
    body["rig"] = path;
    body["generation"] = generation_;
    body["missing_assets"] = missing_json();
    body["slimmed_models"] = slimmed_json();
    body["transitions"] = transitions_json();
    return ok_response(id, body);
  }
  if (method == "rig.apply")
  {
    // A whole document, in memory. This is the path a UI takes: it owns the document, and a
    // structural edit is one publish rather than a sequence of graph ops it would have to get right
    // (docs/ui-integration-plan.md §1.1). The document is parsed by the same parser a rig file goes
    // through, so a document means the same thing however it arrived.
    if (!doc.contains("document") || !doc["document"].is_object())
      return error_response(id, "bad_request", "rig.apply needs an object 'document'");
    graph::Document document;
    std::string error;
    if (!graph::parse_document_json(doc["document"].dump(), document, error))
      return error_response(id, "bad_request", error);

    // Relative model/IR paths resolve against the rigs root, which is the same trust boundary a
    // rig.load name lives behind: a socket client may name a rig but may not point the daemon at an
    // arbitrary directory. An absolute path is the operator's business on the local transport and
    // nobody's business on the network one (docs/daemon.md, threat model).
    if (session.peer == PeerKind::Remote)
    {
      const std::string offender = first_absolute_path(document);
      if (!offender.empty())
      {
        return error_response(id, "path_refused",
                              "a document from a network client may not name the absolute path '" + offender +
                                "'; put the file under the rigs root and name it relatively");
      }
    }

    std::string code = "compile_failed";
    if (!publish_document(document, /*path=*/"", "applied", config_.rigs_root, error, &code))
      return error_response(id, code, error);
    nlohmann::json body;
    body["name"] = document.name;
    body["generation"] = generation_;
    body["missing_assets"] = missing_json();
    body["slimmed_models"] = slimmed_json();
    body["chains"] = document.chains.size();
    body["transitions"] = transitions_json();
    return ok_response(id, body);
  }
  if (method == "rig.get")
  {
    nlohmann::json body;
    body["rig"] = current_rig_;
    body["source"] = current_source_;
    body["generation"] = generation_;
    body["missing_assets"] = missing_json();
    body["slimmed_models"] = slimmed_json();
    body["loaded"] = current_document_.chains.empty() ? false : true;
    if (!current_document_.chains.empty())
    {
      // The engine's own parse of what it is running, so a client can see the ids it minted and the
      // element/role of every node. A diagnostic, not a document store (§1.1).
      body["document"] = nlohmann::json::parse(graph::document_to_json(current_document_));
      // And the lowered view: the primitives each macro became, with the engine-minted ids, which is
      // the only place to see what a document actually runs (§1.5). Two views of one thing: the
      // document as the client wrote it, and the graph the render thread executes.
      nlohmann::json lowered = nlohmann::json::array();
      for (const graph::Chain& chain : current_document_.chains)
      {
        graph::Graph primitives;
        std::string reason;
        // A chain that lowered when it was published lowers again now: the function is deterministic,
        // and this is the control thread with nothing on a deadline.
        if (!graph::lower_graph(chain.graph, primitives, reason))
          continue;
        const nlohmann::json graph = nlohmann::json::parse(graph::graph_to_json(primitives));
        lowered.push_back({{"chain", chain.id}, {"nodes", graph["nodes"]}, {"connections", graph["connections"]}});
      }
      body["lowered"] = std::move(lowered);
      body["chains"] = chains_json();
    }
    return ok_response(id, body);
  }
  if (method == "rig.patch")
  {
    if (!doc.contains("ops") || !doc["ops"].is_array())
      return error_response(id, "bad_request", "rig.patch needs an 'ops' array");
    if (current_document_.chains.empty() || published_slots_.empty())
      return error_response(id, "patch_rejected", "no plan is loaded; rig.load first");

    // Which chain this patch is against. With one chain it is obvious; with several it must be said,
    // because guessing would edit an arbitrary one of them (docs/ui-integration-plan.md §1.3).
    std::size_t chain_index = 0;
    if (doc.contains("chain"))
    {
      if (!doc["chain"].is_string())
        return error_response(id, "bad_request", "'chain' must be a string");
      const int found = find_chain(doc["chain"].get<std::string>());
      if (found < 0)
        return error_response(id, "unknown_chain", "no chain '" + doc["chain"].get<std::string>() + "'");
      chain_index = static_cast<std::size_t>(found);
    }
    else if (current_document_.chains.size() > 1)
    {
      return error_response(id, "unknown_chain",
                            "this rig has " + std::to_string(current_document_.chains.size()) +
                              " chains; rig.patch needs a 'chain'");
    }
    const std::string chain_id = current_document_.chains[chain_index].id;
    const PlanSlot* target = published_slots_[chain_index];
    if (target == nullptr || target->plan == nullptr)
      return error_response(id, "patch_rejected", "no plan is loaded; rig.load first");

    // The same rule as rig.apply, checked on the ops rather than on the patched document: a network
    // client may edit a document the operator loaded from a rig file that uses absolute paths, it may
    // just not introduce any of its own (docs/daemon.md, threat model).
    if (session.peer == PeerKind::Remote)
    {
      const std::string offender = first_absolute_path_in_ops(doc["ops"]);
      if (!offender.empty())
      {
        return error_response(id, "path_refused",
                              "a patch from a network client may not name the absolute path '" + offender +
                                "'; put the file under the rigs root and name it relatively");
      }
    }

    // Route by op class (§5.3). If every op is a `set_param` the live plan can dispatch, the whole
    // patch is the smooth class: a vtable dispatch on the render thread, no compile, no fade. Any
    // other op means the graph changes, so the whole patch is structural and all-or-nothing: it is
    // applied to a copy and published, which the render thread crossfades into.
    //
    // Every op is resolved before any of them is pushed. A patch is one edit from the client's point
    // of view -- a knob drag is a stream of single-op patches, but a batch is a batch -- and leaving
    // half of one applied because the fourth op turned out to be structural is the kind of state that
    // is nobody's intent and everybody's bug report.
    EditContext context;
    context.slot = target;
    context.chain = &current_document_.chains[chain_index];
    context.chain_index = chain_index;

    bool smooth = true;
    std::vector<ResolvedEdit> edits;
    for (std::size_t i = 0; smooth && i < doc["ops"].size(); ++i)
    {
      ResolvedEdit edit;
      bool structural = false;
      std::string reason;
      if (resolve_smooth_edit(context, doc["ops"][i], edit, structural, reason))
      {
        edits.push_back(std::move(edit));
        continue;
      }
      if (!structural)
        return error_response(id, "patch_rejected", "ops[" + std::to_string(i) + "]: " + reason);
      smooth = false;
    }

    if (smooth)
    {
      for (const ResolvedEdit& edit : edits)
      {
        for (const PlanCommand& command : edit.commands)
        {
          if (!commands_.push(command))
            return error_response(id, "patch_rejected",
                                  "the command ring is full; the render thread is not consuming edits");
        }
      }
      // The document follows what is playing. Without this, `rig.get` would report the values the
      // document arrived with rather than the ones the user has been turning, and the next structural
      // edit -- which is built from this document -- would silently revert every ramped parameter
      // (docs/macros.md §"What a macro edit records").
      //
      // And the resolved copy follows too: it is what the next publish is compared against, node by
      // node, to find what changed (docs/transitions.md, "Local transitions"). A ramp is never a model
      // or IR reference, so the two copies agree about every value a ramp can move.
      for (const ResolvedEdit& edit : edits)
      {
        if (edit.document_node.empty())
          continue;
        for (graph::Document* copy : {&current_document_, &current_resolved_})
        {
          if (chain_index >= copy->chains.size())
            continue;
          for (graph::NodeSpec& spec : copy->chains[chain_index].graph.nodes)
          {
            if (spec.id != edit.document_node)
              continue;
            if (edit.is_text)
              spec.strings[edit.document_param] = edit.text;
            else
              spec.params[edit.document_param] = edit.value;
            break;
          }
        }
      }

      nlohmann::json body;
      body["kind"] = "smooth";
      body["applied"] = edits.size();
      body["chain"] = chain_id;
      body["generation"] = generation_;
      body["missing_assets"] = missing_json();
      body["slimmed_models"] = slimmed_json();
      return ok_response(id, body);
    }

    // The structural class. Only the named chain's graph changes, so only that chain is replanned and
    // crossfaded: publish_document compares each chain and leaves the others' plans alone.
    graph::Document patched = current_document_;
    graph::PatchContext patch_context;
    // A model path in a patch is relative to the rig being patched, the same rule a rig's own model
    // paths follow, so the two agree about where models live.
    patch_context.base_dir = current_base_dir_;
    std::string error;
    if (!graph::apply_patch(patched.chains[chain_index].graph, doc["ops"].dump(), patch_context, error))
      return error_response(id, "patch_rejected", error);
    std::string code = "compile_failed";
    if (!publish_document(patched, current_rig_, current_source_, current_base_dir_, error, &code))
      return error_response(id, code, error);

    nlohmann::json body;
    body["kind"] = "structural";
    body["applied"] = static_cast<unsigned>(doc["ops"].size());
    body["chain"] = chain_id;
    body["generation"] = generation_;
    body["missing_assets"] = missing_json();
    body["slimmed_models"] = slimmed_json();
    body["transitions"] = transitions_json();
    return ok_response(id, body);
  }

  return error_response(id, "unknown_method", "unknown method '" + method + "'");
}

int Daemon::run()
{
  if (!started_)
  {
    std::fprintf(stderr, "[namchaind] run() called before start()\n");
    return 1;
  }

  while (!stop_requested_.load())
  {
    // The poll set: both listeners, the signal fd, then one entry per client. The client entries carry
    // POLLOUT only while that connection still owes the peer bytes, so a drained connection costs one
    // readable fd and nothing else.
    std::vector<pollfd> fds;
    fds.push_back(pollfd{listen_fd_, POLLIN, 0});
    fds.push_back(pollfd{signal_fd_, POLLIN, 0});
    if (tcp_fd_ >= 0)
      fds.push_back(pollfd{tcp_fd_, POLLIN, 0});
    const std::size_t tcp_listener = tcp_fd_ >= 0 ? 2 : 0;
    const std::size_t first_client = fds.size();
    for (const Connection& connection : clients_)
    {
      const short events = static_cast<short>(POLLIN | (connection.out_sent < connection.out.size() ? POLLOUT : 0));
      fds.push_back(pollfd{connection.fd, events, 0});
    }

    const int timeout = std::max(1, config_.reclamation_timeout_ms);
    const int ready = ::poll(fds.data(), fds.size(), timeout);
    if (ready < 0)
    {
      if (errno == EINTR)
        continue;
      std::fprintf(stderr, "[namchaind] poll: %s\n", std::strerror(errno));
      stop();
      return 1;
    }

    if ((fds[0].revents & POLLIN) != 0 && listen_fd_ >= 0)
      accept_connections(listen_fd_, PeerKind::Local);
    if ((fds[1].revents & POLLIN) != 0)
    {
      int signo = 0;
      while (platform::read_signal(signal_fd_, signo))
      {
        if (signo == SIGTERM || signo == SIGINT)
          stop_requested_.store(true);
        else if (signo == SIGHUP)
        {
          // A reload is also the moment an operator expects a newly copied model to appear in the
          // palette, so the library is rescanned with the rig.
          assets_.refresh();
          std::string path;
          std::string error;
          // The configured rig again, under the startup's rule: admitted over the model budget.
          if (!config_.rig.empty() && resolve_startup_rig(config_, config_.rig, path, error) &&
              load_rig(path, error, nullptr, /*admit_over_budget=*/true))
          {
            if (config_.verbose)
              std::fprintf(stderr, "[namchaind] SIGHUP: reloaded %s\n", path.c_str());
          }
          else
          {
            std::fprintf(stderr, "[namchaind] SIGHUP reload failed: %s\n", error.c_str());
          }
        }
      }
    }
    if (stop_requested_.load())
      break;
    if (tcp_listener != 0 && (fds[tcp_listener].revents & POLLIN) != 0)
      accept_connections(tcp_fd_, PeerKind::Remote);

    // A client can vanish at any point in the walk below; collect the dead indices and erase them
    // after it, so the index arithmetic stays honest. `clients_` does not change during the walk --
    // only this vector of indices does.
    //
    // The walk is bounded by the poll set, not by `clients_`: a connection accepted above was not in
    // `fds` when it was built, so it has no revents to read and is picked up on the next pass.
    // Bounding by `clients_.size()` would index past the end of `fds` -- into whatever the allocator
    // freed when `fds` last grew.
    const std::size_t polled_clients = fds.size() - first_client;
    std::vector<std::size_t> dead;
    for (std::size_t i = 0; i < std::min(clients_.size(), polled_clients); ++i)
    {
      Connection& connection = clients_[i];
      const short revents = fds[first_client + i].revents;
      // POLLOUT belongs in this guard, not only in the branch below. A connection whose *only*
      // ready event is "writable" is one that owes the peer bytes and has nothing more to say --
      // which is exactly a reply too big for the socket's send buffer, waiting for room. Skipping
      // it here left that reply in the outbox for ever and the caller blocked in recv.
      //
      // It took a stream socket with an 8 KiB buffer to show: a Unix SOCK_SEQPACKET reply is one
      // datagram and fits, and a TCP connection is given its own SO_SNDBUF and answers small
      // requests. A large answer -- engine.schema, engine.assets -- over a stream whose buffer is
      // smaller than the answer is the case none of those cover.
      if ((revents & (POLLIN | POLLOUT | POLLHUP | POLLERR)) == 0)
        continue;

      bool alive = true;
      if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0)
        alive = read_connection(connection);
      if (alive && (revents & POLLOUT) != 0)
        alive = write_connection(connection);
      if (!alive)
        dead.push_back(i);
    }
    for (std::size_t i = dead.size(); i-- > 0;)
      close_connection(dead[i]);

    drop_expired_connections();

    // A recording that has all its audio is finished here rather than when someone next asks, so its
    // files are complete on disk as soon as they can be.
    if (recorder_live_.load() != nullptr && recorder_->complete())
      end_recording();

    // The reclamation window of rig-pipeline.md §5.3: this is the "somebody" that frees a plan once
    // its grace has closed, and the reason the poll timeout is neither zero nor infinite.
    reclaimer_.collect();
  }

  stop();
  return 0;
}

} // namespace daemon
} // namespace namchain

#endif // __linux__ || __APPLE__
