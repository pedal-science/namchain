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
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>

#include "namchain/daemon/framing.h"
#include "namchain/daemon/platform.h"
#include "namchain/daemon/config.h"
#include "namchain/daemon/daemon.h"
#include "namchain/graph/macro.h"
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

/// \brief One request, one response, over the local socket.
///
/// The envelope follows the socket type: one datagram where `PF_LOCAL` has `SOCK_SEQPACKET`,
/// the same length prefix the TCP transport uses where it does not (see
/// namchain/daemon/platform.h). The daemon picks from the same constant, so a test that
/// passes here is testing the transport the daemon actually opened.
std::string local_exchange(const std::string& socket_path, const std::string& request)
{
  namespace platform = namchain::daemon::platform;

  const int fd = ::socket(AF_UNIX, platform::local_socket_type(), 0);
  REQUIRE(fd >= 0);

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  REQUIRE(socket_path.size() < sizeof(address.sun_path));
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

  // A reply that never comes should fail this test, not hang the whole binary with no clue which
  // case it was in. Generous: the daemon answers in microseconds when it answers at all.
  timeval timeout{};
  timeout.tv_sec = 5;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  std::vector<char> out;
  if (platform::local_transport_is_stream)
    namchain::daemon::append_frame(out, request);
  else
    out.assign(request.begin(), request.end());

  std::size_t sent = 0;
  while (sent < out.size())
  {
    const ssize_t wrote = ::send(fd, out.data() + sent, out.size() - sent, 0);
    if (wrote < 0 && errno == EINTR)
      continue;
    REQUIRE(wrote > 0);
    sent += static_cast<std::size_t>(wrote);
  }

  std::string body;
  if (platform::local_transport_is_stream)
  {
    // A stream does not preserve what the sender wrote, so read the header, then exactly the
    // length it names. Anything less is a truncated reply reported as a short one.
    char header[namchain::daemon::kFrameHeaderBytes] = {};
    std::size_t have = 0;
    while (have < sizeof(header))
    {
      const ssize_t got = ::recv(fd, header + have, sizeof(header) - have, 0);
      if (got < 0 && errno == EINTR)
        continue;
      REQUIRE(got > 0);
      have += static_cast<std::size_t>(got);
    }
    body.resize(namchain::daemon::frame_length(header));
    have = 0;
    while (have < body.size())
    {
      const ssize_t got = ::recv(fd, body.data() + have, body.size() - have, 0);
      if (got < 0 && errno == EINTR)
        continue;
      REQUIRE(got > 0);
      have += static_cast<std::size_t>(got);
    }
  }
  else
  {
    std::vector<char> buffer(1 << 16);
    const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
    REQUIRE(got > 0);
    body.assign(buffer.data(), static_cast<std::size_t>(got));
  }

  ::close(fd);
  return body;
}

