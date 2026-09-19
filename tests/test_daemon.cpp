// Tests for namchaind: configuration and rig resolution, the JSON protocol, and the control loop
// end to end against the paced `null` backend.
//
// The end-to-end cases matter more than they look: they are the only place the render thread, the
// plan publication ring, the reclaimer and the socket server run together. A unit test of the
// request handler alone would not catch a plan that is published but never adopted, or a device
// state that never leaves "closed".
#include <doctest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>

#include "namchain/daemon/config.h"
#include "namchain/daemon/daemon.h"
#include "namchain/wav.h"

namespace
{

using namchain::BackendKind;
using namchain::daemon::Config;
using namchain::daemon::Daemon;

/// A unique directory under the system temp dir, removed on destruction. Unique per process and per
/// instance so the test binary can run twice at once (CI does).
struct TempDir
{
  TempDir()
  {
    static std::atomic<unsigned> counter{0};
    path = std::filesystem::temp_directory_path() /
           ("namchaind-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)));
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

void write_file(const std::filesystem::path& path, const std::string& contents)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  REQUIRE(file.good());
  file << contents;
}

/// A rig with no model, so the test needs no generated NAM fixtures: in -> gain -> out.
const char* kGainRig = R"({
  "nodes": [
    { "id": "in", "type": "nc.input" },
    { "id": "g", "type": "nc.gain", "gain": 0.5 },
    { "id": "out", "type": "nc.output" }
  ],
  "connections": [
    { "from": "in", "to": "g" },
    { "from": "g", "to": "out" }
  ]
})";

/// A structurally different chain (one more node), for the transition test.
const char* kSecondRig = R"({
  "nodes": [
    { "id": "in", "type": "nc.input" },
    { "id": "g", "type": "nc.gain", "gain": 0.5 },
    { "id": "g2", "type": "nc.gain", "gain": 0.5 },
    { "id": "out", "type": "nc.output" }
  ],
  "connections": [
    { "from": "in", "to": "g" },
    { "from": "g", "to": "g2" },
    { "from": "g2", "to": "out" }
  ]
})";

/// One SEQPACKET request, one response. The framing the daemon does not have to implement.
std::string call(const std::string& socket_path, const std::string& request)
{
  const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
  REQUIRE(fd >= 0);

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  REQUIRE(socket_path.size() < sizeof(address.sun_path));
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  REQUIRE(::send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));

  std::vector<char> buffer(1 << 16);
  const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
  ::close(fd);
  REQUIRE(got > 0);
  return std::string(buffer.data(), static_cast<std::size_t>(got));
}

