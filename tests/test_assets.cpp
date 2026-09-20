// Tests for the model/IR registry (docs/ui-integration-plan.md slice D, docs/assets.md).
//
// The registry's whole job is to answer two questions cheaply and honestly -- "what is in the
// library" and "what does this reference mean" -- so the cases here are about the answers being
// *shaped* right (ids, kinds, labels, and what a palette can show) and about the scan not doing work
// it does not have to. The fixtures are files this test writes, because the registry never loads a
// model: a `.nam` here is a JSON header and a pile of numbers, which is exactly what a real one is to
// this code. The one place a real model is needed -- that an id compiles into a plan -- is
// tests/test_daemon.cpp, where a daemon and the generated fixtures are already in scope.
#include <doctest.h>

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "namchain/assets/registry.h"
#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"
#include "namchain/wav.h"

namespace
{

using namchain::assets::Asset;
using namchain::assets::Kind;
using namchain::assets::Registry;

/// A unique directory under the system temp dir, removed on destruction.
struct TempDir
{
  TempDir()
  {
    static std::atomic<unsigned> counter{0};
    path = std::filesystem::temp_directory_path() /
           ("namchain-assets-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(path);
  }
  ~TempDir()
  {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::filesystem::path path;
};

void write_text(const std::filesystem::path& path, const std::string& contents)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  REQUIRE(file.good());
  file << contents;
}

/// \brief A `.nam` file with the fields where the reference writer puts them.
///
/// `architecture` goes near the front and `sample_rate` right at the end, after the weights, which is
/// what the vendored core's own files do -- and the reason the registry reads both ends of a file
/// rather than assuming a header. \p weights numbers of padding push the file past the prefix the
/// sniff reads, which is what makes that a real test rather than a coincidence of small fixtures.
void write_nam(const std::filesystem::path& path, const std::string& architecture, double sample_rate,
               bool rate_at_end = true, std::size_t weights = 4000)
{
  std::string numbers;
  for (std::size_t i = 0; i < weights; ++i)
  {
    numbers += std::to_string(0.001 * static_cast<double>(i % 997));
    if (i + 1 < weights)
      numbers += ", ";
  }
  std::string body = "{\"version\": \"0.7.0\", \"architecture\": \"" + architecture +
                     "\", \"config\": {\"layers\": []}";
  if (!rate_at_end)
    body += ", \"sample_rate\": " + std::to_string(static_cast<long long>(sample_rate));
  body += ", \"weights\": [" + numbers + "]";
  if (rate_at_end)
    body += ", \"sample_rate\": " + std::to_string(static_cast<long long>(sample_rate));
  body += "}";
  write_text(path, body);
}

void write_wav(const std::filesystem::path& path, unsigned frames, unsigned channels, double sample_rate)
{
  std::filesystem::create_directories(path.parent_path());
  std::vector<float> samples(static_cast<std::size_t>(frames) * channels, 0.0f);
  std::string error;
  REQUIRE_MESSAGE(namchain::save_wav_float32(path.string(), samples.data(), frames,
                                             static_cast<int>(channels), sample_rate, error),
                  error);
}

/// A models root and an IR root with a small, known library in them.
struct Library
{
  Library()
  {
    models = dir.path / "models";
    irs = dir.path / "irs";
    write_nam(models / "tweed/deluxe.nam", "WaveNet", 48000.0);
    write_nam(models / "tweed/bassman.nam", "LSTM", 44100.0, /*rate_at_end=*/false);
    write_nam(models / "clean.nam", "WaveNet", 48000.0);
    write_text(models / "readme.txt", "not a model");
    write_text(models / ".hidden.nam", "{}");
    write_wav(models / "not-a-model.wav", 16, 1, 48000.0); // a WAV is not a model

    write_nam(irs / "cab-a.nam", "Linear", 0.0);
    write_wav(irs / "cab-b.wav", 256, 1, 48000.0);
    write_wav(irs / "stereo.wav", 128, 2, 44100.0);
  }

  Registry registry() const
  {
    Registry registry;
    registry.configure(models.string(), irs.string());
    registry.refresh();
    return registry;
  }