std::string call(const std::string& socket_path, const std::string& request)
{
  return local_exchange(socket_path, request);
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
    "tcp_listen": true,
    "tcp_address": "0.0.0.0",
    "tcp_port": 47811,
    "tcp_max_connections": 8,
    "tcp_max_message_bytes": 65536,
    "tcp_token": "0123456789abcdef0123456789abcdef",
    "tcp_auth_timeout_ms": 2000,
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
  CHECK(config.tcp_listen);
  CHECK(config.tcp_address == "0.0.0.0");
  CHECK(config.tcp_port == 47811u);
  CHECK(config.tcp_max_connections == 8u);
  CHECK(config.tcp_max_message_bytes == 65536u);
  CHECK(config.tcp_token == "0123456789abcdef0123456789abcdef");
  CHECK(config.tcp_auth_timeout_ms == 2000u);
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

  // The TCP transport's flags spell the same fields as the config keys, for the same reason every
  // other flag does: what a unit passes and what an operator types should be one vocabulary.
  CHECK(namchain::daemon::apply_option(config, "tcp-listen", "", error));
  CHECK(config.tcp_listen);
  CHECK(namchain::daemon::apply_option(config, "tcp-address", "0.0.0.0", error));
  CHECK(config.tcp_address == "0.0.0.0");
  CHECK(namchain::daemon::apply_option(config, "tcp-port", "0", error));
  CHECK(config.tcp_port == 0u);
  CHECK(namchain::daemon::apply_option(config, "tcp-max-connections", "2", error));
  CHECK(config.tcp_max_connections == 2u);
  CHECK(namchain::daemon::apply_option(config, "tcp-max-message-bytes", "4096", error));
  CHECK(config.tcp_max_message_bytes == 4096u);
  CHECK(namchain::daemon::apply_option(config, "tcp-token", "0123456789abcdef", error));
  CHECK(config.tcp_token == "0123456789abcdef");
  CHECK(namchain::daemon::apply_option(config, "tcp-token-file", "/etc/namchain/token", error));
  CHECK(config.tcp_token_file == "/etc/namchain/token");
  CHECK(namchain::daemon::apply_option(config, "tcp-auth-timeout-ms", "500", error));
  CHECK(config.tcp_auth_timeout_ms == 500u);
  CHECK_FALSE(namchain::daemon::apply_option(config, "tcp-address", "", error));
  CHECK_FALSE(namchain::daemon::apply_option(config, "tcp-token", "", error));

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
    // A version-1 client is still served, and is told what the server actually speaks: every change
    // since is additive, so a client that ignores what it does not know keeps working.
    const auto reply = nlohmann::json::parse(
      daemon.handle_request(R"({"id":"1","method":"engine.hello","protocol":1})"));
    CHECK(reply["ok"] == true);
    CHECK(reply["id"] == "1");
    CHECK(reply["protocol"] == 2);
    CHECK(reply["role"] == "namchaind");
    CHECK(reply["features"].is_array());
    CHECK(reply["features"].size() > 0);
  }
  {
    const auto reply = nlohmann::json::parse(
      daemon.handle_request(R"({"id":"2","method":"engine.hello","protocol":2})"));
    CHECK(reply["ok"] == true);
    CHECK(reply["protocol"] == 2);
  }
  for (const char* too_new : {"0", "99"})
  {
    const std::string request =
      std::string(R"({"id":"3","method":"engine.hello","protocol":)") + too_new + "}";
    const auto reply = nlohmann::json::parse(daemon.handle_request(request));
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
    // `ramp_ms` is a *build* parameter -- a smoothing time, so changing it is a rebuild rather than a
    // ramp inside a running plan. The router is what decides, and the response says which path it
    // took. Both of `nc.gain`'s value parameters, `gain_db` and `gain`, are rampable.
    const auto reply = nlohmann::json::parse(daemon.handle_request(
      R"({"method":"rig.patch","ops":[{"op":"set_param","node":"g","param":"ramp_ms","value":10.0}]})"));
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
  // Every block is timed against its deadline (64 frames at 48 kHz), and the histogram accounts for
  // at least the blocks counted before it was read (the render thread keeps going in between).
  const auto& render = status["status"]["render"];
  CHECK(render["deadline_us"].get<double>() == doctest::Approx(1333.333).epsilon(0.001));
  uint64_t timed = 0;
  for (const auto& bucket : render["load_histogram"])
  {
    CHECK(bucket[0].get<unsigned>() <= 100);
    timed += bucket[1].get<uint64_t>();
  }
  CHECK(timed > 0);
  CHECK(render["worst_us"].get<double>() > 0.0);
  CHECK(render["over_deadline"].get<uint64_t>() <= render["over_90_percent"].get<uint64_t>());

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

// ---------------------------------------------------------------------------
// The document on the wire (docs/ui-integration-plan.md §1.1, slice A3)
// ---------------------------------------------------------------------------

TEST_CASE("rig.apply takes a whole document and rig.get hands it back")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  std::filesystem::create_directories(root);

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.capture_channels = 2;
  config.playback_channels = 2;
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;
  config.lock_memory = false;
  config.rt_priority = 0;
  // No startup rig: the document arrives over the socket, which is how a UI would do it.

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

  // A document that names one node by element and role and mints a name the client never wrote.
  nlohmann::json document;
  document["name"] = "Applied";
  {
    nlohmann::json chain;
    chain["id"] = "gtr";
    chain["name"] = "Guitar";
    chain["input_channel"] = 0;
    chain["output_channel"] = 1;
    chain["nodes"] = nlohmann::json::array({
      {{"id", "in"}, {"type", "nc.input"}},
      {{"element", "blk7"}, {"role", "trim"}, {"type", "nc.gain"}, {"gain", 0.5}},
      {{"id", "out"}, {"type", "nc.output"}},
    });
    chain["connections"] = nlohmann::json::array({
      {{"from", "in"}, {"to", "blk7/trim"}},
      {{"from", "blk7/trim"}, {"to", "out"}},
    });
    document["chains"] = nlohmann::json::array({chain});
  }

  {
    nlohmann::json request;
    request["id"] = "a";
    request["method"] = "rig.apply";
    request["document"] = document;
    const auto reply = nlohmann::json::parse(call(config.socket_path, request.dump()));
    REQUIRE_MESSAGE(reply["ok"] == true, reply.dump());
    CHECK(reply["name"] == "Applied");
    CHECK(reply["chains"] == 1);
    REQUIRE(reply["generation"] == 1);
  }

  const bool running = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["audible_generation"].get<uint64_t>() == 1;
  });
  CHECK_MESSAGE(running, "an applied document never reached the device");

  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(status["status"]["rig_source"] == "applied");
  CHECK(status["status"]["rig_name"] == "Applied");
  CHECK(status["status"]["rig"] == ""); // there is no file, and saying so is better than inventing one
  CHECK(status["status"]["chains"][0]["output_channel"] == 1);

  // rig.get hands back the engine's parse: the minted node id, the element the client owns, and the
  // channel binding it asked for.
  const auto got = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  REQUIRE(got["ok"] == true);
  CHECK(got["source"] == "applied");
  CHECK(got["loaded"] == true);
  const auto& chain = got["document"]["chains"][0];
  CHECK(chain["id"] == "gtr");
  CHECK(chain["output_channel"] == 1);
  CHECK(chain["nodes"][1]["id"] == "blk7/trim");
  CHECK(chain["nodes"][1]["element"] == "blk7");
  CHECK(chain["nodes"][1]["role"] == "trim");
  CHECK(chain["nodes"][1]["gain"] == doctest::Approx(0.5));
  CHECK(got["chains"][0]["id"] == "gtr");
  CHECK(got["chains"][0]["audible_generation"] == 1);

  // Applying what rig.get returned is a document that changes nothing, so it does not advance the
  // generation: the round trip is a fixed point, which is what makes it usable for reconciliation.
  {
    nlohmann::json request;
    request["method"] = "rig.apply";
    request["document"] = got["document"];
    const auto reply = nlohmann::json::parse(call(config.socket_path, request.dump()));
    REQUIRE_MESSAGE(reply["ok"] == true, reply.dump());
    CHECK(reply["generation"] == 1);
  }

  // And a patch on an applied document works without a chain, because it has one.
  const auto patch = nlohmann::json::parse(call(
    config.socket_path,
    R"({"method":"rig.patch","ops":[{"op":"set_param","node":"blk7/trim","param":"gain","value":0.75}]})"));
  CHECK(patch["ok"] == true);
  CHECK(patch["kind"] == "smooth");
}