/// Poll the status until \p predicate holds, so the tests do not race the render thread's startup.
bool wait_for(const std::string& socket_path, const std::function<bool(const nlohmann::json&)>& predicate)
{
  for (int attempt = 0; attempt < 200; ++attempt)
  {
    const auto reply = nlohmann::json::parse(call(socket_path, R"({"id":"p","method":"engine.status"})"));
    if (reply["ok"] == true && predicate(reply["status"]))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

TEST_CASE("a config file sets the fields the daemon uses")
{
  TempDir dir;
  const auto path = dir.path / "namchaind.json";
  write_file(path, R"({
    "backend": "null",
    "device": "hw:9,0",
    "sample_rate": 96000,
    "period_size": 128,
    "periods": 6,
    "capture_channels": 2,
    "playback_channels": 2,
    "rigs_root": "/srv/rigs",
    "socket": "/tmp/example.sock",
    "socket_mode": "0600",
    "rt_priority": 70,
    "cpu": 3,
    "lock_memory": false,
    "output_queue_periods": 4,
    "output_queue_floor_periods": 2,
    "fade_ms": 2.5,
    "device_retry_ms": 500,
    "verbose": true
  })");

  Config config;
  std::string error;
  REQUIRE_MESSAGE(namchain::daemon::load_config_file(path.string(), config, error), error);

  CHECK(config.backend == BackendKind::Null);
  CHECK(config.device == "hw:9,0");
  CHECK(config.sample_rate == 96000u);
  CHECK(config.period_size == 128u);
  CHECK(config.periods == 6u);
  CHECK(config.capture_channels == 2u);
  CHECK(config.playback_channels == 2u);
  CHECK(config.rigs_root == "/srv/rigs");
  CHECK(config.socket_path == "/tmp/example.sock");
  CHECK(config.socket_mode == 0600u);
  CHECK(config.rt_priority == 70);
  CHECK(config.cpu == 3);
  CHECK_FALSE(config.lock_memory);
  CHECK(config.output_queue_periods == 4u);
  CHECK(config.output_queue_floor_periods == 2u);
  CHECK(config.fade_ms == doctest::Approx(2.5));
  CHECK(config.device_retry_ms == 500u);
  CHECK(config.verbose);
}

TEST_CASE("a config file names what it does not understand")
{
  TempDir dir;

  SUBCASE("unknown key")
  {
    const auto path = dir.path / "unknown.json";
    write_file(path, R"({"period_sizes": 64})");
    Config config;
    std::string error;
    CHECK_FALSE(namchain::daemon::load_config_file(path.string(), config, error));
    CHECK(error.find("period_sizes") != std::string::npos);
  }

  SUBCASE("wrong type")
  {
    const auto path = dir.path / "wrong.json";
    write_file(path, R"({"sample_rate": "48000"})");
    Config config;
    std::string error;
    CHECK_FALSE(namchain::daemon::load_config_file(path.string(), config, error));
    CHECK(error.find("whole number") != std::string::npos);
  }

  SUBCASE("bad socket mode")
  {
    const auto path = dir.path / "mode.json";
    write_file(path, R"({"socket_mode": "08"})");
    Config config;
    std::string error;
    CHECK_FALSE(namchain::daemon::load_config_file(path.string(), config, error));
    CHECK(error.find("octal") != std::string::npos);
  }

  SUBCASE("unknown backend")
  {
    const auto path = dir.path / "backend.json";
    write_file(path, R"({"backend": "jack"})");
    Config config;
    std::string error;
    CHECK_FALSE(namchain::daemon::load_config_file(path.string(), config, error));
    CHECK(error.find("backend") != std::string::npos);
  }
}

TEST_CASE("command-line options parse and reject")
{
  Config config;
  std::string error;

  CHECK(namchain::daemon::apply_option(config, "backend", "null", error));
  CHECK(config.backend == BackendKind::Null);
  CHECK(namchain::daemon::apply_option(config, "socket-mode", "0660", error));
  CHECK(config.socket_mode == 0660u);
  CHECK(namchain::daemon::apply_option(config, "period-size", "32", error));
  CHECK(config.period_size == 32u);
  CHECK(namchain::daemon::apply_option(config, "cpu", "-1", error));
  CHECK(config.cpu == -1);
  CHECK(namchain::daemon::apply_option(config, "no-lock-memory", "", error));
  CHECK_FALSE(config.lock_memory);

  CHECK_FALSE(namchain::daemon::apply_option(config, "nonsense", "1", error));
  CHECK(error.find("nonsense") != std::string::npos);
  CHECK_FALSE(namchain::daemon::apply_option(config, "period-size", "-4", error));
  CHECK_FALSE(namchain::daemon::apply_option(config, "socket-mode", "8", error));
  CHECK_FALSE(namchain::daemon::apply_option(config, "rt-priority", "loud", error));
}

TEST_CASE("a protocol rig name cannot escape the rigs root")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "ok.json", kGainRig);
  write_file(dir.path / "secret.json", "{}");

  Config config;
  config.rigs_root = root.string();
  std::string path;
  std::string error;

  CHECK(namchain::daemon::resolve_rig_name(config, "ok.json", path, error));
  CHECK(path == std::filesystem::canonical(root / "ok.json").string());

  CHECK_FALSE(namchain::daemon::resolve_rig_name(config, "../secret.json", path, error));
  CHECK(error.find("outside the rigs root") != std::string::npos);
  CHECK_FALSE(namchain::daemon::resolve_rig_name(config, "/etc/passwd", path, error));
  CHECK(error.find("relative") != std::string::npos);
  CHECK_FALSE(namchain::daemon::resolve_rig_name(config, "missing.json", path, error));
}

// ---------------------------------------------------------------------------
// Protocol (no daemon started: the handler is the unit under test)
// ---------------------------------------------------------------------------

TEST_CASE("the protocol version negotiates, rejects and names unknown methods")
{
  TempDir dir;
  Config config;
  config.rigs_root = (dir.path / "rigs").string();
  Daemon daemon(config);

  {
    const auto reply = nlohmann::json::parse(
      daemon.handle_request(R"({"id":"1","method":"engine.hello","protocol":1})"));
    CHECK(reply["ok"] == true);
    CHECK(reply["id"] == "1");
    CHECK(reply["protocol"] == 1);
    CHECK(reply["role"] == "namchaind");
  }
  {
    const auto reply = nlohmann::json::parse(
      daemon.handle_request(R"({"id":"2","method":"engine.hello","protocol":99})"));
    CHECK(reply["ok"] == false);
    CHECK(reply["error"]["code"] == "unsupported_protocol");
  }
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request(R"({"id":"3","method":"engine.nope"})"));
    CHECK(reply["ok"] == false);
    CHECK(reply["error"]["code"] == "unknown_method");
  }
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request("{"));
    CHECK(reply["ok"] == false);
    CHECK(reply["error"]["code"] == "bad_json");
  }
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request(R"({"method":"engine.status"})"));
    CHECK(reply["ok"] == true);
    CHECK(reply["status"]["role"] == "namchaind");
    // The honest labels: parameter edits are ramped, structural reloads are crossfaded.
    CHECK(reply["status"]["param_edits"] == "ramped");
    CHECK(reply["status"]["structural_transitions"] == "crossfade");
  }
}

