#pragma once

// The device's model and impulse-response library (docs/ui-integration-plan.md slice D).
//
// Why this exists: until now a `nc.nam` node named a *file*, so "which amps has this device got" was
// a question only the filesystem could answer, and the app had to be told the layout out of band. A
// registry turns the library into data the engine publishes -- `engine.assets` -- and turns a model
// into an **id** a document can name, which is what makes a rig portable between machines and what
// makes a palette possible at all (decision 5 of the plan: the amp/cab palette comes from the
// device's own library).
//
// Two roots, not one, because the operator's filing is the labelling: a `.nam` under `models_root` is
// a model, a `.nam` or `.wav` under `irs_root` is an impulse response. Nothing here reads a model to
// find out what it is -- see \ref sniff_nam for what is read and why.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace namchain
{
namespace graph
{
struct Document;
}

namespace assets
{

/// Which library an asset belongs to. Decided by the root it was found under.
enum class Kind
{
  Model,
  Ir,
};

/// `"model"` / `"ir"`: the value of \ref Asset::kind on the wire.
const char* kind_name(Kind kind);

/// The `options_from` string a node type declares for this kind (`"models"` / `"irs"`).
const char* kind_options_from(Kind kind);

/// \brief The kind a node parameter's `options_from` names.
///
/// \return false for an unknown string, which is a node type's bug rather than a client's: a
/// parameter that claims its options come from somewhere the engine does not have would otherwise
/// resolve to an empty list and look merely empty.
bool kind_from_options_from(const char* options_from, Kind& kind);

/// \brief One file in the library.
///
/// The fields are what a palette needs and what a test can assert: an identity to name, a label to
/// show, where it is, and the few facts that are cheap to know without loading it.
struct Asset
{
  /// What a document names: the path under the root with the extension removed, `/`-separated.
  /// `tweed/deluxe.nam` has id `tweed/deluxe`.
  std::string id;
  /// The path under the root *with* the extension, so a rig that names a file still matches exactly.
  std::string relative;
  /// The absolute path on this device. What the compiler is handed.
  std::string path;
  /// The file's stem, for a palette. Presentation beyond this is the app's business (§1.9).
  std::string name;
  /// The id's directory, or empty when the asset sits at the root of its library.
  std::string group;
  Kind kind = Kind::Model;
  std::uintmax_t size = 0;
  std::int64_t mtime = 0; ///< seconds since the epoch; the cache key together with \ref size
  /// The rate the file declares, or 0 for "not known without loading it". Advisory: the load is
  /// still what decides whether a model is usable at the engine's rate.
  double sample_rate = 0.0;
  /// WAV impulse responses: the number of frames, or 0 when the header did not say.
  std::uint64_t frames = 0;
  /// `.nam` files: `"WaveNet"`, `"LSTM"`, `"Linear"`, ... Empty when the file did not say.
  std::string architecture;
};

/// One root as the scan found it.
struct Root
{
  std::string path;
  bool readable = false; ///< a directory the scan could open and walk
  std::string error;     ///< why not, when it could not
  std::size_t count = 0; ///< assets found under it by the last scan
};

/// What the last scan actually did. \ref parsed is the number that makes the cache observable: a
/// refresh that finds nothing changed reads no file headers at all.
struct ScanStats
{
  std::size_t files = 0;   ///< regular files seen under both roots; a hidden file is not seen at all
  std::size_t parsed = 0;  ///< headers read, because the asset was new or had changed
  std::size_t cached = 0;  ///< assets described from the cache: same size and modification time
  std::size_t skipped = 0; ///< regular files that are not assets: wrong kind, unreadable, outside
};

/// \brief The model and IR library, scanned from two directories.
///
/// Not thread-safe and not meant to be: the control thread owns it, exactly as it owns compilation.
/// The render thread never sees it -- resolution happens at compile time, and a plan carries paths
/// that were resolved once.
class Registry
{
public:
  /// Point the registry at two roots. Does not scan; \ref refresh does that.
  void configure(std::string models_root, std::string irs_root);

  /// \brief Rescan both roots.
  ///
  /// An asset whose size and modification time are unchanged keeps everything parsed last time, so a
  /// refresh costs a stat per file rather than a read per file. A root that is missing or unreadable
  /// is *not* an error: it yields no assets and says why in \ref root, because a device with no IRs
  /// is a device with no IRs, not a daemon that will not start.
  ///
  /// The modification time used for the cache is the finest the filesystem offers (nanoseconds where
  /// the platform has them), not the whole seconds \ref Asset::mtime publishes: a model saved twice
  /// in the same second with the same size is exactly the case a cache keyed on seconds gets wrong.
  ScanStats refresh();

  /// Every asset from the last scan, ordered by id (and, within one id, by relative path).
  const std::vector<Asset>& assets() const { return assets_; }

  /// \brief The asset a reference names, or null.
  ///
  /// A reference is matched as a relative filename first (`tweed/deluxe.nam`, which is what a rig
  /// that names a file says) and then as an id (`tweed/deluxe`). The order matters only in the one
  /// case where two files share a stem, which is why it is stated: the exact filename wins.
  const Asset* find(Kind kind, const std::string& reference) const;

  /// \brief The nearest ids to \p reference, best first, at most \p limit of them.
  ///
  /// A hint for an error message, not a resolution: it is what turns "unknown model 'tweedy'" into
  /// "...did you mean 'tweed'".
  std::vector<std::string> suggest(Kind kind, const std::string& reference, std::size_t limit = 3) const;

  /// \brief Turn an asset reference into an absolute path.
  ///
  /// In order: a registry id (or a relative filename that matches one exactly); an absolute path that
  /// exists; a path relative to \p base_dir that exists.
  ///
  /// \param error a message naming the library, the reference and the near misses.
  bool resolve_path(Kind kind, const std::string& reference, const std::string& base_dir, std::string& path,
                    std::string& error) const;

  const Root& root(Kind kind) const { return kind == Kind::Model ? models_ : irs_; }
  const std::string& models_root() const { return models_.path; }
  const std::string& irs_root() const { return irs_.path; }
  /// How many scans have run, so a client can tell "refreshed" from "answered from the cache".
  std::size_t scans() const { return scans_; }
  const ScanStats& last_scan() const { return last_scan_; }
  /// Assets of one kind, from the last scan.
  std::vector<const Asset*> of_kind(Kind kind) const;

private:
  /// What was parsed for one file, and the stamp it was parsed at. The stamp is kept here rather than
  /// on \ref Asset because clients get whole seconds and the cache wants nanoseconds.
  struct CacheEntry
  {
    Asset asset;
    std::int64_t mtime_ns = 0;
  };

  std::string models_root_;
  std::string irs_root_;
  Root models_;
  Root irs_;
  std::vector<Asset> assets_;
  /// Absolute path -> what was parsed for it, kept across scans so an unchanged file costs a stat.
  std::map<std::string, CacheEntry> cache_;
  ScanStats last_scan_{};
  std::size_t scans_ = 0;
};

/// \brief A model or IR reference that resolved nowhere, and the block that stands in for it.
struct MissingAsset
{
  std::string chain;
  std::string node;
  /// The node's element (its id when the document gave none): what a client keys a block on.
  std::string element;
  /// The node's type as the document wrote it (`nc.nam`, `nc.cab.ir`, a macro).
  std::string type;
  /// The parameter that named the asset (`model`, `ir`).
  std::string param;
  Kind kind = Kind::Model;
  /// The reference as written.
  std::string reference;
  /// The registry's message: which library was searched, and the near misses.
  std::string message;
};

/// \brief Rewrite every asset reference in \p document into an absolute path.
///
/// Which strings are asset references is the *node type's* declaration -- a parameter whose schema
/// says `options_from` -- so this needs no list of its own, and a node type added later is covered
/// without touching this function. Called on the copy the daemon compiles, never on the document the
/// engine remembers and hands back, so `rig.get` stays a fixed point and a client keeps seeing its
/// own ids (docs/daemon.md §"Assets").
///
/// **Order, and a departure from the plan worth knowing about.** The daemon resolves a reference that
/// names a *file relative to the document* before it calls this (that is
/// \ref graph::resolve_relative_paths, which predates the registry), so the effective order for a rig
/// is: a file next to the rig, then the library, then an absolute path. Slice D of
/// docs/ui-integration-plan.md asks for the library first; this is deliberately the other way round,
/// because a hand-written rig that names the file sitting beside it must keep meaning that file, and
/// because the offline tools -- which have no registry -- must resolve the same file the daemon does.
/// The two orders agree whenever they can both answer, since a library id is normally
/// extension-less and a filename is not.
///
/// \param error a message naming the node, the reference and the near misses. The caller reports it
/// as `asset_missing`.
/// \param missing when given, a reference that resolves nowhere does not fail the document: the node
/// that names it is replaced, in \p document (the resolved copy), by an `nc.bypass` stand-in with the
/// same id, element and role, and the miss is appended here. The block then plays as if bypassed and
/// the client is told which one and why (`asset_missing` in `engine.status`), so a rig whose library
/// is incomplete -- a fresh card, a model deleted from the library -- still plays, rather than taking
/// the whole engine down with it. A node that is not 1->1 has no dry path to stand in with, and still
/// fails the document.
bool resolve_document(graph::Document& document, const Registry& registry, const std::string& base_dir,
                      std::string& error, std::vector<MissingAsset>* missing = nullptr);

/// \brief The two `.nam` fields a palette wants, read without loading the model.
///
/// A NAM file is JSON whose `weights` array can be hundreds of kilobytes, and the two fields worth
/// publishing sit at opposite ends of it: `architecture` near the front, `sample_rate` at the very
/// end in every file the reference writer produces. So this reads the front \ref kSniffPrefixBytes
/// and the last \ref kSniffTailBytes and looks for two keys -- a sniff, not a parse.
///
/// The consequence is stated rather than hidden: a file that puts those fields somewhere else reports
/// nothing for them, which is a palette with no rate shown rather than a wrong rate. Nothing
/// functional depends on the answer -- the load is still what decides whether a model is usable.
void sniff_nam(const std::string& path, std::string& architecture, double& sample_rate);

/// \brief A WAV's rate, channel count and length, from its header.
///
/// Exact rather than a sniff, and still cheap: `fmt ` is at a fixed place in any RIFF file and a
/// canonical one puts `data` right after it. \return false when the file does not begin with a RIFF
/// WAVE header, or when the header does not fit in the first \ref kWavProbeBytes bytes.
bool probe_wav(const std::string& path, double& sample_rate, unsigned& channels, std::uint64_t& frames);

/// How much of a `.nam`'s front and tail \ref sniff_nam reads.
inline constexpr std::size_t kSniffPrefixBytes = 16u * 1024;
inline constexpr std::size_t kSniffTailBytes = 1024;
/// How much of a WAV \ref probe_wav reads.
inline constexpr std::size_t kWavProbeBytes = 4096;

} // namespace assets
} // namespace namchain