  // `dir` first: the roots are built from its path, and a member that reads another member has to
  // come after it (declaration order is initialization order).
  TempDir dir;
  std::filesystem::path models;
  std::filesystem::path irs;
};

const Asset* find_by_id(const Registry& registry, Kind kind, const std::string& id)
{
  for (const Asset* asset : registry.of_kind(kind))
  {
    if (asset->id == id)
      return asset;
  }
  return nullptr;
}

} // namespace

TEST_CASE("a scan describes the library it found")
{
  Library library;
  Registry registry = library.registry();

  REQUIRE(registry.root(Kind::Model).readable);
  REQUIRE(registry.root(Kind::Ir).readable);
  CHECK(registry.root(Kind::Model).count == 3);
  CHECK(registry.root(Kind::Ir).count == 3);
  CHECK(registry.assets().size() == 6);
  CHECK(registry.scans() == 1);

  // An id is the path under the root without its extension; the group is the directory it came from,
  // which is all a palette needs to show a folder.
  const Asset* deluxe = find_by_id(registry, Kind::Model, "tweed/deluxe");
  REQUIRE(deluxe != nullptr);
  CHECK(deluxe->relative == "tweed/deluxe.nam");
  CHECK(deluxe->name == "deluxe");
  CHECK(deluxe->group == "tweed");
  CHECK(deluxe->kind == Kind::Model);
  CHECK(deluxe->path == std::filesystem::weakly_canonical(library.models / "tweed/deluxe.nam").string());
  CHECK(deluxe->size > 16000); // big enough that a front-only sniff would have missed the rate
  CHECK(deluxe->mtime > 0);
  CHECK(deluxe->architecture == "WaveNet");
  CHECK(deluxe->sample_rate == doctest::Approx(48000.0));

  // A file that declares its rate near the front is read the same way as one that declares it at the
  // end. Both spellings exist in the wild and neither is a reason to load the model.
  const Asset* bassman = find_by_id(registry, Kind::Model, "tweed/bassman");
  REQUIRE(bassman != nullptr);
  CHECK(bassman->architecture == "LSTM");
  CHECK(bassman->sample_rate == doctest::Approx(44100.0));

  // An IR is a `.nam` or a `.wav`, and the WAV route reports what its header says: rate, and how many
  // frames of it there are.
  const Asset* cab = find_by_id(registry, Kind::Ir, "cab-b");
  REQUIRE(cab != nullptr);
  CHECK(cab->kind == Kind::Ir);
  CHECK(cab->sample_rate == doctest::Approx(48000.0));
  CHECK(cab->frames == 256);
  CHECK(cab->architecture.empty()); // a WAV has no architecture to declare

  const Asset* linear = find_by_id(registry, Kind::Ir, "cab-a");
  REQUIRE(linear != nullptr);
  CHECK(linear->architecture == "Linear");
  CHECK(linear->sample_rate == doctest::Approx(0.0)); // it declares none, and none is invented

  // Everything that is not the root's kind is ignored rather than guessed at.
  CHECK(find_by_id(registry, Kind::Model, "readme") == nullptr);
  CHECK(find_by_id(registry, Kind::Model, "not-a-model") == nullptr);
  CHECK(find_by_id(registry, Kind::Model, ".hidden") == nullptr);

  // Ordered by kind, then id: a listing that changes order between scans is a listing a client
  // cannot diff.
  CHECK(registry.assets().front().id == "clean");
  CHECK(registry.assets().front().kind == Kind::Model);
  CHECK(registry.assets().back().id == "stereo");
}

TEST_CASE("an unchanged file is not read again and a changed one is")
{
  Library library;
  Registry registry;
  registry.configure(library.models.string(), library.irs.string());

  const namchain::assets::ScanStats first = registry.refresh();
  // Eight regular files are seen: six that are assets, `readme.txt`, and a `.wav` in the models root
  // (which is not a model). Nothing is cached on a first scan.
  CHECK(first.files == 8);
  CHECK(first.parsed == 6);
  CHECK(first.cached == 0);
  CHECK(first.skipped == 2);

  // The second scan reads no file headers at all: size and mtime are the cache key, and this is what
  // makes a refresh cost a stat per file rather than a read per file. \ref ScanStats::parsed is the
  // number that makes that observable rather than a claim in a comment.
  const namchain::assets::ScanStats second = registry.refresh();
  CHECK(second.files == 8);
  CHECK(second.parsed == 0);
  CHECK(second.cached == 6);
  CHECK(registry.scans() == 2);

  // A file that changes is re-read, and only that file -- and this one changes *without* changing
  // size (48000 and 96000 are both five digits), which is why the cache key is not the whole-second
  // stamp a client is shown.
  write_nam(library.models / "clean.nam", "WaveNet", 96000.0);
  const namchain::assets::ScanStats third = registry.refresh();
  CHECK(third.parsed == 1);
  CHECK(third.cached == 5);
  const Asset* clean = find_by_id(registry, Kind::Model, "clean");
  REQUIRE(clean != nullptr);
  CHECK(clean->sample_rate == doctest::Approx(96000.0));

  // A file that is removed leaves the library, and leaves the cache with it.
  std::filesystem::remove(library.models / "tweed/bassman.nam");
  const namchain::assets::ScanStats fourth = registry.refresh();
  CHECK(fourth.files == 7);
  CHECK(find_by_id(registry, Kind::Model, "tweed/bassman") == nullptr);
  CHECK(registry.root(Kind::Model).count == 2);
}

TEST_CASE("a model added while the daemon runs appears on the next scan")
{
  Library library;
  Registry registry;
  registry.configure(library.models.string(), library.irs.string());
  registry.refresh();
  CHECK(find_by_id(registry, Kind::Model, "new-amp") == nullptr);

  write_nam(library.models / "new-amp.nam", "WaveNet", 48000.0);

  // Not until the scan is asked for: holding a listing still is what lets a client diff two of them
  // and know that a change it sees is a change on disk.
  CHECK(find_by_id(registry, Kind::Model, "new-amp") == nullptr);
  registry.refresh();
  const Asset* added = find_by_id(registry, Kind::Model, "new-amp");
  REQUIRE(added != nullptr);
  CHECK(added->architecture == "WaveNet");
}

TEST_CASE("a reference resolves by id, by filename, and by path")
{
  Library library;
  Registry registry = library.registry();

  std::string path;
  std::string error;

  // By id, which is the spelling a client uses because a path may not cross the network.
  CHECK(registry.resolve_path(Kind::Model, "tweed/deluxe", "", path, error));
  CHECK(path == std::filesystem::weakly_canonical(library.models / "tweed/deluxe.nam").string());

  // By the relative filename, which is what a hand-written rig says.
  CHECK(registry.resolve_path(Kind::Model, "tweed/deluxe.nam", "", path, error));
  CHECK(path == std::filesystem::weakly_canonical(library.models / "tweed/deluxe.nam").string());

  // By a path relative to the document, and by an absolute path: both routes predate the registry and
  // both have to keep working, which is why a reference that names a file is never declared unknown.
  const auto elsewhere = library.dir.path / "elsewhere";
  write_nam(elsewhere / "loose.nam", "WaveNet", 48000.0);
  CHECK(registry.resolve_path(Kind::Model, "loose.nam", elsewhere.string(), path, error));
  CHECK(path == std::filesystem::weakly_canonical(elsewhere / "loose.nam").string());
  CHECK(registry.resolve_path(Kind::Model, (elsewhere / "loose.nam").string(), "", path, error));
  CHECK(path == std::filesystem::weakly_canonical(elsewhere / "loose.nam").string());

  // The exact filename wins over the id when two files share a stem, and the id still resolves.
  CHECK(registry.resolve_path(Kind::Ir, "cab-b.wav", "", path, error));
  CHECK(path == std::filesystem::weakly_canonical(library.irs / "cab-b.wav").string());
  CHECK(registry.resolve_path(Kind::Ir, "cab-b", "", path, error));
  CHECK(path == std::filesystem::weakly_canonical(library.irs / "cab-b.wav").string());

  // A kind is a separate namespace: a model id is not an IR.
  CHECK_FALSE(registry.resolve_path(Kind::Ir, "tweed/deluxe", "", path, error));
  CHECK(error.find("no ir") != std::string::npos);
}

TEST_CASE("an unknown reference is refused with the near misses")
{
  Library library;
  Registry registry = library.registry();

  std::string path;
  std::string error;
  CHECK_FALSE(registry.resolve_path(Kind::Model, "tweed/delux", "", path, error));
  CHECK(error.find("tweed/deluxe") != std::string::npos); // one edit away, and offered
  CHECK(error.find("no model 'tweed/delux'") != std::string::npos);
  CHECK(error.find(library.models.string()) != std::string::npos);

  // A reference that contains a whole id is offered too: this is the "which amp was it called"
  // search, where the edit distance between the two whole strings means nothing.
  CHECK_FALSE(registry.resolve_path(Kind::Model, "deluxe-reverb?", "", path, error));
  CHECK(error.find("tweed/deluxe") != std::string::npos);

  // Nothing close: the message still says where it looked and how big the library is, because that
  // is the difference between "empty library" and "typo".
  CHECK_FALSE(registry.resolve_path(Kind::Model, "zzzzzzzz", "", path, error));
  CHECK(error.find("3 models in the library") != std::string::npos);

  CHECK(registry.suggest(Kind::Model, "tweed/delux", 3).size() == 1);
  CHECK(registry.suggest(Kind::Model, "tweed/delux", 3).front() == "tweed/deluxe");
  CHECK(registry.suggest(Kind::Model, "zzzzzzzz", 3).empty());
}

TEST_CASE("a root that is missing or has nothing in it is empty rather than fatal")
{
  TempDir dir;
  Registry registry;
  registry.configure((dir.path / "does-not-exist").string(), dir.path.string());
  registry.refresh();

  CHECK_FALSE(registry.root(Kind::Model).readable);
  CHECK(registry.root(Kind::Model).error == "no such directory");
  CHECK(registry.root(Kind::Model).count == 0);
  CHECK(registry.root(Kind::Ir).readable);
  CHECK(registry.of_kind(Kind::Model).empty());

  std::string path;
  std::string error;
  CHECK_FALSE(registry.resolve_path(Kind::Model, "anything", "", path, error));
  CHECK(error.find("not readable") != std::string::npos);

  // A path that exists but is a file says so, rather than reporting the same "not readable" as a
  // path that is not there.
  TempDir other;
  write_text(other.path / "a-file", "{}");
  Registry confused;
  confused.configure((other.path / "a-file").string(), "");
  confused.refresh();
  CHECK(confused.root(Kind::Model).error == "not a directory");
  CHECK(confused.root(Kind::Ir).error == "no directory is configured");

  // A second refresh after the root appears fills it in: nothing about a missing directory is sticky.
  write_nam(dir.path / "later.nam", "WaveNet", 48000.0);
  Registry second;
  second.configure(dir.path.string(), (dir.path / "no-irs").string());
  second.refresh();
  CHECK(find_by_id(second, Kind::Model, "later") != nullptr);
  CHECK_FALSE(second.root(Kind::Ir).readable);
}

TEST_CASE("a symlink is followed only while it stays inside the library")
{
  Library library;
  write_nam(library.dir.path / "outside.nam", "WaveNet", 48000.0);

  std::error_code ec;
  std::filesystem::create_symlink(library.dir.path / "outside.nam", library.models / "linked-out.nam", ec);
  REQUIRE_FALSE(ec);
  std::filesystem::create_symlink(library.models / "clean.nam", library.models / "linked-in.nam", ec);
  REQUIRE_FALSE(ec);

  Registry registry = library.registry();
  // An id promises "a file in this library". A link out of it would make that a promise the registry
  // cannot keep, so it is skipped rather than published as an asset of the device.
  CHECK(find_by_id(registry, Kind::Model, "linked-out") == nullptr);
  CHECK(find_by_id(registry, Kind::Model, "linked-in") != nullptr);
  CHECK(registry.last_scan().skipped >= 1);
}

TEST_CASE("a document's asset parameters are resolved and nothing else is touched")
{
  Library library;
  Registry registry = library.registry();

  namchain::graph::Document document;
  std::string error;
  const char* const catalogue = R"({
    "name": "assets",
    "chains": [{
      "id": "main",
      "nodes": [
        { "id": "in", "type": "nc.input" },
        { "id": "amp", "type": "nc.nam", "model": "tweed/deluxe" },
        { "id": "cab", "type": "nc.cab.ir", "ir": "cab-b.wav" },
        { "id": "out", "type": "nc.output" }
      ],
      "connections": [
        { "from": "in", "to": "amp" }, { "from": "amp", "to": "cab" }, { "from": "cab", "to": "out" }
      ]
    }]
  })";
  const bool parsed = namchain::graph::parse_document_json(catalogue, document, error);
  REQUIRE_MESSAGE(parsed, error);

  REQUIRE_MESSAGE(namchain::assets::resolve_document(document, registry, "", error), error);
  const auto& nodes = document.chains.front().graph.nodes;
  CHECK(nodes[1].strings.at("model") ==
        std::filesystem::weakly_canonical(library.models / "tweed/deluxe.nam").string());
  CHECK(nodes[2].strings.at("ir") ==
        std::filesystem::weakly_canonical(library.irs / "cab-b.wav").string());

  // An unknown reference fails the document with the registry's own message, so a client sees the
  // same near misses whether it asked about an asset or applied a rig that names one.
  namchain::graph::Document missing = document;
  missing.chains.front().graph.nodes[1].strings["model"] = "tweed/delux";
  error.clear();
  CHECK_FALSE(namchain::assets::resolve_document(missing, registry, "", error));
  CHECK(error.find("tweed/deluxe") != std::string::npos);
  CHECK(error.find("'amp'") != std::string::npos); // and which node asked for it
}