TEST_CASE("mute and panic are settable and reported")
{
  Config config;
  Daemon daemon(config);

  {
    const auto reply = nlohmann::json::parse(
      daemon.handle_request(R"({"id":"m","method":"engine.mute","mute":true})"));
    CHECK(reply["ok"] == true);
    CHECK(reply["mute"] == true);
  }
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request(R"({"method":"engine.status"})"));
    CHECK(reply["status"]["mute"] == true);
  }
  {
    const auto reply = nlohmann::json::parse(
      daemon.handle_request(R"({"id":"u","method":"engine.mute","mute":false})"));
    CHECK(reply["mute"] == false);
  }
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request(R"({"method":"engine.panic"})"));
    CHECK(reply["ok"] == true);
    CHECK(reply["panic"] == true);
  }
  {
    const auto reply = nlohmann::json::parse(
      daemon.handle_request(R"({"method":"engine.mute","mute":"loud"})"));
    CHECK(reply["ok"] == false);
    CHECK(reply["error"]["code"] == "bad_request");
  }
}

TEST_CASE("rig.load compiles a named rig and rig.get reports it")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "gain.json", kGainRig);

  Config config;
  config.rigs_root = root.string();
  Daemon daemon(config);

  {
    const auto reply = nlohmann::json::parse(
      daemon.handle_request(R"({"id":"l","method":"rig.load","rig":"gain.json"})"));
    REQUIRE(reply["ok"] == true);
    CHECK(reply["generation"] == 1);
    CHECK(reply["rig"] == std::filesystem::canonical(root / "gain.json").string());
  }
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request(R"({"method":"rig.get"})"));
    CHECK(reply["ok"] == true);
    CHECK(reply["generation"] == 1);
    CHECK(reply["loaded"] == true);
  }
  {
    const auto reply = nlohmann::json::parse(
      daemon.handle_request(R"({"id":"x","method":"rig.load","rig":"../secret.json"})"));
    CHECK(reply["ok"] == false);
    CHECK(reply["error"]["code"] == "rig_not_found");
  }
}

TEST_CASE("rig.patch pushes a ramped set_param and refuses what it cannot apply")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "gain.json", kGainRig);

  Config config;
  config.rigs_root = root.string();
  Daemon daemon(config);

  SUBCASE("without a plan it is rejected, not ignored")
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request(
      R"({"method":"rig.patch","ops":[{"op":"set_param","node":"g","param":"gain","value":0.7}]})"));
    CHECK(reply["ok"] == false);
    CHECK(reply["error"]["code"] == "patch_rejected");
  }

  REQUIRE(nlohmann::json::parse(daemon.handle_request(R"({"method":"rig.load","rig":"gain.json"})"))["ok"] ==
          true);

  SUBCASE("a settable param takes the smooth path and does not recompile")
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request(
      R"({"method":"rig.patch","ops":[{"op":"set_param","node":"g","param":"gain","value":0.7}]})"));
    REQUIRE(reply["ok"] == true);
    CHECK(reply["kind"] == "smooth");
    CHECK(reply["applied"] == 1);
    CHECK(reply["generation"] == 1); // no new plan was built
  }

  SUBCASE("a param the node cannot ramp takes the structural path instead")
  {
    // `gain_db` is a *build* parameter: `nc.gain` only ramps `gain`, so the only way to honour this
    // is to rebuild the node. The router is what decides, and the response says which path it took.
    const auto reply = nlohmann::json::parse(daemon.handle_request(
      R"({"method":"rig.patch","ops":[{"op":"set_param","node":"g","param":"gain_db","value":3.0}]})"));
    REQUIRE(reply["ok"] == true);
    CHECK(reply["kind"] == "structural");
    CHECK(reply["generation"] == 2);
  }

  SUBCASE("an unknown op is rejected rather than ignored")
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request(
      R"({"method":"rig.patch","ops":[{"op":"explode","node":"g"}]})"));
    CHECK(reply["ok"] == false);
    CHECK(reply["error"]["code"] == "patch_rejected");
    CHECK(reply["error"]["message"].get<std::string>().find("unknown op") != std::string::npos);
  }

  SUBCASE("add_node and rewire take the structural path and build a new plan")
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request(R"({"method":"rig.patch","ops":[
      {"op":"add_node","id":"g2","type":"nc.gain","gain":0.5},
      {"op":"connect","from":"g","to":"g2"},
      {"op":"rewire","from":"g2","to":"out"}
    ]})"));
    REQUIRE(reply["ok"] == true);
    CHECK(reply["kind"] == "structural");
    CHECK(reply["applied"] == 3);
    CHECK(reply["generation"] == 2);
  }

  SUBCASE("a structural op that would leave the rig uncompilable is refused, generation unchanged")
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request(
      R"({"method":"rig.patch","ops":[{"op":"remove_node","node":"out"}]})"));
    CHECK(reply["ok"] == false);
    CHECK(reply["error"]["code"] == "compile_failed");
    // Nothing was published, so the running generation is still the loaded rig.
    const auto status = nlohmann::json::parse(daemon.handle_request(R"({"method":"engine.status"})"));
    CHECK(status["status"]["generation"] == 1);
  }

  SUBCASE("bypass takes the structural path, because the plan changes")
  {
    const auto reply = nlohmann::json::parse(
      daemon.handle_request(R"({"method":"rig.patch","ops":[{"op":"bypass","node":"g"}]})"));
    REQUIRE(reply["ok"] == true);
    CHECK(reply["kind"] == "structural");
    CHECK(reply["generation"] == 2);
  }
}

