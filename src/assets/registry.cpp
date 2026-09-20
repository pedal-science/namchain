#include "namchain/assets/registry.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"

namespace namchain
{
namespace assets
{
namespace
{

std::string lowercase(std::string text)
{
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

/// \brief Read at most \p limit bytes from the front of \p path.
bool read_prefix(const std::string& path, std::size_t limit, std::string& out)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  out.assign(limit, '\0');
  file.read(out.data(), static_cast<std::streamsize>(limit));
  out.resize(static_cast<std::size_t>(file.gcount()));
  return true;
}

/// \brief Read at most \p limit bytes from the end of \p path.
///
/// The tail is where the reference NAM writer puts `sample_rate`, after the weights, so a scan that
/// only read the front would report no rate for every real model.
bool read_tail(const std::string& path, std::size_t limit, std::string& out)
{
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return false;
  const std::streamoff size = file.tellg();
  if (size <= 0)
  {
    out.clear();
    return true;
  }
  const std::streamoff want = std::min<std::streamoff>(static_cast<std::streamoff>(limit), size);
  file.seekg(size - want);
  out.assign(static_cast<std::size_t>(want), '\0');
  file.read(out.data(), static_cast<std::streamsize>(want));
  out.resize(static_cast<std::size_t>(file.gcount()));
  return true;
}

/// Skip whitespace, and the ':' a JSON object puts after a key.
const char* after_key(const std::string& text, const char* key)
{
  const std::string pattern = std::string("\"") + key + "\"";
  const std::size_t found = text.find(pattern);
  if (found == std::string::npos)
    return nullptr;
  const char* at = text.data() + found + pattern.size();
  while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r')
    ++at;
  if (*at != ':')
    return nullptr;
  ++at;
  while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r')
    ++at;
  return at;
}

/// The value of a JSON string field, or false when the key is absent or its value is not a string.
bool json_string_field(const std::string& text, const char* key, std::string& out)
{
  const char* at = after_key(text, key);
  if (at == nullptr || *at != '"')
    return false;
  ++at;
  const char* end = at;
  while (*end != '\0' && *end != '"')
    ++end;
  if (*end != '"')
    return false;
  out.assign(at, static_cast<std::size_t>(end - at));
  return true;
}

/// The value of a JSON number field, or false when the key is absent or its value is not a number.
bool json_number_field(const std::string& text, const char* key, double& out)
{
  const char* at = after_key(text, key);
  if (at == nullptr)
    return false;
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(at, &end);
  if (end == at || errno != 0)
    return false;
  out = value;
  return true;
}

std::uint32_t read_u32(const unsigned char* bytes)
{
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint16_t read_u16(const unsigned char* bytes)
{
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                    (static_cast<std::uint16_t>(bytes[1]) << 8));
}

/// The extensions a root accepts. A model is a `.nam`; an impulse response is a `.nam` (the core's
/// own container for a `Linear` model) or a `.wav`.
bool accepts_extension(Kind kind, const std::string& extension)
{
  if (extension == ".nam")
    return true;
  if (extension == ".wav")
    return kind == Kind::Ir;
  return false;
}

/// \brief Levenshtein distance, capped: anything above \p cap is reported as cap + 1.
///
/// Bounded because the only use is "is this close enough to be worth suggesting", and a full
/// distance matrix for a long reference against every id in the library is work with no answer at
/// the end of it.
int edit_distance(const std::string& a, const std::string& b, int cap)
{
  const std::size_t n = a.size();
  const std::size_t m = b.size();
  if (n > m + static_cast<std::size_t>(cap) || m > n + static_cast<std::size_t>(cap))
    return cap + 1;

  std::vector<int> previous(m + 1);
  std::vector<int> current(m + 1);
  for (std::size_t j = 0; j <= m; ++j)
    previous[j] = static_cast<int>(j);

  for (std::size_t i = 1; i <= n; ++i)
  {
    current[0] = static_cast<int>(i);
    int row_min = current[0];
    for (std::size_t j = 1; j <= m; ++j)
    {
      const int substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
      row_min = std::min(row_min, current[j]);
    }
    if (row_min > cap)
      return cap + 1;
    previous.swap(current);
  }
  return previous[m] > cap ? cap + 1 : previous[m];
}

/// The last component of an id: `tweed/deluxe` -> `deluxe`.
std::string leaf(const std::string& id)
{
  const std::size_t slash = id.rfind('/');
  return slash == std::string::npos ? id : id.substr(slash + 1);
}

} // namespace

const char* kind_name(Kind kind)
{
  return kind == Kind::Model ? "model" : "ir";
}

const char* kind_options_from(Kind kind)
{
  return kind == Kind::Model ? "models" : "irs";
}

bool kind_from_options_from(const char* options_from, Kind& kind)
{
  if (options_from == nullptr)
    return false;
  if (std::strcmp(options_from, "models") == 0)
  {
    kind = Kind::Model;
    return true;
  }
  if (std::strcmp(options_from, "irs") == 0)
  {
    kind = Kind::Ir;
    return true;
  }
  return false;
}

void sniff_nam(const std::string& path, std::string& architecture, double& sample_rate)
{
  architecture.clear();
  sample_rate = 0.0;

  std::string front;
  if (read_prefix(path, kSniffPrefixBytes, front))
  {
    json_string_field(front, "architecture", architecture);
    json_number_field(front, "sample_rate", sample_rate);
  }
  if (sample_rate != 0.0 && !architecture.empty())
    return;

  // Only the tail when the front did not already answer, which is the common case for a model whose
  // weights push `sample_rate` to the end of the file.
  std::string tail;
  if (read_tail(path, kSniffTailBytes, tail))
  {
    if (sample_rate == 0.0)
      json_number_field(tail, "sample_rate", sample_rate);
    if (architecture.empty())
      json_string_field(tail, "architecture", architecture);
  }
}

bool probe_wav(const std::string& path, double& sample_rate, unsigned& channels, std::uint64_t& frames)
{
  sample_rate = 0.0;
  channels = 0;
  frames = 0;

  std::string header;
  if (!read_prefix(path, kWavProbeBytes, header) || header.size() < 12)
    return false;

  const auto* bytes = reinterpret_cast<const unsigned char*>(header.data());
  if (std::memcmp(bytes, "RIFF", 4) != 0 || std::memcmp(bytes + 8, "WAVE", 4) != 0)
    return false;

  unsigned bits = 0;
  std::uint32_t data_bytes = 0;
  bool have_format = false;
  std::size_t at = 12;
  while (at + 8 <= header.size())
  {
    const auto* chunk = bytes + at;
    const std::uint32_t size = read_u32(chunk + 4);
    const std::size_t payload = at + 8;
    if (std::memcmp(chunk, "fmt ", 4) == 0 && payload + 16 <= header.size())
    {
      channels = read_u16(bytes + payload + 2);
      sample_rate = static_cast<double>(read_u32(bytes + payload + 4));
      bits = read_u16(bytes + payload + 14);
      have_format = true;
    }
    else if (std::memcmp(chunk, "data", 4) == 0)
    {
      data_bytes = size;
      break;
    }
    // Chunks are word-aligned, so an odd size is followed by a pad byte.
    at = payload + size + (size % 2);
  }

  if (!have_format)
    return false;
  const unsigned frame_bytes = (bits / 8) * (channels == 0 ? 1u : channels);
  frames = frame_bytes == 0 ? 0 : data_bytes / frame_bytes;
  return true;
}

void Registry::configure(std::string models_root, std::string irs_root)
{
  models_root_ = std::move(models_root);
  irs_root_ = std::move(irs_root);
  models_.path = models_root_;
  irs_.path = irs_root_;
}

std::vector<const Asset*> Registry::of_kind(Kind kind) const
{
  std::vector<const Asset*> result;
  for (const Asset& asset : assets_)
  {
    if (asset.kind == kind)
      result.push_back(&asset);
  }
  return result;
}

const Asset* Registry::find(Kind kind, const std::string& reference) const
{
  // The exact relative filename first, then the id. Two files sharing a stem (`cab.nam` and
  // `cab.wav` as IRs) both answer to the id `cab`; a rig that names one of them exactly gets the one
  // it named.
  for (const Asset& asset : assets_)
  {
    if (asset.kind == kind && asset.relative == reference)
      return &asset;
  }
  for (const Asset& asset : assets_)
  {
    if (asset.kind == kind && asset.id == reference)
      return &asset;
  }
  // A reference that spells the id with its extension (`tweed/deluxe.nam`) against an asset whose
  // relative path is stored with a different case is a miss, which is deliberate: ids are exact.
  return nullptr;
}

std::vector<std::string> Registry::suggest(Kind kind, const std::string& reference, std::size_t limit) const
{
  const std::string want = lowercase(reference);
  std::vector<std::pair<int, std::string>> scored;
  for (const Asset& asset : assets_)
  {
    if (asset.kind != kind)
      continue;
    const std::string full = lowercase(asset.id);
    const std::string last = lowercase(leaf(asset.id));
    int score = std::min(edit_distance(want, full, 3), edit_distance(want, last, 3));
    // A reference that is contained in an id, or contains one, is worth offering even when the edit
    // distance between the two whole strings is large: "deluxe" should find "tweed/deluxe-reverb".
    if (full.find(want) != std::string::npos || last.find(want) != std::string::npos ||
        want.find(last) != std::string::npos)
      score = std::min(score, 1);
    if (score <= 2)
      scored.emplace_back(score, asset.id);
  }
  std::sort(scored.begin(), scored.end());
  scored.erase(std::unique(scored.begin(), scored.end(),
                           [](const auto& a, const auto& b) { return a.second == b.second; }),
               scored.end());

  std::vector<std::string> result;
  for (const auto& [score, id] : scored)
  {
    if (result.size() >= limit)
      break;
    result.push_back(id);
  }
  return result;
}

bool Registry::resolve_path(Kind kind, const std::string& reference, const std::string& base_dir,
                            std::string& path, std::string& error) const
{
  if (reference.empty())
  {
    error = std::string("the ") + kind_name(kind) + " reference is empty";
    return false;
  }

  // 1. The library. An id, or a relative filename that matches one exactly.
  if (const Asset* asset = find(kind, reference))
  {
    path = asset->path;
    return true;
  }

  // 2. A path that exists: as written when it is absolute, and relative to the directory the
  //    document came from otherwise. This is the route every rig written before this slice takes, so
  //    it is checked before the reference is declared unknown rather than after.
  std::error_code ec;
  const std::filesystem::path written(reference);
  const std::filesystem::path candidate =
    written.is_absolute() || base_dir.empty() ? written : std::filesystem::path(base_dir) / written;
  if (std::filesystem::is_regular_file(candidate, ec))
  {
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
    path = ec ? candidate.string() : canonical.string();
    return true;
  }

  // 3. Neither. Say which library was searched, how big it is, and what it has that is close -- an
  //    unknown model is nearly always a typo or a stale palette, and both are fixable from here.
  const Root& root = this->root(kind);
  const std::size_t count = of_kind(kind).size();
  std::string message = "no " + std::string(kind_name(kind)) + " '" + reference + "' in '" + root.path + "'";
  if (!root.readable)
    message += " (which is not readable: " + (root.error.empty() ? "no such directory" : root.error) + ")";
  else
    message += " (" + std::to_string(count) + " " + std::string(kind_name(kind)) + (count == 1 ? "" : "s") +
               " in the library)";

  const std::vector<std::string> near = suggest(kind, reference, 3);
  if (!near.empty())
  {
    message += "; did you mean ";
    for (std::size_t i = 0; i < near.size(); ++i)
      message += (i == 0 ? "'" : i + 1 == near.size() ? " or '" : ", '") + near[i] + "'";
  }
  message += "; a reference is a library id, a path relative to the rig, or an absolute path";
  error = message;
  return false;
}

ScanStats Registry::refresh()
{
  ++scans_;
  ScanStats stats;
  std::map<std::string, CacheEntry> parsed;
  std::vector<Asset> found;

  const auto scan_root = [&](Kind kind, const std::string& root_path, Root& root) {
    root.path = root_path;
    root.readable = false;
    root.error.clear();
    root.count = 0;
    if (root_path.empty())
    {
      root.error = "no directory is configured";
      return;
    }

    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(root_path, ec);
    if (!std::filesystem::is_directory(status))
    {
      // Three ways to have no library, spelled three ways: a path that is not there, a path that is
      // there but is a file, and a path that could not be looked at. "Not readable" for all three
      // would be true and useless.
      if (std::filesystem::exists(status))
        root.error = "not a directory";
      else if (ec && ec != std::errc::no_such_file_or_directory)
        root.error = ec.message();
      else
        root.error = "no such directory";
      return;
    }
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root_path, ec);
    if (ec)
    {
      root.error = ec.message();
      return;
    }
    root.readable = true;

    std::filesystem::recursive_directory_iterator it(canonical_root,
                                                     std::filesystem::directory_options::skip_permission_denied,
                                                     ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec))
    {
      if (ec)
      {
        // A directory that vanished or is unreadable costs that directory, not the scan: a library
        // being reorganised while the daemon runs should not empty the palette.
        ++stats.skipped;
        ec.clear();
        continue;
      }
      const std::filesystem::directory_entry& entry = *it;
      const std::string filename = entry.path().filename().string();
      if (!filename.empty() && filename[0] == '.')
      {
        if (entry.is_directory(ec))
          it.disable_recursion_pending();
        continue;
      }
      if (!entry.is_regular_file(ec))
        continue;
      ++stats.files;

      const std::string extension = lowercase(entry.path().extension().string());
      if (!accepts_extension(kind, extension))
      {
        ++stats.skipped;
        continue;
      }

      const std::filesystem::path relative = entry.path().lexically_relative(canonical_root);
      const std::string relative_text = relative.generic_string();
      const std::string absolute = (canonical_root / relative).string();

      // A symlink is followed only while it stays inside the root, which is the rule `rig.load`
      // already lives by: an id means "a file in this library", and a link out of it would make that
      // a promise the registry cannot keep.
      if (entry.is_symlink(ec))
      {
        const std::filesystem::path target = std::filesystem::weakly_canonical(entry.path(), ec);
        const std::string target_text = ec ? std::string() : target.string();
        const std::string root_text = canonical_root.string();
        const bool inside = !target_text.empty() && target_text.size() > root_text.size() &&
                            target_text.compare(0, root_text.size(), root_text) == 0 &&
                            target_text[root_text.size()] == '/';
        if (!inside)
        {
          ++stats.skipped;
          continue;
        }
      }

      struct stat info{};
      if (::stat(absolute.c_str(), &info) != 0)
      {
        ++stats.skipped;
        continue;
      }
#if defined(__linux__)
      const std::int64_t mtime_ns =
        static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1000000000ll + info.st_mtim.tv_nsec;
#else
      const std::int64_t mtime_ns = static_cast<std::int64_t>(info.st_mtime) * 1000000000ll;
#endif

      Asset asset;
      asset.kind = kind;
      asset.relative = relative_text;
      asset.id = relative_text.substr(0, relative_text.size() - extension.size());
      asset.path = absolute;
      asset.name = entry.path().stem().string();
      const std::size_t slash = asset.id.rfind('/');
      asset.group = slash == std::string::npos ? std::string() : asset.id.substr(0, slash);
      asset.size = static_cast<std::uintmax_t>(info.st_size);
      asset.mtime = static_cast<std::int64_t>(info.st_mtime);

      const auto previous = cache_.find(absolute);
      if (previous != cache_.end() && previous->second.asset.size == asset.size &&
          previous->second.mtime_ns == mtime_ns && previous->second.asset.kind == kind)
      {
        // Unchanged: keep what was parsed last time, including the architecture and rate.
        asset.sample_rate = previous->second.asset.sample_rate;
        asset.frames = previous->second.asset.frames;
        asset.architecture = previous->second.asset.architecture;
        ++stats.cached;
      }
      else
      {
        if (extension == ".nam")
        {
          sniff_nam(absolute, asset.architecture, asset.sample_rate);
        }
        else
        {
          unsigned channels = 0;
          if (!probe_wav(absolute, asset.sample_rate, channels, asset.frames))
          {
            asset.sample_rate = 0.0;
            asset.frames = 0;
          }
        }
        ++stats.parsed;
      }

      parsed.emplace(absolute, CacheEntry{asset, mtime_ns});
      found.push_back(std::move(asset));
    }
  };