TEST_CASE("an applied document resolves its relative paths against the rigs root")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  std::filesystem::create_directories(root / "models");
  // A stand-in rather than a model: what is being tested is which path the daemon tried to load, and
  // the failure names it.
  write_file(root / "models" / "missing.nam", "not a model");

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;
  config.lock_memory = false;
  config.rt_priority = 0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);

  const auto reply = nlohmann::json::parse(daemon.handle_request(R"({"method":"rig.apply","document":
    {"chains":[{"id":"c","nodes":[{"id":"in","type":"nc.input"},
      {"id":"amp","type":"nc.nam","model":"models/missing.nam"},
      {"id":"out","type":"nc.output"}],
      "connections":[{"from":"in","to":"amp"},{"from":"amp","to":"out"}]}]}})"));
  CHECK(reply["ok"] == false);
  const std::string message = reply["error"]["message"];
  // The path in the message is absolute and under the rigs root, which is the rule the socket's trust
  // boundary needs: a client cannot point the daemon at an arbitrary directory by applying a document.
  CHECK(message.find((root / "models" / "missing.nam").string()) != std::string::npos);
}

TEST_CASE("rig.apply refuses a document with no chains and one that does not parse")
{
  Config config;
  Daemon daemon(config);

  const auto empty = nlohmann::json::parse(
    daemon.handle_request(R"({"method":"rig.apply","document":{"chains":[]}})"));
  CHECK(empty["ok"] == false);
  CHECK(empty["error"]["code"] == "bad_request");
  CHECK(empty["error"]["message"].get<std::string>().find("at least one chain") != std::string::npos);

  const auto malformed =
    nlohmann::json::parse(daemon.handle_request(R"({"method":"rig.apply","document":{"chains":{}}})"));
  CHECK(malformed["ok"] == false);
  CHECK(malformed["error"]["code"] == "bad_request");

  const auto missing = nlohmann::json::parse(daemon.handle_request(R"({"method":"rig.apply"})"));
  CHECK(missing["ok"] == false);
  CHECK(missing["error"]["code"] == "bad_request");
}

// ---------------------------------------------------------------------------
// The catalogue on the wire (docs/ui-integration-plan.md §1.4, slice B)
// ---------------------------------------------------------------------------

TEST_CASE("engine.schema publishes the catalogue a palette and an inspector are built from")
{
  Config config;
  Daemon daemon(config);

  const auto reply = nlohmann::json::parse(daemon.handle_request(R"({"method":"engine.schema"})"));
  REQUIRE(reply["ok"] == true);
  CHECK(reply["protocol"] == 2);
  CHECK(reply["max_chains"] == namchain::daemon::kMaxChains);
  // Declared and refused: nothing in the engine carries more than one channel per port, and a client
  // reads this rather than offering a pan control that would be refused.
  CHECK(reply["stereo"] == false);

  const nlohmann::json* gain = nullptr;
  for (const auto& node : reply["nodes"])
  {
    if (node["id"] == "nc.gain")
      gain = &node;
  }
  REQUIRE(gain != nullptr);
  CHECK((*gain)["name"] == "Gain");
  CHECK((*gain)["category"] == "utility");
  CHECK((*gain)["kind"] == "primitive");
  CHECK((*gain)["ports"]["in"][0]["channels"] == 1);
  // A legal bypass target: which is what lets a client offer the control instead of finding out.
  CHECK((*gain)["flags"]["bypassable"] == true);
  CHECK((*gain)["arity"] == nullptr);

  const auto& params = (*gain)["params"];
  REQUIRE(params.size() == 3u);
  CHECK(params[0]["id"] == "gain_db");
  CHECK(params[0]["unit"] == "dB");
  CHECK(params[0]["rampable"] == true);
  CHECK(params[0]["min"] == -60.0);
  CHECK(params[0]["max"] == 24.0);
  CHECK(params[0]["min_inclusive"] == true);
  // The linear spelling is the same value under another name, marked so that a generic inspector shows
  // one knob rather than two.
  CHECK(params[1]["id"] == "gain");
  CHECK(params[1]["hidden"] == true);
  CHECK(params[1]["rampable"] == true);
  // A smoothing time: declared, patchable, and not rampable, which is what tells a client to commit it
  // on release rather than streaming it.
  CHECK(params[2]["id"] == "ramp_ms");
  CHECK(params[2]["rampable"] == false);

  // An arity node describes its own port count, so an "add a lane" control knows its bounds.
  for (const auto& node : reply["nodes"])
  {
    if (node["id"] != "nc.mix")
      continue;
    CHECK(node["arity"]["param"] == "inputs");
    CHECK(node["arity"]["min"] == 1);
    CHECK(node["arity"]["max"] == 64);
    CHECK(node["arity"]["default"] == 2);
  }

  // An option parameter carries the values *and* their labels, and one whose values live on the device
  // says where they come from instead (the registry is slice D).
  for (const auto& node : reply["nodes"])
  {
    if (node["id"] == "nc.eq.biquad")
    {
      CHECK(node["params"][0]["id"] == "mode");
      CHECK(node["params"][0]["kind"] == "option");
      CHECK(node["params"][0]["options"][0]["value"] == "peaking");
      CHECK(node["params"][0]["options"][0]["label"] == "Peaking");
      CHECK(node["params"][0]["options_from"] == nullptr);
    }
    if (node["id"] == "nc.nam")
    {
      CHECK(node["params"][0]["id"] == "model");
      CHECK(node["params"][0]["required"] == true);
      CHECK(node["params"][0]["default"] == nullptr); // no default: it must be given
      CHECK(node["params"][0]["options_from"] == "models");
    }
  }
}