// ---------------------------------------------------------------------------
// End to end: render thread, publication and socket together
// ---------------------------------------------------------------------------
TEST_CASE("the daemon runs the null backend and serves the control socket")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "gain.json", kGainRig);

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.rig = (root / "gain.json").string();
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;
  // The test process is not the appliance: do not mlock all of it or ask for SCHED_FIFO. Those are
  // exercised where they belong (the soak and the unit's rlimits), not here.
  config.lock_memory = false;
  config.rt_priority = 0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);
  std::thread loop([&daemon] { daemon.run(); });
  // Joins on every exit path, including a failed REQUIRE below.
  struct Guard
  {
    Daemon& daemon;
    std::thread& thread;
    ~Guard()
    {
      daemon.request_stop();
      if (thread.joinable())
        thread.join();
    }
  } guard{daemon, loop};

  const auto hello = nlohmann::json::parse(call(config.socket_path, R"({"id":"h","method":"engine.hello"})"));
  CHECK(hello["ok"] == true);

  // The device comes up and the startup plan is adopted without a rig.load: both facts are polled,
  // because the render thread starts concurrently with this thread.
  const bool running = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["adopted_generation"].get<uint64_t>() == 1 &&
           status["blocks"].get<uint64_t>() > 0;
  });
  CHECK_MESSAGE(running, "the render thread never reached running with the startup plan");

  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(status["status"]["rig"] == std::filesystem::canonical(root / "gain.json").string());
  CHECK(status["status"]["sample_rate"] == 48000);
  CHECK(status["status"]["period_size"] == 64);
  // A published plan is not adopted by magic: the generation the render thread runs tracks the one
  // the control thread assigned.
  CHECK(status["status"]["generation"] == status["status"]["adopted_generation"]);

  // The `smooth` op class, end to end: the control thread resolves the vtable dispatch, the render
  // thread drains it and applies it, and the status counter proves it happened rather than assuming.
  const auto patch = nlohmann::json::parse(call(
    config.socket_path,
    R"({"id":"p","method":"rig.patch","ops":[{"op":"set_param","node":"g","param":"gain","value":0.25}]})"));
  REQUIRE(patch["ok"] == true);
  CHECK(patch["applied"] == 1);
  const bool applied = wait_for(config.socket_path, [](const nlohmann::json& s) {
    return s["params_applied"].get<uint64_t>() == 1;
  });
  CHECK_MESSAGE(applied, "the render thread never applied the parameter edit");
}

TEST_CASE("a structural rig.load is dual-rendered and crossfaded")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "a.json", kGainRig);
  write_file(root / "b.json", kSecondRig);

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.rig = (root / "a.json").string();
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;
  config.lock_memory = false;
  config.rt_priority = 0;
  config.fade_ms = 5.0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);
  std::thread loop([&daemon] { daemon.run(); });
  struct Guard
  {
    Daemon& daemon;
    std::thread& thread;
    ~Guard()
    {
      daemon.request_stop();
      if (thread.joinable())
        thread.join();
    }
  } guard{daemon, loop};

  REQUIRE(wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["audible_generation"].get<uint64_t>() == 1;
  }));

  const auto loaded = nlohmann::json::parse(
    call(config.socket_path, R"({"id":"l","method":"rig.load","rig":"b.json"})"));
  REQUIRE(loaded["ok"] == true);
  CHECK(loaded["generation"] == 2);

  // The fade is ~5 ms, so the end state is the robust thing to poll for; `transitions_started`
  // distinguishes a crossfade from an immediate swap, which is the whole point.
  const bool settled = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["transition_active"] == false && status["audible_generation"].get<uint64_t>() == 2;
  });
  CHECK_MESSAGE(settled, "the render thread never finished the structural transition");

  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(status["status"]["structural_transitions"] == "crossfade");
  CHECK(status["status"]["transitions_started"].get<uint64_t>() >= 1);
  CHECK(status["status"]["target_generation"] == 2);
  CHECK(status["status"]["generation"] == 2);
}