  scan_root(Kind::Model, models_root_, models_);
  scan_root(Kind::Ir, irs_root_, irs_);

  std::sort(found.begin(), found.end(), [](const Asset& a, const Asset& b) {
    if (a.kind != b.kind)
      return a.kind == Kind::Model;
    if (a.id != b.id)
      return a.id < b.id;
    return a.relative < b.relative;
  });

  assets_ = std::move(found);
  for (const Asset& asset : assets_)
  {
    if (asset.kind == Kind::Model)
      ++models_.count;
    else
      ++irs_.count;
  }
  // Pruning by replacement: a file that is gone is gone from the cache too, so a long-running daemon
  // does not accumulate a description of every model that ever sat in the library.
  cache_ = std::move(parsed);
  last_scan_ = stats;
  return stats;
}

bool resolve_document(graph::Document& document, const Registry& registry, const std::string& base_dir,
                      std::string& error)
{
  for (graph::Chain& chain : document.chains)
  {
    for (graph::NodeSpec& node : chain.graph.nodes)
    {
      const render::NodeType* type = render::find_node_type(node.type);
      if (type == nullptr || type->params == nullptr)
        continue; // an unknown node type is the compiler's error to report, not this function's

      for (int i = 0; i < type->param_count; ++i)
      {
        const render::ParamDesc& param = type->params[i];
        if (param.options_from == nullptr)
          continue;

        Kind kind = Kind::Model;
        if (!kind_from_options_from(param.options_from, kind))
        {
          error = "node '" + node.id + "' (" + node.type + "): parameter '" + param.id +
                  "' declares options_from '" + param.options_from + "', which this engine does not have";
          return false;
        }

        const auto value = node.strings.find(param.id);
        if (value == node.strings.end())
          continue; // missing is the schema validator's business, and it says so better

        std::string path;
        std::string reason;
        if (!registry.resolve_path(kind, value->second, base_dir, path, reason))
        {
          error = "chain '" + chain.id + "': node '" + node.id + "' (" + node.type + "): " + reason;
          return false;
        }
        value->second = path;
      }
    }
  }
  return true;
}

} // namespace assets
} // namespace namchain
