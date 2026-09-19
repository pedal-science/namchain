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
    // The honest label: there is no transition machinery yet.
    CHECK(reply["status"]["transitions"] == "none");
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