TEST_CASE("a structural rig.patch rebuilds the running rig and crossfades into it")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "gain.json", kGainRig);

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.rig = (root / "gain.json").string();
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;
  config.lock_memory = false;
  config.rt_priority = 0;
  config.fade_ms = 5.0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);
  std::thread loop([&daemon] { daemon.run(); });
  struct Guard
  {
    Daemon& daemon;
    std::thread& thread;
    ~Guard()
    {
      daemon.request_stop();
      if (thread.joinable())
        thread.join();
    }
  } guard{daemon, loop};

  REQUIRE(wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["audible_generation"].get<uint64_t>() == 1;
  }));

  const auto patch = nlohmann::json::parse(call(config.socket_path, R"({"id":"p","method":"rig.patch","ops":[
    {"op":"add_node","id":"g2","type":"nc.gain","gain":0.5},
    {"op":"connect","from":"g","to":"g2"},
    {"op":"rewire","from":"g2","to":"out"}
  ]})"));
  REQUIRE(patch["ok"] == true);
  CHECK(patch["kind"] == "structural");
  CHECK(patch["generation"] == 2);

  const bool settled = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["transition_active"] == false && status["audible_generation"].get<uint64_t>() == 2;
  });
  CHECK_MESSAGE(settled, "the render thread never finished the patch's transition");

  // The rebuilt plan is the one now live: a param on the *added* node resolves, and takes the smooth
  // path (so the patch protocol and the plan agree about the graph).
  const auto smooth = nlohmann::json::parse(call(
    config.socket_path,
    R"({"id":"s","method":"rig.patch","ops":[{"op":"set_param","node":"g2","param":"gain","value":0.25}]})"));
  REQUIRE(smooth["ok"] == true);
  CHECK(smooth["kind"] == "smooth");
  CHECK(smooth["generation"] == 2);

  // Bypass is structural: the plan changes, so the render thread crossfades into the new one.
  const auto bypass = nlohmann::json::parse(call(
    config.socket_path, R"({"id":"b","method":"rig.patch","ops":[{"op":"bypass","node":"g2"}]})"));
  REQUIRE(bypass["ok"] == true);
  CHECK(bypass["kind"] == "structural");
  CHECK(bypass["generation"] == 3);

  const bool bypassed = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["transition_active"] == false && status["audible_generation"].get<uint64_t>() == 3;
  });
  CHECK_MESSAGE(bypassed, "the render thread never finished the bypass transition");

  // The guard reports through status, and a well-behaved rig reports zero.
  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(status["status"]["failed_nodes"] == 0);
  CHECK(status["status"]["guard_trips"] == 0);

  // Un-bypass restores the node: another structural change, another generation.
  const auto restored = nlohmann::json::parse(call(
    config.socket_path,
    R"({"id":"r","method":"rig.patch","ops":[{"op":"bypass","node":"g2","bypass":false}]})"));
  REQUIRE(restored["ok"] == true);
  CHECK(restored["generation"] == 4);
}

TEST_CASE("the daemon stays up and reports lost when the device cannot be opened")
{
  TempDir dir;
  Config config;
  // `file` with no input path cannot open. The daemon must not exit; it must report and retry.
  config.backend = BackendKind::File;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = (dir.path / "rigs").string();
  config.device_retry_ms = 10;
  config.reclamation_timeout_ms = 5;
  config.lock_memory = false;
  config.rt_priority = 0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);
  std::thread loop([&daemon] { daemon.run(); });
  struct Guard
  {
    Daemon& daemon;
    std::thread& thread;
    ~Guard()
    {
      daemon.request_stop();
      if (thread.joinable())
        thread.join();
    }
  } guard{daemon, loop};

  const bool lost = wait_for(config.socket_path,
                             [](const nlohmann::json& status) { return status["state"] == "lost"; });
  CHECK_MESSAGE(lost, "a device that cannot open must leave the daemon in the lost state");

  // Still serving, and it has not claimed to have rendered anything.
  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(status["ok"] == true);
  CHECK(status["status"]["blocks"] == 0);
  CHECK(daemon.device_state() == namchain::daemon::DeviceState::Lost);
}

// ---------------------------------------------------------------------------
// More than one chain (docs/ui-integration-plan.md §1.1, slice A2)
// ---------------------------------------------------------------------------

namespace
{

/// A two-chain document: guitar on capture channel 0, vocal on capture channel 1, each an
/// `nc.input -> nc.gain -> nc.output` chain at the given gain, writing the given playback channels.
///
/// Built with nlohmann rather than a string literal so a test can vary one thing at a time and the
/// difference between two documents is visible in the call.
std::string two_chain_document(double guitar_gain, double vocal_gain, int guitar_out, int vocal_out)
{
  const auto chain = [](const char* id, int in_channel, int out_channel, double gain) {
    nlohmann::json c;
    c["id"] = id;
    c["input_channel"] = in_channel;
    c["output_channel"] = out_channel;
    c["nodes"] = nlohmann::json::array({
      {{"id", "in"}, {"type", "nc.input"}},
      {{"id", "g"}, {"type", "nc.gain"}, {"gain", gain}},
      {{"id", "out"}, {"type", "nc.output"}},
    });
    c["connections"] = nlohmann::json::array({
      {{"from", "in"}, {"to", "g"}},
      {{"from", "g"}, {"to", "out"}},
    });
    return c;
  };

  nlohmann::json document;
  document["name"] = "Two chains";
  document["chains"] = nlohmann::json::array(
    {chain("gtr", 0, guitar_out, guitar_gain), chain("voc", 1, vocal_out, vocal_gain)});
  return document.dump();
}

/// A stereo WAV of constant values, so a channel's destination is obvious in the output.
void write_stereo_wav(const std::filesystem::path& path, unsigned frames, float left, float right)
{
  std::vector<float> interleaved;
  interleaved.reserve(static_cast<std::size_t>(frames) * 2);
  for (unsigned i = 0; i < frames; ++i)
  {
    interleaved.push_back(left);
    interleaved.push_back(right);
  }
  std::string error;
  REQUIRE_MESSAGE(namchain::save_wav_float32(path.string(), interleaved.data(), frames, 2, 48000.0, error), error);
}

std::vector<float> read_wav(const std::filesystem::path& path, unsigned& channels)
{
  std::vector<float> interleaved;
  double rate = 0.0;
  std::string error;
  REQUIRE_MESSAGE(namchain::load_wav(path.string(), interleaved, channels, rate, error), error);
  return interleaved;
}

/// Run a `file` backend daemon to the end of its input and wait for the render thread to finish, which
/// is when the output WAV has been written. No control loop is needed: `handle_request` is the control
/// thread's work and no other thread is doing it.
///
/// "Closed" alone is not enough to wait for: it is also the state *before* the device is opened, and a
/// loaded machine makes that window wide enough to observe. A block count is what distinguishes a
/// device that has been and gone from one that has not arrived yet.
void run_to_end(Daemon& daemon)
{
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);
  for (int attempt = 0; attempt < 4000; ++attempt)
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request(R"({"method":"engine.status"})"));
    if (reply["status"]["state"] == "closed" && reply["status"]["blocks"].get<uint64_t>() > 0)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  FAIL("the file backend never ran out of input");
}