TEST_CASE("an applied document with a parameter that does not exist is refused by name")
{
  Config config;
  Daemon daemon(config);

  const auto reply = nlohmann::json::parse(daemon.handle_request(R"({"method":"rig.apply","document":
    {"chains":[{"id":"c","nodes":[{"id":"in","type":"nc.input"},
      {"id":"g","type":"nc.gain","gainz":3.0},
      {"id":"out","type":"nc.output"}],
      "connections":[{"from":"in","to":"g"},{"from":"g","to":"out"}]}]}})"));
  CHECK(reply["ok"] == false);
  CHECK(reply["error"]["code"] == "compile_failed");
  const std::string message = reply["error"]["message"];
  // The whole point of the slice: this used to be published happily and the knob did nothing.
  CHECK(message.find("gainz") != std::string::npos);
  CHECK(message.find("gain_db") != std::string::npos);
}

TEST_CASE("a per-port parameter is a smooth edit, and a port that does not exist is refused")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  // in -> mix (both ports fed) -> out: the output is the sum of the two ports' gains.
  write_file(root / "mix.json", R"({
    "nodes": [
      { "id": "in", "type": "nc.input" },
      { "id": "m",  "type": "nc.mix", "inputs": 2, "ramp_ms": 5 },
      { "id": "out", "type": "nc.output" }
    ],
    "connections": [
      { "from": "in", "to": "m", "to_port": 0 },
      { "from": "in", "to": "m", "to_port": 1 },
      { "from": "m",  "to": "out" }
    ]
  })");

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.rig = (root / "mix.json").string();
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

  // A lane fader is the smooth class: a lane's level must not cost a compile and a crossfade, or
  // every drag of a lane slider would be a rebuild.
  const auto level = nlohmann::json::parse(call(
    config.socket_path,
    R"({"id":"p","method":"rig.patch","ops":[{"op":"set_param","node":"m","port":0,"param":"level_db","value":-6}]})"));
  REQUIRE(level["ok"] == true);
  CHECK(level["kind"] == "smooth");
  CHECK(level["generation"] == 1); // no recompile, so no new generation
  const bool applied = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["params_applied"].get<uint64_t>() == 1;
  });
  CHECK_MESSAGE(applied, "the render thread never applied the lane's level");

  // Mute and polarity are lanes too, and they take the same path.
  const auto muted = nlohmann::json::parse(call(
    config.socket_path,
    R"({"id":"q","method":"rig.patch","ops":[{"op":"set_param","node":"m","port":1,"param":"mute","value":1}]})"));
  CHECK(muted["ok"] == true);
  CHECK(muted["kind"] == "smooth");

  // A port the mixer does not have is refused by name rather than silently ignored: with two inputs
  // there is no lane 2, and a client that set one would believe it had done something.
  const auto missing = nlohmann::json::parse(call(
    config.socket_path,
    R"({"id":"r","method":"rig.patch","ops":[{"op":"set_param","node":"m","port":2,"param":"level_db","value":-6}]})"));
  CHECK(missing["ok"] == false);
  CHECK(missing["error"]["code"] == "patch_rejected");
  CHECK(missing["error"]["message"].get<std::string>().find("port 2") != std::string::npos);

  // A node with no per-port parameters has no ports to address, and says so the same way.
  const auto no_ports = nlohmann::json::parse(call(
    config.socket_path,
    R"({"id":"s","method":"rig.patch","ops":[{"op":"set_param","node":"out","port":0,"param":"level_db","value":-6}]})"));
  CHECK(no_ports["ok"] == false);
  CHECK(no_ports["error"]["code"] == "patch_rejected");
}

// ---------------------------------------------------------------------------
// Health and issues (docs/ui-integration-plan.md §1.6, slice B5)
// ---------------------------------------------------------------------------

TEST_CASE("a daemon with nothing wrong reports health ok and no issues")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "gain.json", kGainRig);

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.rig = (root / "gain.json").string();
  // Empty libraries that exist, so this case is about a daemon with nothing to report: a *missing*
  // library directory is an informational issue of its own (see the library case below), which is
  // honest but not what "nothing wrong" means.
  config.models_root = (dir.path / "models").string();
  config.irs_root = (dir.path / "irs").string();
  std::filesystem::create_directories(config.models_root);
  std::filesystem::create_directories(config.irs_root);
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
    return status["state"] == "running" && status["blocks"].get<uint64_t>() > 0;
  }));

  const auto reply = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  const auto& status = reply["status"];
  // The one field a light binds to, and the reasons behind it in a shape a client maps to its own
  // words rather than parses.
  CHECK(status["health"] == "ok");
  CHECK(status["issues"].is_array());
  CHECK(status["issues"].empty());
  // Deliberately not issues: a daemon that was asked for no real-time priority and got none, and one
  // that was asked not to lock memory, are doing exactly what they were told.
  CHECK(status["rt_priority"] == 0);
  CHECK(status["memory_locked"] == false);
}

