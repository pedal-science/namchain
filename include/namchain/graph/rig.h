#pragma once

#include <string>

#include "namchain/graph/graph.h"

namespace namchain
{
namespace graph
{

/// \brief The graph for "one model, or several in series".
///
/// This is what a soak with `--model --instances N` means, expressed in the IR rather than as a
/// private loop: once the chain is a graph, the device path, the offline path and the unit tests
/// all exercise the same compiler and the same render plan.
Graph chain_graph(const std::string& model_path, unsigned instances);

/// \brief Parse a rig from JSON text.
///
/// Schema:
/// \code
/// {
///   "nodes": [
///     { "id": "in",  "type": "nc.input" },
///     { "id": "amp", "type": "nc.nam", "model": "amp.nam" },
///     { "id": "out", "type": "nc.output" }
///   ],
///   "connections": [ { "from": "in", "to": "amp" }, { "from": "amp", "to": "out" } ]
/// }
/// \endcode
///
/// `id`, `type` and `bypass` are reserved; every other key of a node object is a parameter. A number
/// or a boolean becomes a numeric param, a string becomes a string param -- so a model path does not
/// need a separate block. `bypass` is a boolean property of the node (it keeps running with its dry
/// input substituted), not a parameter, so it does not land in the param map. Explicit
/// `"params"`/`"strings"` objects are also accepted for the cases where the distinction matters.
/// `from_port`/`to_port` default to 0.
bool parse_graph_json(const std::string& text, Graph& graph, std::string& error);

/// \brief Load a rig from a file. \see parse_graph_json
bool load_graph_json(const std::string& path, Graph& graph, std::string& error);

} // namespace graph
} // namespace namchain
