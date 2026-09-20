#pragma once

#include <string>
#include <vector>

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

/// \brief One signal path: a graph, plus what the device and the document know about it.
///
/// A chain is compiled into one plan. Two chains are two plans, rendered independently, which is how
/// a guitar and a vocal path live in one rig (docs/ui-integration-plan.md §1.1).
struct Chain
{
  /// Client-owned, opaque, unique within the document. Never parsed.
  std::string id;
  /// Free text, for a client's own display.
  std::string name;
  /// Which captured device channel this chain reads. Two chains may read the same one.
  int input_channel = 0;
  /// Which playback channel it writes. Two chains writing the same one are summed.
  int output_channel = 0;
  Graph graph;
};

/// \brief A whole document: zero or more chains, and nothing the engine needs beyond them.
///
/// The name is free text; there is no other rig-level metadata in the IR, because everything else a
/// client stores (layout, timestamps, presentation) is the client's business and `rig.get` is a
/// diagnostic rather than a document store.
struct Document
{
  std::string name;
  std::vector<Chain> chains;
};

/// \brief Parse a document from JSON text.
///
/// Schema:
/// \code
/// {
///   "name": "Guitar + Vocal",
///   "chains": [
///     { "id": "gtr", "name": "Guitar", "input_channel": 0, "output_channel": 0,
///       "nodes": [ { "id": "in", "type": "nc.input" }, ... ],
///       "connections": [ { "from": "in", "to": "amp" }, ... ] }
///   ]
/// }
/// \endcode
///
/// A document that is a bare graph -- top-level `nodes`/`connections`, no `chains` -- is one chain
/// with id `"main"` on channels 0 and 0. Everything committed before documents existed is in that
/// shape, so this is the compatibility rule rather than a special case.
///
/// A node's `id`, `type` and `bypass` are reserved, as are `element` and `role`; every other key of a
/// node object is a parameter. A number or a boolean becomes a numeric param, a string becomes a
/// string param -- so a model path does not need a separate block. `bypass` is a boolean property of
/// the node (it keeps running with its dry input substituted), not a parameter, so it does not land
/// in the param map. Explicit `"params"`/`"strings"` objects are also accepted for the cases where
/// the distinction matters. `from_port`/`to_port` default to 0.
///
/// \par Identity
/// `element` is the client's own name for the thing this node belongs to (a block, a lane, a
/// parallel section). It is stored and returned verbatim and never interpreted; see
/// \ref NodeSpec::element. `role` says which node of that element this is.
///
/// A node's `id` may be **omitted** when it has an `element`: the engine then mints one from the
/// element and the role (`"<element>"`, or `"<element>/<role>"`), disambiguating with `~2`, `~3` ...
/// if that name is taken. Because the id is minted rather than composed by the client, nothing needs
/// a reserved character or a forbidden spelling in a client's own names.
///
/// The rule of thumb is that **minting is for a node the client does not have to name**: a node whose
/// connections the client also writes needs an explicit `id`, because a connection names nodes. That
/// covers the two cases that matter -- a macro, which the engine lowers into primitives and wires
/// itself, and a document whose author addresses nodes by `element` and `role`. A node with neither
/// an `id` nor an `element` is an error: there would be nothing to call it.
bool parse_document_json(const std::string& text, Document& document, std::string& error);

/// \brief Load a document from a file. \see parse_document_json
bool load_document_json(const std::string& path, Document& document, std::string& error);

/// \brief Parse a single-chain rig: a document that must hold exactly one chain.
///
/// The long-standing entry point, kept because most callers -- the offline tools, the patch ops, the
/// tests -- have no interest in chains. It accepts both the bare-graph shape and an envelope with one
/// chain, and rejects an envelope with more with a message that says how many it found.
bool parse_graph_json(const std::string& text, Graph& graph, std::string& error);

/// \brief Load a single-chain rig from a file. \see parse_graph_json
bool load_graph_json(const std::string& path, Graph& graph, std::string& error);

/// \brief Rewrite every string parameter that names a file relative to \p base_dir, when that file is
/// there.
///
/// A rig that says "A2-Lite.nam" almost always means the file sitting next to it. For a document read
/// from a file the base is that file's directory; for one applied over the socket it is the rigs root,
/// which is the same trust boundary a `rig.load` name lives behind. A parameter whose file is not
/// there is left exactly as written, so "cannot load 'x.nam'" names what the author typed rather than
/// a path they never wrote.
void resolve_relative_paths(Document& document, const std::string& base_dir);

/// \brief Serialise a document back to JSON, in the canonical form the parser accepts.
///
/// What `rig.get` hands a client: the engine's own parse of the document it is running, including the
/// node ids it minted and the `element`/`role` of every node. It is a diagnostic rather than a store
/// (docs/ui-integration-plan.md §1.1) -- the app owns the document, and a client must not rehydrate a
/// library from this -- but it is the only way to see what a document *became* under the engine's
/// rules, which is what makes "did my edit land, and where" answerable.
std::string document_to_json(const Document& document);

/// \brief One graph as JSON: `{"nodes": [...], "connections": [...]}`, in the same spelling a
/// document's chain uses.
///
/// Used to show the *lowered* graph -- what the engine is actually running, after every macro has
/// been expanded -- which is the half of `rig.get` that a macro makes worth having: the document says
/// "a feedback delay", and this says which seven primitives that became.
std::string graph_to_json(const Graph& graph);

/// \brief Structural checks over a whole document, with the chain named in any failure.
///
/// One chain, one plan: the per-chain checks are \ref validate(const Graph&) unchanged, and this adds
/// only what is a property of the document -- that there is at least one chain, that chain ids are
/// present and unique, that the channel numbers are not negative.
Validation validate(const Document& document);

} // namespace graph
} // namespace namchain