Config file_backend_config(const TempDir& dir, unsigned capture_channels, unsigned playback_channels)
{
  Config config;
  config.backend = BackendKind::File;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.capture_path = (dir.path / "in.wav").string();
  config.playback_path = (dir.path / "out.wav").string();
  config.capture_channels = capture_channels;
  config.playback_channels = playback_channels;
  config.sample_rate = 48000;
  config.period_size = 64;
  config.periods = 4;
  config.lock_memory = false;
  config.rt_priority = 0;
  return config;
}

} // namespace

TEST_CASE("a document runs its chains on the channels it names")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "two.json", two_chain_document(0.5, 0.25, /*guitar_out=*/0, /*vocal_out=*/1));
  write_stereo_wav(dir.path / "in.wav", 128, /*left=*/1.0f, /*right=*/0.5f);

  Config config = file_backend_config(dir, 2, 2);
  config.rigs_root = root.string();
  config.rig = (root / "two.json").string();

  Daemon daemon(config);
  run_to_end(daemon);

  unsigned channels = 0;
  const std::vector<float> output = read_wav(dir.path / "out.wav", channels);
  REQUIRE(channels == 2u);
  REQUIRE(output.size() >= 128u * 2u);

  // Guitar: channel 0 in at 1.0 through a gain of 0.5, out on channel 0. Vocal: channel 1 in at 0.5
  // through 0.25, out on channel 1. Each chain read its own input and wrote its own output.
  for (unsigned i = 0; i < 128; ++i)
  {
    CHECK(output[i * 2 + 0] == doctest::Approx(0.5f));
    CHECK(output[i * 2 + 1] == doctest::Approx(0.125f));
  }

  const auto status = nlohmann::json::parse(daemon.handle_request(R"({"method":"engine.status"})"));
  REQUIRE(status["status"]["chains"].size() == 2u);
  CHECK(status["status"]["chains"][0]["id"] == "gtr");
  CHECK(status["status"]["chains"][0]["input_channel"] == 0);
  CHECK(status["status"]["chains"][0]["output_channel"] == 0);
  CHECK(status["status"]["chains"][1]["id"] == "voc");
  CHECK(status["status"]["chains"][1]["input_channel"] == 1);
  CHECK(status["status"]["chains"][1]["output_channel"] == 1);
  CHECK(status["status"]["max_chains"] == namchain::daemon::kMaxChains);
}

TEST_CASE("two chains writing one channel are summed")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "two.json", two_chain_document(0.5, 0.25, /*guitar_out=*/0, /*vocal_out=*/0));
  write_stereo_wav(dir.path / "in.wav", 128, /*left=*/1.0f, /*right=*/0.5f);

  Config config = file_backend_config(dir, 2, 2);
  config.rigs_root = root.string();
  config.rig = (root / "two.json").string();

  Daemon daemon(config);
  run_to_end(daemon);

  unsigned channels = 0;
  const std::vector<float> output = read_wav(dir.path / "out.wav", channels);
  REQUIRE(channels == 2u);

  // 1.0 * 0.5 + 0.5 * 0.25 = 0.625 on channel 0, and nothing on channel 1: a shared output channel is
  // a sum, not a race, and the channel nobody writes stays silent.
  CHECK(output[0] == doctest::Approx(0.625f));
  CHECK(output[1] == doctest::Approx(0.0f));
}

TEST_CASE("a chain that names a channel the engine does not have is refused")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "two.json", two_chain_document(0.5, 0.25, 0, 1));

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.capture_channels = 1; // the vocal chain wants channel 1
  config.playback_channels = 1;
  config.lock_memory = false;
  config.rt_priority = 0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);

  const auto reply = nlohmann::json::parse(
    daemon.handle_request(R"({"id":"l","method":"rig.load","rig":"two.json"})"));
  CHECK(reply["ok"] == false);
  CHECK(reply["error"]["code"] == "compile_failed");
  const std::string message = reply["error"]["message"];
  CHECK(message.find("chain 'voc'") != std::string::npos);
  CHECK(message.find("capture_channels") != std::string::npos);
}