TEST_CASE("a device that cannot be opened is an error issue, not just a counter")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "gain.json", kGainRig);

  Config config;
  config.backend = BackendKind::AlsaRaw;
  config.device = "hw:99,0"; // not a device
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.rig = (root / "gain.json").string();
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

  REQUIRE(wait_for(config.socket_path, [](const nlohmann::json& status) { return status["state"] == "lost"; }));

  const auto reply = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  const auto& status = reply["status"];
  CHECK(status["health"] == "error");
  REQUIRE(status["issues"].size() >= 1u);

  bool found = false;
  for (const auto& issue : status["issues"])
  {
    if (issue["code"] != "device_lost")
      continue;
    found = true;
    CHECK(issue["severity"] == "error");
    CHECK(issue["count"].get<uint64_t>() >= 1u);
    CHECK(issue["message"].get<std::string>().find("retrying") != std::string::npos);
  }
  CHECK_MESSAGE(found, "a device that will not open must say so as an issue, not only as a counter");
  // The device state and the health code agree, because one is derived from the other rather than
  // tracked beside it.
  CHECK(status["device_lost"].get<uint64_t>() >= 1u);
  CHECK(status["health"] == "error");
}

// ---------------------------------------------------------------------------
// The library: models and IRs by id (docs/ui-integration-plan.md slice D)
// ---------------------------------------------------------------------------

TEST_CASE("the library is published, refreshed on request, and compiled by id")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  const auto models = dir.path / "models";
  const auto irs = dir.path / "irs";

  // A rig that names the model the way a client does: by id, with no path and no extension. Before
  // slice D this could only be a filename; now it is a library entry.
  write_file(root / "by-id.json", R"({
    "name": "by id",
    "nodes": [
      { "id": "in", "type": "nc.input" },
      { "id": "amp", "type": "nc.nam", "model": "A2-Lite" },
      { "id": "out", "type": "nc.output" }
    ],
    "connections": [ { "from": "in", "to": "amp" }, { "from": "amp", "to": "out" } ]
  })");
  write_file(root / "missing.json", R"({
    "nodes": [
      { "id": "in", "type": "nc.input" },
      { "id": "amp", "type": "nc.nam", "model": "A2-Lit" },
      { "id": "out", "type": "nc.output" }
    ],
    "connections": [ { "from": "in", "to": "amp" }, { "from": "amp", "to": "out" } ]
  })");

  // A library with one real model in it, by reference to the generated fixture rather than a copy:
  // the registry reads a header, and the compiler loads the file it points at.
  std::filesystem::create_directories(models);
  std::filesystem::create_directories(irs);
  std::filesystem::copy_file(std::string(NAMCHAIN_TEST_DATA_DIR) + "/A2-Lite.nam", models / "A2-Lite.nam");
  std::filesystem::copy_file(std::string(NAMCHAIN_TEST_DATA_DIR) + "/CabIR.nam", irs / "cab.nam");

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.models_root = models.string();
  config.irs_root = irs.string();
  config.lock_memory = false;
  config.rt_priority = 0;
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;

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

  // The listing: an id that is what a document names, and the facts a palette wants.
  const auto assets = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.assets"})"));
  REQUIRE(assets["ok"] == true);
  REQUIRE(assets["models"].size() == 1);
  CHECK(assets["models"][0]["id"] == "A2-Lite");
  CHECK(assets["models"][0]["kind"] == "model");
  CHECK(assets["models"][0]["architecture"] == "WaveNet");
  CHECK(assets["models"][0]["sample_rate"] == 48000.0);
  CHECK(assets["models"][0]["path"] == std::filesystem::canonical(models / "A2-Lite.nam").string());
  CHECK(assets["scan"] == 1);
  CHECK(assets["roots"]["models"]["readable"] == true);
  REQUIRE(assets["irs"].size() == 1);
  CHECK(assets["irs"][0]["id"] == "cab");

  // engine.schema resolves the same scan into the node types' option lists, so a palette does not
  // have to join two answers to build a picker.
  const auto schema = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.schema"})"));
  REQUIRE(schema["ok"] == true);
  nlohmann::json amp_params;
  for (const auto& node : schema["nodes"])
  {
    if (node["id"] == "nc.nam")
      amp_params = node["params"];
  }
  REQUIRE(amp_params.size() == 1);
  CHECK(amp_params[0]["options_from"] == "models");
  REQUIRE(amp_params[0]["options"].size() == 1);
  CHECK(amp_params[0]["options"][0]["value"] == "A2-Lite");
  CHECK(amp_params[0]["options"][0]["label"] == "A2-Lite");

  // A model added while the daemon runs appears only when the scan is asked for, which is what makes
  // a refresh a thing a client can decide to do rather than a race it has to survive.
  std::filesystem::copy_file(std::string(NAMCHAIN_TEST_DATA_DIR) + "/CabIR.nam", models / "second.nam");
  const auto stale = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.assets"})"));
  CHECK(stale["models"].size() == 1);
  const auto refreshed =
    nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.assets","refresh":true})"));
  CHECK(refreshed["models"].size() == 2);
  CHECK(refreshed["scan"] == 2);
  CHECK(refreshed["files_parsed"] == 1); // one new file; the other two came from the cache
  const auto now_three = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.schema"})"));
  for (const auto& node : now_three["nodes"])
  {
    if (node["id"] == "nc.nam")
      CHECK(node["params"][0]["options"].size() == 2);
  }

  // The point of the whole slice: a document that names an id compiles, renders, and does not have to
  // know where the file is.
  const auto applied = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.load","rig":"by-id.json"})"));
  REQUIRE(applied["ok"] == true);
  CHECK(applied["generation"].get<uint64_t>() >= 1);
  const bool adopted = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["adopted_generation"].get<uint64_t>() >= 1 &&
           status["blocks"].get<uint64_t>() > 0;
  });
  CHECK_MESSAGE(adopted, "the plan built from a model id never reached the device");

  // The document the engine hands back still says `A2-Lite`: resolution happened on the copy it
  // compiled, so a client sees its own ids and not this device's paths.
  const auto running = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  REQUIRE(running["ok"] == true);
  CHECK(running["document"]["chains"][0]["nodes"][1]["model"] == "A2-Lite");

  // A miss is refused by name, with the library's near miss, before anything is compiled.
  const auto missing =
    nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.load","rig":"missing.json"})"));
  REQUIRE(missing["ok"] == false);
  CHECK(missing["error"]["code"] == "asset_missing");
  CHECK(missing["error"]["message"].get<std::string>().find("A2-Lite") != std::string::npos);
  CHECK(missing["error"]["message"].get<std::string>().find("did you mean") != std::string::npos);

  // And the same through a patch, because `swap_model` names a model too.
  const auto swapped = nlohmann::json::parse(call(config.socket_path,
                                                  R"({"method":"rig.patch","ops":[{"op":"swap_model",
                                                     "node":"amp","model":"A2-Lit"}]})"));
  REQUIRE(swapped["ok"] == false);
  CHECK(swapped["error"]["code"] == "asset_missing");
}