TEST_CASE("a WAV header is read rather than guessed at")
{
  Library library;
  double sample_rate = 0.0;
  unsigned channels = 0;
  std::uint64_t frames = 0;

  REQUIRE(namchain::assets::probe_wav((library.irs / "stereo.wav").string(), sample_rate, channels, frames));
  CHECK(sample_rate == doctest::Approx(44100.0));
  CHECK(channels == 2u);
  CHECK(frames == 128u);

  // Not a RIFF file at all, and a file that does not exist: both are "no answer", not a wrong one.
  CHECK_FALSE(namchain::assets::probe_wav((library.models / "clean.nam").string(), sample_rate, channels, frames));
  CHECK_FALSE(namchain::assets::probe_wav((library.irs / "nothing.wav").string(), sample_rate, channels, frames));
}

TEST_CASE("a .nam's fields are found at either end of the file")
{
  Library library;
  std::string architecture;
  double sample_rate = 0.0;

  // The canonical layout: the rate sits after the weights, past everything the sniff reads at the
  // front. This is the case that makes reading the tail worth doing.
  namchain::assets::sniff_nam((library.models / "tweed/deluxe.nam").string(), architecture, sample_rate);
  CHECK(architecture == "WaveNet");
  CHECK(sample_rate == doctest::Approx(48000.0));

  // A file that declares no rate says so rather than borrowing one.
  const auto bare = library.dir.path / "bare.nam";
  write_text(bare, R"({"version": "0.5.4", "architecture": "Linear", "config": {}, "weights": [1.0]})");
  architecture.clear();
  namchain::assets::sniff_nam(bare.string(), architecture, sample_rate);
  CHECK(architecture == "Linear");
  CHECK(sample_rate == doctest::Approx(0.0));

  // Not a .nam at all: no fields, no crash.
  architecture.clear();
  namchain::assets::sniff_nam((library.irs / "cab-b.wav").string(), architecture, sample_rate);
  CHECK(architecture.empty());
  CHECK(sample_rate == doctest::Approx(0.0));
}