TEST_CASE("a document apply transitions its chains one at a time")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "a.json", two_chain_document(0.5, 0.5, 0, 1));
  write_file(root / "b.json", two_chain_document(0.25, 0.25, 0, 1));

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.rig = (root / "a.json").string();
  config.capture_channels = 2;
  config.playback_channels = 2;
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;
  config.lock_memory = false;
  config.rt_priority = 0;
  // Long enough that the stagger is observable from another thread over a socket.
  config.fade_ms = 40.0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);
  std::thread loop([&daemon] { daemon.run(); });
  struct Guard
  {
    Daemon& daemon;
    std::thread& thread;
    ~Guard()
    {
      daemon.request_stop();
      if (thread.joinable())
        thread.join();
    }
  } guard{daemon, loop};

  REQUIRE(wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["audible_generation"].get<uint64_t>() == 1;
  }));

  const auto loaded =
    nlohmann::json::parse(call(config.socket_path, R"({"id":"l","method":"rig.load","rig":"b.json"})"));
  REQUIRE(loaded["ok"] == true);
  REQUIRE(loaded["generation"] == 2);

  // Sample the status while the apply works through the chains. The evidence is the per-chain
  // *generations* rather than the transition flags: a status response is built on the control thread
  // while the render thread mutates those flags, so a snapshot can straddle the moment one chain's
  // fade ends and the next begins. Generations only ever move forward, and only in this order --
  // chain 0 reaching generation 2 while chain 1 is still on 1 is the stagger itself, and it lasts for
  // a whole fade rather than an instant.
  bool saw_staggered = false;
  bool saw_second_before_first = false;
  for (int attempt = 0; attempt < 400; ++attempt)
  {
    const auto reply = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
    const auto& status = reply["status"];
    const uint64_t first = status["chains"][0]["audible_generation"].get<uint64_t>();
    const uint64_t second = status["chains"][1]["audible_generation"].get<uint64_t>();
    saw_staggered = saw_staggered || (first == 2 && second == 1);
    saw_second_before_first = saw_second_before_first || (first == 1 && second == 2);
    if (first == 2 && second == 2)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  CHECK_MESSAGE(saw_staggered, "the second chain reached the new document before the first had settled");
  CHECK_FALSE(saw_second_before_first); // chain 1 must never overtake chain 0

  const bool settled = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["transition_active"] == false && status["audible_generation"].get<uint64_t>() == 2 &&
           status["chains"][0]["audible_generation"].get<uint64_t>() == 2 &&
           status["chains"][1]["audible_generation"].get<uint64_t>() == 2;
  });
  CHECK_MESSAGE(settled, "the document never finished being applied");

  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(status["status"]["transitions_started"].get<uint64_t>() == 2);
  CHECK(status["status"]["generation"] == 2);
  CHECK(status["status"]["adopted_generation"] == 2);
  // The rule the stagger exists for, as a number rather than a promise: two chains changed, and never
  // two crossfades at once.
  CHECK(status["status"]["max_concurrent_transitions"] == 1);
}

TEST_CASE("a patch to one chain leaves the other chain's plan alone")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "two.json", two_chain_document(0.5, 0.25, 0, 1));

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.rig = (root / "two.json").string();
  config.capture_channels = 2;
  config.playback_channels = 2;
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;
  config.lock_memory = false;
  config.rt_priority = 0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);
  std::thread loop([&daemon] { daemon.run(); });
  struct Guard
  {
    Daemon& daemon;
    std::thread& thread;
    ~Guard()
    {
      daemon.request_stop();
      if (thread.joinable())
        thread.join();
    }
  } guard{daemon, loop};

  REQUIRE(wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["audible_generation"].get<uint64_t>() == 1;
  }));

  // Smooth, on one chain, by chain id.
  const auto smooth = nlohmann::json::parse(call(
    config.socket_path,
    R"({"id":"p","method":"rig.patch","chain":"gtr","ops":[{"op":"set_param","node":"g","param":"gain","value":0.75}]})"));
  REQUIRE(smooth["ok"] == true);
  CHECK(smooth["kind"] == "smooth");
  CHECK(smooth["chain"] == "gtr");
  const bool applied = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["params_applied"].get<uint64_t>() == 1;
  });
  CHECK_MESSAGE(applied, "the render thread never applied the parameter edit");

  // Structural, on the same chain: a new node means a new plan for it and nothing for the other.
  const auto structural = nlohmann::json::parse(call(
    config.socket_path,
    R"({"id":"q","method":"rig.patch","chain":"gtr","ops":[{"op":"add_node","id":"g2","type":"nc.gain","gain":0.5},
       {"op":"connect","from":"g","to":"g2"},{"op":"rewire","from":"g2","to":"out"}]})"));
  REQUIRE(structural["ok"] == true);
  CHECK(structural["kind"] == "structural");
  REQUIRE(structural["generation"] == 2);

  const bool settled = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["chains"][0]["audible_generation"].get<uint64_t>() == 2 && status["transition_active"] == false;
  });
  CHECK_MESSAGE(settled, "the patched chain never finished its transition");

  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(status["status"]["chains"][0]["generation"] == 2);
  CHECK(status["status"]["chains"][1]["generation"] == 1); // untouched: not replanned, not faded
  CHECK(status["status"]["chains"][1]["audible_generation"] == 1);
  // One transition for the whole apply: the untouched chain was never even queued behind it.
  CHECK(status["status"]["transitions_started"].get<uint64_t>() == 1);

  // A patch with no chain, on a rig with several chains, is refused rather than guessing.
  const auto ambiguous = nlohmann::json::parse(call(
    config.socket_path,
    R"({"id":"r","method":"rig.patch","ops":[{"op":"set_param","node":"g","param":"gain","value":0.5}]})"));
  CHECK(ambiguous["ok"] == false);
  CHECK(ambiguous["error"]["code"] == "unknown_chain");
}