TEST_CASE("a library root that is not there is an informational issue, not a failure")
{
  TempDir dir;
  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = (dir.path / "rigs").string();
  config.models_root = (dir.path / "no-models-here").string();
  config.irs_root = (dir.path / "no-irs-here").string();
  config.lock_memory = false;
  config.rt_priority = 0;
  config.reclamation_timeout_ms = 5;

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

  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  REQUIRE(status["ok"] == true);
  CHECK(status["status"]["assets"]["models"] == 0);
  CHECK(status["status"]["assets"]["irs"] == 0);

  bool named = false;
  for (const auto& issue : status["status"]["issues"])
  {
    if (issue["code"] == "models_root_missing")
      named = issue["severity"] == "info" && issue["message"].get<std::string>().find("no-models-here") !=
                                               std::string::npos;
  }
  CHECK(named);
  // Informational issues do not colour the light, which is the point of the severity: a device with
  // no IRs is not a device that is unwell.
  CHECK(status["status"]["health"] == "ok");

  const auto assets = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.assets"})"));
  CHECK(assets["models"].empty());
  CHECK(assets["roots"]["models"]["readable"] == false);
  CHECK(assets["roots"]["models"]["error"] == "no such directory");
}

// ---------------------------------------------------------------------------
// Macros on the wire: element addressing, lowering, and what a patch records
// ---------------------------------------------------------------------------

namespace
{

/// A chain with two macros in it, which is the shape the addressing tests need: a feedback delay and
/// a tone stack, both behind one input.
const char* const kMacroRig = R"({
  "name": "macros",
  "nodes": [
    { "id": "in", "type": "nc.input" },
    { "id": "g", "type": "nc.gain", "gain_db": 0 },
    { "element": "dly", "type": "fx.delay.feedback", "time": 200, "feedback": 0.4, "mix": 0.5 },
    { "element": "ts", "type": "fx.amp.tonestack", "bass": 2, "mid": -1 },
    { "id": "out", "type": "nc.output" }
  ],
  "connections": [
    { "from": "in", "to": "g" },
    { "from": "g", "to": "dly" },
    { "from": "dly", "to": "ts" },
    { "from": "ts", "to": "out" }
  ]
})";

} // namespace