TEST_CASE("per-chain mute is resolved, reported, and refused for a chain that does not exist")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "two.json", two_chain_document(0.5, 0.25, 0, 1));

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.rig = (root / "two.json").string();
  config.capture_channels = 2;
  config.playback_channels = 2;
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;
  config.lock_memory = false;
  config.rt_priority = 0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);
  std::thread loop([&daemon] { daemon.run(); });
  struct Guard
  {
    Daemon& daemon;
    std::thread& thread;
    ~Guard()
    {
      daemon.request_stop();
      if (thread.joinable())
        thread.join();
    }
  } guard{daemon, loop};

  REQUIRE(wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["audible_generation"].get<uint64_t>() == 1;
  }));

  const auto muted = nlohmann::json::parse(
    call(config.socket_path, R"({"id":"m","method":"engine.mute","chain":"voc","mute":true})"));
  CHECK(muted["ok"] == true);
  CHECK(muted["chain"] == "voc");

  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(status["status"]["chains"][0]["muted"] == false);
  CHECK(status["status"]["chains"][1]["muted"] == true);
  // The global mute is a different thing and stays off: muting one chain is not muting the engine.
  CHECK(status["status"]["mute"] == false);

  const auto unknown = nlohmann::json::parse(
    call(config.socket_path, R"({"id":"m2","method":"engine.mute","chain":"bass","mute":true})"));
  CHECK(unknown["ok"] == false);
  CHECK(unknown["error"]["code"] == "unknown_chain");

  const auto unmuted = nlohmann::json::parse(
    call(config.socket_path, R"({"id":"m3","method":"engine.mute","chain":"voc","mute":false})"));
  CHECK(unmuted["ok"] == true);
  const auto after = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(after["status"]["chains"][1]["muted"] == false);
}

TEST_CASE("a document that changes nothing does not advance the generation")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "a.json", kGainRig);

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.rig = (root / "a.json").string();
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;
  config.lock_memory = false;
  config.rt_priority = 0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);
  std::thread loop([&daemon] { daemon.run(); });
  struct Guard
  {
    Daemon& daemon;
    std::thread& thread;
    ~Guard()
    {
      daemon.request_stop();
      if (thread.joinable())
        thread.join();
    }
  } guard{daemon, loop};

  REQUIRE(wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["audible_generation"].get<uint64_t>() == 1;
  }));

  // Reloading the same rig is not an edit: nothing is recompiled, nothing fades, and a client polling
  // for adoption does not see a generation that will never arrive.
  const auto again =
    nlohmann::json::parse(call(config.socket_path, R"({"id":"l","method":"rig.load","rig":"a.json"})"));
  CHECK(again["ok"] == true);
  CHECK(again["generation"] == 1);

  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(status["status"]["transition_active"] == false);
  CHECK(status["status"]["transitions_started"].get<uint64_t>() == 0);

  // A real change still publishes.
  write_file(root / "a.json", kSecondRig);
  const auto changed =
    nlohmann::json::parse(call(config.socket_path, R"({"id":"l2","method":"rig.load","rig":"a.json"})"));
  CHECK(changed["ok"] == true);
  CHECK(changed["generation"] == 2);
}

TEST_CASE("a document with fewer chains drops the ones that are gone and keeps running")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "two.json", two_chain_document(0.5, 0.25, 0, 1));
  write_file(root / "one.json", kGainRig);

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.rig = (root / "two.json").string();
  config.capture_channels = 2;
  config.playback_channels = 2;
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;
  config.lock_memory = false;
  config.rt_priority = 0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);
  std::thread loop([&daemon] { daemon.run(); });
  struct Guard
  {
    Daemon& daemon;
    std::thread& thread;
    ~Guard()
    {
      daemon.request_stop();
      if (thread.joinable())
        thread.join();
    }
  } guard{daemon, loop};

  REQUIRE(wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["chains"].size() == 2u;
  }));

  const auto loaded =
    nlohmann::json::parse(call(config.socket_path, R"({"id":"l","method":"rig.load","rig":"one.json"})"));
  REQUIRE(loaded["ok"] == true);

  const bool dropped = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["chains"].size() == 1u && status["chains"][0]["id"] == "main";
  });
  CHECK_MESSAGE(dropped, "the chain that the new document does not have is still reported");

  // The engine is still rendering, with one chain, at the new generation.
  const bool running = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["audible_generation"].get<uint64_t>() == 2 &&
           status["transition_active"] == false;
  });
  CHECK_MESSAGE(running, "the engine did not settle after losing a chain");
}