TEST_CASE("a document with macros renders, reports both views, and is patched by element")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "macros.json", kMacroRig);

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.models_root = NAMCHAIN_TEST_DATA_DIR;
  config.irs_root = NAMCHAIN_TEST_DATA_DIR;
  config.rig = (root / "macros.json").string();
  config.lock_memory = false;
  config.rt_priority = 0;
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;

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
    return status["state"] == "running" && status["blocks"].get<uint64_t>() > 0;
  }));

  // Both views: the document as the client wrote it (two macros), and the primitives they became.
  const auto get = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  REQUIRE(get["ok"] == true);
  REQUIRE(get["document"]["chains"][0]["nodes"].size() == 5);
  CHECK(get["document"]["chains"][0]["nodes"][2]["type"] == "fx.delay.feedback");
  CHECK(get["document"]["chains"][0]["nodes"][2]["element"] == "dly");

  const auto& lowered = get["lowered"][0]["nodes"];
  CHECK(lowered.size() == 13); // in + gain + 7 for the delay + 3 for the tone stack + out
  bool saw_role = false;
  bool saw_minted_id = false;
  for (const auto& node : lowered)
  {
    saw_role = saw_role || node["role"] == "line";
    saw_minted_id = saw_minted_id || node["id"] == "dly:line";
  }
  CHECK(saw_role);
  CHECK(saw_minted_id);

  // A macro-level parameter, addressed by element alone: one knob, two gains, one smooth edit.
  const auto mix = nlohmann::json::parse(call(
    config.socket_path,
    R"({"method":"rig.patch","ops":[{"op":"set_param","element":"dly","param":"mix","value":0.75}]})"));
  REQUIRE(mix["ok"] == true);
  CHECK(mix["kind"] == "smooth");
  CHECK(mix["applied"] == 1);
  const bool applied = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["params_applied"].get<uint64_t>() >= 2; // both gains of the crossfade
  });
  CHECK_MESSAGE(applied, "a macro parameter did not reach both of its targets");

  // The document follows what is playing, so `rig.get` is not a stale answer and a later structural
  // edit does not silently revert the knob.
  const auto after = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  CHECK(after["document"]["chains"][0]["nodes"][2]["mix"] == 0.75);
  CHECK(after["document"]["chains"][0]["nodes"][1]["gain_db"] == 0); // nothing else moved

  // A plain node is addressed by its element too -- which defaults to its id, so a hand-written rig
  // needs no new field to be addressable this way.
  const auto gain = nlohmann::json::parse(call(
    config.socket_path,
    R"({"method":"rig.patch","ops":[{"op":"set_param","element":"g","param":"gain_db","value":-6}]})"));
  REQUIRE(gain["ok"] == true);
  CHECK(gain["kind"] == "smooth");
  CHECK(nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"))["document"]["chains"][0]
          ["nodes"][1]["gain_db"] == -6);

  // A parameter the lowering itself consumes (a band count) is structural, and goes through the
  // document rather than the plan.
  const auto bands = nlohmann::json::parse(call(
    config.socket_path,
    R"({"method":"rig.patch","ops":[{"op":"set_param","element":"ts","param":"mid_freq","value":800}]})"));
  REQUIRE(bands["ok"] == true);
  CHECK(bands["kind"] == "smooth"); // mid_freq is one biquad's frequency: rampable

  // And a parameter that is genuinely structural: `bands` on the EQ macro, which changes which bands
  // are bypassed rather than a value anywhere.
  write_file(root / "eq.json", R"({
    "nodes": [
      { "id": "in", "type": "nc.input" },
      { "element": "eq", "type": "fx.eq.bands" },
      { "id": "out", "type": "nc.output" }
    ],
    "connections": [ { "from": "in", "to": "eq" }, { "from": "eq", "to": "out" } ]
  })");
  REQUIRE(nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.load","rig":"eq.json"})"))["ok"] ==
          true);
  const auto before_generation = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.get"})"))["generation"].get<uint64_t>();
  const auto band_change = nlohmann::json::parse(call(
    config.socket_path, R"({"method":"rig.patch","ops":[{"op":"set_param","element":"eq","param":"bands","value":2}]})"));
  REQUIRE(band_change["ok"] == true);
  CHECK(band_change["kind"] == "structural");
  CHECK(band_change["generation"].get<uint64_t>() > before_generation);
  CHECK(nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"))["document"]["chains"][0]
          ["nodes"][1]["bands"] == 2);

  // `element` + `role` addresses one node of the lowering, which is the escape hatch for poking a
  // primitive inside a macro.
  const auto role_edit = nlohmann::json::parse(call(
    config.socket_path,
    R"({"method":"rig.patch","ops":[{"op":"set_param","element":"eq","role":"band1","param":"freq","value":250}]})"));
  REQUIRE(role_edit["ok"] == true);
  CHECK(role_edit["kind"] == "smooth");

  // Bypassing a whole macro is one flag on the macro, and it is a structural edit -- the lowering
  // decides what the flag means.
  const auto bypass = nlohmann::json::parse(call(
    config.socket_path, R"({"method":"rig.patch","ops":[{"op":"bypass","element":"eq","bypass":true}]})"));
  REQUIRE(bypass["ok"] == true);
  CHECK(bypass["kind"] == "structural");
  const auto bypassed = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  CHECK(bypassed["document"]["chains"][0]["nodes"][1]["bypass"] == true); // the eq rig: nodes[1] is the macro
  bool every_band_bypassed = true;
  for (const auto& node : bypassed["lowered"][0]["nodes"])
    every_band_bypassed = every_band_bypassed && (node["bypass"] == true || node["type"] == "nc.input" ||
                                                  node["type"] == "nc.output");
  CHECK(every_band_bypassed);

  // An address that names nothing is refused with what exists, rather than doing nothing.
  const auto unknown = nlohmann::json::parse(call(
    config.socket_path,
    R"({"method":"rig.patch","ops":[{"op":"set_param","element":"nope","param":"time","value":1}]})"));
  REQUIRE(unknown["ok"] == false);
  CHECK(unknown["error"]["code"] == "patch_rejected");
  CHECK(unknown["error"]["message"].get<std::string>().find("nope") != std::string::npos);

  const auto unknown_role = nlohmann::json::parse(call(
    config.socket_path,
    R"({"method":"rig.patch","ops":[{"op":"set_param","element":"eq","role":"band9","param":"freq","value":250}]})"));
  REQUIRE(unknown_role["ok"] == false);
  CHECK(unknown_role["error"]["message"].get<std::string>().find("band9") != std::string::npos);

  // A macro parameter that does not exist is refused with the list of the ones that do.
  const auto unknown_param = nlohmann::json::parse(call(
    config.socket_path,
    R"({"method":"rig.patch","ops":[{"op":"set_param","element":"eq","param":"gain","value":1}]})"));
  REQUIRE(unknown_param["ok"] == false);
  CHECK(unknown_param["error"]["message"].get<std::string>().find("gain1") != std::string::npos);
}

TEST_CASE("engine.schema publishes macros beside the primitives, and a macro's IR comes from the library")
{
  TempDir dir;
  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = (dir.path / "rigs").string();
  config.models_root = NAMCHAIN_TEST_DATA_DIR;
  config.irs_root = NAMCHAIN_TEST_DATA_DIR;
  config.lock_memory = false;
  config.rt_priority = 0;
  config.reclamation_timeout_ms = 5;

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

  const auto schema = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.schema"})"));
  REQUIRE(schema["ok"] == true);

  const nlohmann::json* delay = nullptr;
  const nlohmann::json* cab = nullptr;
  std::size_t macros = 0;
  std::size_t primitives = 0;
  for (const auto& node : schema["nodes"])
  {
    if (node["kind"] == "macro")
    {
      ++macros;
      if (node["id"] == "fx.delay.feedback")
        delay = &node;
      if (node["id"] == "fx.cab.ir")
        cab = &node;
    }
    else
    {
      ++primitives;
      // A primitive says so too, so one array can be filtered either way.
      CHECK(node["kind"] == "primitive");
    }
  }
  CHECK(macros == 7);
  CHECK(primitives >= 11);
  REQUIRE(delay != nullptr);
  REQUIRE(cab != nullptr);

  // A macro publishes the same parameter fields as a node type, plus its roles and its param map.
  CHECK((*delay)["inputs"] == 1);
  CHECK((*delay)["outputs"] == 1);
  CHECK((*delay)["bypass"] == "crossfade");
  bool has_time = false;
  bool has_target = false;
  for (const auto& param : (*delay)["params"])
  {
    if (param["id"] != "time")
      continue;
    has_time = true;
    CHECK(param["kind"] == "number");
    CHECK(param["unit"] == "ms");
    CHECK(param["rampable"] == true);
    CHECK(param["min"] == 1.0);
    CHECK(param["max"] == 1000.0);
    REQUIRE(param["targets"].size() == 1);
    has_target = param["targets"][0]["role"] == "line" && param["targets"][0]["param"] == "ms" &&
                 param["targets"][0]["transform"] == "direct";
  }
  CHECK(has_time);
  CHECK(has_target);

  bool saw_line_role = false;
  for (const auto& role : (*delay)["roles"])
    saw_line_role = saw_line_role || (role["role"] == "line" && role["type"] == "nc.delay");
  CHECK(saw_line_role);

  // The cabinet's `ir` is an option whose values come from the device's library -- the same scan
  // `engine.assets` reports -- and the macro's own schema is what the client reads it from.
  const auto& ir_param = (*cab)["params"][0];
  CHECK(ir_param["id"] == "ir");
  CHECK(ir_param["kind"] == "option");
  CHECK(ir_param["options_from"] == "irs");
  CHECK(ir_param["required"] == true);
  REQUIRE(ir_param["options"].size() >= 1);
  bool saw_cab_ir = false;
  for (const auto& option : ir_param["options"])
    saw_cab_ir = saw_cab_ir || option["value"] == "CabIR";
  CHECK(saw_cab_ir);

  // A macro names its IR by library id, and it resolves through the registry on the way to the node
  // that will load it -- which is the piece that makes a macro's asset parameter the same thing as a
  // primitive's.
  namchain::graph::Document document;
  std::string parse_error;
  const char* const rig = R"({
    "nodes": [
      { "id": "in", "type": "nc.input" },
      { "element": "cab", "type": "fx.cab.ir", "ir": "CabIR" },
      { "id": "out", "type": "nc.output" }
    ],
    "connections": [ { "from": "in", "to": "cab" }, { "from": "cab", "to": "out" } ]
  })";
  REQUIRE_MESSAGE(namchain::graph::parse_document_json(rig, document, parse_error), parse_error);
  namchain::graph::Graph primitives_graph;
  REQUIRE_MESSAGE(namchain::graph::lower_graph(document.chains[0].graph, primitives_graph, parse_error),
                  parse_error);
  // Before resolution the string is still the id; the daemon resolves it on the copy it compiles, so
  // this checks the lowering carries a string parameter through to the node that reads it.
  bool carried = false;
  for (const namchain::graph::NodeSpec& spec : primitives_graph.nodes)
    carried = carried || (spec.role == "ir" && spec.strings.count("ir") == 1 && spec.strings.at("ir") == "CabIR");
  CHECK(carried);
}

TEST_CASE("a recent-trouble bucket reaches zero in bounded time")
{
  using namchain::daemon::decay_recent_trouble;

  CHECK(decay_recent_trouble(0) == 0);
  CHECK(decay_recent_trouble(1) == 0); // the case the first version got wrong: it stuck at 7
  CHECK(decay_recent_trouble(7) == 6);
  CHECK(decay_recent_trouble(18) == 16);

  // The property the bucket exists for: a light that has been amber goes back to green. Eighteen
  // xruns -- one dropout -- must be gone within a few seconds of blocks, not never.
  uint64_t value = 18;
  int steps = 0;
  while (value != 0 && steps < 1000)
  {
    value = decay_recent_trouble(value);
    ++steps;
  }
  CHECK(value == 0);
  CHECK(steps < 40); // ~40 * 100 blocks = 5 s at 64 frames and 48 kHz

  // And it is still a decay: a big count takes longer than a small one, and never grows.
  CHECK(decay_recent_trouble(1000) < 1000);
  CHECK(decay_recent_trouble(1000) > decay_recent_trouble(18));
}
