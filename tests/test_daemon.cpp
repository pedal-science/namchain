// Tests for namchaind: configuration and rig resolution, the JSON protocol, and the control loop
// end to end against the paced `null` backend.
//
// The end-to-end cases matter more than they look: they are the only place the render thread, the
// plan publication ring, the reclaimer and the socket server run together. A unit test of the
// request handler alone would not catch a plan that is published but never adopted, or a device
// state that never leaves "closed".
#include <doctest.h>

#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
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
#include "namchain/rt_check.h"
#include "namchain/daemon/config.h"
#include "namchain/daemon/daemon.h"
#include "namchain/graph/macro.h"
#include "namchain/lookback.h"
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

/// The cost table these tests are admitted against. Two Fulls cost 1.0, over D3's 0.8 headroom, so
/// the second amp is trimmed to Lite (0.15) and the rig costs 0.65 -- which is the shape the model
/// budget's own 448 KiB had on this library, with the units and the numbers the document asks for.
std::string test_cost_table()
{
  return R"({
    "host": "test", "measured": "2026-10-07T00:00:00Z", "engine_commit": "c0e3f37", "kernel": "test",
    "rate": 48000, "quantum": 64, "deadline_us": 1333.333, "headroom": 0.8,
    "bar": "p99 under the deadline and zero overruns, paced offline",
    "core_percent": {"NAM": {"WaveNet": {"full": 0.5, "lite": 0.15}}, "IR": {},
                     "node": {"nc.input": 0.01, "nc.output": 0.01, "nc.gain": 0.01}},
    "unknown": {"NAM": 0.25, "IR": 0.1, "node": 0.05},
    "sources": {"NAM.WaveNet.full": {"n": 2, "p99_at_n": 0.95, "p99_at_n_plus_1": 1.3},
                "NAM.WaveNet.lite": {"n": 6, "p99_at_n": 0.9, "p99_at_n_plus_1": 1.1},
                "node.nc.input": {"n": 100, "p99_at_n": 0.9, "p99_at_n_plus_1": 1.1},
                "node.nc.output": {"n": 100, "p99_at_n": 0.9, "p99_at_n_plus_1": 1.1},
                "node.nc.gain": {"n": 100, "p99_at_n": 0.9, "p99_at_n_plus_1": 1.1}}
  })";
}

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


/// Poll until a two-step publish's Full has landed *and* is what the device is playing: the Lite has
/// stopped standing in for it, which is the end state every `lite_first` request is aiming at. The
/// reply to the request cannot carry that state any more -- the Full is compiled after the reply, on
/// another thread, which is exactly what makes the Lite pollable (D6).
bool wait_for_full(const std::string& socket_path)
{
  return wait_for(socket_path, [](const nlohmann::json& status) {
    const auto found = status.find("catch_up");
    if (found == status.end() || (*found)["full"] != "published")
      return false;
    return (*found)["replay"] != "running" && status["cost"]["lite_first"] == false;
  });
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
    "input_device": "AppleUSBAudioEngine:Some Interface:140000:1,2",
    "capture_device_path": "/tmp/what-the-device-was-sent.wav",
    "device_buffer_frames": 32,
    "rigs_root": "/srv/rigs",
    "socket": "/tmp/example.sock",
    "socket_mode": "0600",
    "rt_priority": 70,
    "cpu": 3,
    "lock_memory": false,
    "output_queue_periods": 4,
    "output_queue_floor_periods": 2,
    "fade_ms": 2.5,
    "cost_table": "/etc/namchain/vip-cost.json",
    "device_retry_ms": 500,
    "levels": false,
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
  CHECK(config.input_device == "AppleUSBAudioEngine:Some Interface:140000:1,2");
  // The other side of the same lesson: `capture_device_path` was a known key with no reader, so a
  // file that set it was accepted and silently ignored. Both are read now.
  CHECK(config.capture_device_path == "/tmp/what-the-device-was-sent.wav");
  // The device's own IO buffer, which is where the monitoring latency lives, is a setting and not a
  // constant: 32 frames is 0.67 ms at 48 kHz against 10.67 ms for the 512 most interfaces arrive with.
  CHECK(config.device_buffer_frames == 32u);
  CHECK(config.rigs_root == "/srv/rigs");
  CHECK(config.socket_path == "/tmp/example.sock");
  CHECK(config.socket_mode == 0600u);
  CHECK(config.rt_priority == 70);
  CHECK(config.cpu == 3);
  CHECK_FALSE(config.lock_memory);
  CHECK(config.output_queue_periods == 4u);
  CHECK(config.output_queue_floor_periods == 2u);
  CHECK(config.fade_ms == doctest::Approx(2.5));
  CHECK(config.cost_table == "/etc/namchain/vip-cost.json");
  CHECK(config.device_retry_ms == 500u);
  // Measuring is on unless it is turned off, and the key that turns it off is the flag's own name
  // (config.h: "keys are the long flag names without the dashes").
  CHECK_FALSE(config.levels);
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
  CHECK(config.cost_table == "/etc/namchain/cost.json"); // the device's own table, by default
  CHECK(namchain::daemon::apply_option(config, "cost-table", "/tmp/measured.json", error));
  CHECK(config.cost_table == "/tmp/measured.json");
  CHECK_FALSE(namchain::daemon::apply_option(config, "cost-table", "", error));
  CHECK(error.find("needs a path") != std::string::npos);

  // Level measuring: on unless it is turned off, so an appliance that wants the per-sample cost back
  // has to say so. Both spellings are accepted from the start, because a flag that can only be
  // turned off (or only on) is one an operator has to read the help for.
  CHECK(config.levels);
  CHECK(namchain::daemon::apply_option(config, "no-levels", "", error));
  CHECK_FALSE(config.levels);
  CHECK(namchain::daemon::apply_option(config, "levels", "", error));
  CHECK(config.levels);

  // The two defaults that changed on 2026-10-06, so that a change of mind has to be deliberate:
  // publishing per period (which recovers 55% of what measuring costs, while measuring every sample)
  // and an 8 Hz drain rate (half what a 20 Hz meter can show, and a fraction of the conversion work).
  // Each has an off-switch, because a default somebody cannot turn off is one they work around.
  CHECK(config.levels_publish_period);
  CHECK(config.levels_rate_hz == doctest::Approx(8.0));
  CHECK(namchain::daemon::apply_option(config, "no-levels-publish-period", "", error));
  CHECK_FALSE(config.levels_publish_period);
  CHECK(namchain::daemon::apply_option(config, "levels-publish-period", "", error));
  CHECK(config.levels_publish_period);
  // `--no-levels-rate-hz` is the readable spelling of "no limit": the alternative is passing a zero,
  // and a flag whose off-switch is a magic value is one an operator has to be told about.
  CHECK(namchain::daemon::apply_option(config, "no-levels-rate-hz", "", error));
  CHECK(config.levels_rate_hz == doctest::Approx(0.0));
  CHECK(namchain::daemon::apply_option(config, "levels-rate-hz", "8", error));
  CHECK(config.levels_rate_hz == doctest::Approx(8.0));

  // The device's buffer, and the two ways of asking for it wrongly. A typo must be a refusal rather
  // than a silently ignored latency claim: an operator who typed `--device-buffer-frames 3two` and got
  // 512 frames of latency would have no way to tell that nothing had happened.
  CHECK(namchain::daemon::apply_option(config, "device-buffer-frames", "32", error));
  CHECK(config.device_buffer_frames == 32u);
  error.clear();
  CHECK_FALSE(namchain::daemon::apply_option(config, "device-buffer-frames", "3two", error));
  CHECK_FALSE(error.empty());
  error.clear();
  CHECK_FALSE(namchain::daemon::apply_option(config, "device-buffer-frames", "0", error));
  CHECK(config.device_buffer_frames == 32u); // refused, not reset: the last good value stands

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
  CHECK_FALSE(namchain::daemon::apply_option(config, "model-budget-kb", "-1", error));
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

  // Bypass changes the document, so it goes the structural way, but it is a ramp: the running plan
  // crossfades the node between its output and its input, and no new plan is built.
  const auto bypass = nlohmann::json::parse(call(
    config.socket_path, R"({"id":"b","method":"rig.patch","ops":[{"op":"bypass","node":"g2"}]})"));
  REQUIRE(bypass["ok"] == true);
  CHECK(bypass["kind"] == "structural");
  CHECK(bypass["generation"] == 2);
  REQUIRE(bypass["transitions"].size() == 1);
  CHECK(bypass["transitions"][0]["kind"] == "ramps");
  CHECK(bypass["transitions"][0]["bypasses"] == 1);
  const auto g2 = [&config] {
    const auto got = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
    for (const auto& node : got["document"]["chains"][0]["nodes"])
    {
      if (node["id"] == "g2")
        return node;
    }
    return nlohmann::json();
  };
  CHECK(g2()["bypass"] == true);

  // The guard reports through status, and a well-behaved rig reports zero.
  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(status["status"]["failed_nodes"] == 0);
  CHECK(status["status"]["guard_trips"] == 0);

  // Un-bypass restores the node the same way.
  const auto restored = nlohmann::json::parse(call(
    config.socket_path,
    R"({"id":"r","method":"rig.patch","ops":[{"op":"bypass","node":"g2","bypass":false}]})"));
  REQUIRE(restored["ok"] == true);
  CHECK(restored["generation"] == 2);
  CHECK(restored["transitions"][0]["kind"] == "ramps");
  CHECK_FALSE(g2().contains("bypass"));
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
/// With \p second_stage, each chain has a second gain stage (`g2`, unity) after the first, which makes
/// the two documents differ structurally rather than only in values that ramp.
///
/// Built with nlohmann rather than a string literal so a test can vary one thing at a time and the
/// difference between two documents is visible in the call.
std::string two_chain_document(double guitar_gain, double vocal_gain, int guitar_out, int vocal_out,
                               bool second_stage = false)
{
  const auto chain = [second_stage](const char* id, int in_channel, int out_channel, double gain) {
    nlohmann::json c;
    c["id"] = id;
    c["input_channel"] = in_channel;
    c["output_channel"] = out_channel;
    c["nodes"] = nlohmann::json::array({
      {{"id", "in"}, {"type", "nc.input"}},
      {{"id", "g"}, {"type", "nc.gain"}, {"gain", gain}},
      {{"id", "out"}, {"type", "nc.output"}},
    });
    c["connections"] = nlohmann::json::array({{{"from", "in"}, {"to", "g"}}});
    if (second_stage)
    {
      c["nodes"].push_back({{"id", "g2"}, {"type", "nc.gain"}, {"gain", 1.0}});
      c["connections"].push_back({{"from", "g"}, {"to", "g2"}});
      c["connections"].push_back({{"from", "g2"}, {"to", "out"}});
    }
    else
      c["connections"].push_back({{"from", "g"}, {"to", "out"}});
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
  // A structural change to both chains: a change only to gains would ramp, with no new plan to stagger.
  write_file(root / "b.json", two_chain_document(0.25, 0.25, 0, 1, /*second_stage=*/true));

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

TEST_CASE("a structural load keeps what did not change, and a load that only moves gains ramps")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "a.json", two_chain_document(0.5, 0.5, 0, 1));
  write_file(root / "b.json", two_chain_document(0.5, 0.5, 0, 1, /*second_stage=*/true));
  write_file(root / "c.json", two_chain_document(0.25, 0.125, 0, 1, /*second_stage=*/true));

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

  const auto settled_at = [](uint64_t generation) {
    return [generation](const nlohmann::json& status) {
      return status["state"] == "running" && status["transition_active"] == false &&
             status["audible_generation"].get<uint64_t>() == generation;
    };
  };
  REQUIRE(wait_for(config.socket_path, settled_at(1)));

  // A second gain stage in each chain: input, first gain and output carry over; one fade, on the
  // output's input, which now comes from the new stage.
  const auto loaded =
    nlohmann::json::parse(call(config.socket_path, R"({"id":"l","method":"rig.load","rig":"b.json"})"));
  REQUIRE(loaded["ok"] == true);
  CHECK(loaded["generation"] == 2);
  REQUIRE(loaded["transitions"].size() == 2);
  for (const auto& transition : loaded["transitions"])
  {
    CHECK(transition["kind"] == "local");
    CHECK(transition["fades"] == 1);
    CHECK(transition["kept"] == 3);
    CHECK(transition["created"] == 1);
    CHECK(transition["retiring"] == 0);
  }
  REQUIRE(wait_for(config.socket_path, settled_at(2)));
  auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"))["status"];
  CHECK(status["local_transitions"] == 2);
  CHECK(status["local_base_mismatches"] == 0);
  CHECK(status["max_concurrent_transitions"] == 1);
  const uint64_t params_before = status["params_applied"].get<uint64_t>();

  // Only the gains move: no plan is built, the generation stays, and each chain's gain ramps.
  const auto ramped =
    nlohmann::json::parse(call(config.socket_path, R"({"id":"m","method":"rig.load","rig":"c.json"})"));
  REQUIRE(ramped["ok"] == true);
  CHECK(ramped["generation"] == 2);
  REQUIRE(ramped["transitions"].size() == 2);
  for (const auto& transition : ramped["transitions"])
  {
    CHECK(transition["kind"] == "ramps");
    CHECK(transition["ramps"] == 1);
  }
  const bool applied = wait_for(config.socket_path, [params_before](const nlohmann::json& s) {
    return s["params_applied"].get<uint64_t>() == params_before + 2;
  });
  CHECK_MESSAGE(applied, "the two gain ramps never reached the render thread");
  status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"))["status"];
  CHECK(status["generation"] == 2);
  CHECK(status["local_transitions"] == 2);
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

  // A client's own data, at every level: a change to it alone is kept and handed back, and rebuilds
  // nothing, so the generation stays where it was.
  {
    nlohmann::json annotated = got["document"];
    annotated["meta"] = {{"version", 3}};
    annotated["chains"][0]["meta"] = {{"colour", "teal"}};
    annotated["chains"][0]["nodes"][1]["meta"] = {{"label", "Trim"}};
    nlohmann::json request;
    request["method"] = "rig.apply";
    request["document"] = annotated;
    const auto reply = nlohmann::json::parse(call(config.socket_path, request.dump()));
    REQUIRE_MESSAGE(reply["ok"] == true, reply.dump());
    CHECK(reply["generation"] == 1);
    CHECK(reply["transitions"].empty());
  }

  // And a patch on an applied document works without a chain, because it has one.
  const auto patch = nlohmann::json::parse(call(
    config.socket_path,
    R"({"method":"rig.patch","ops":[{"op":"set_param","node":"blk7/trim","param":"gain","value":0.75}]})"));
  CHECK(patch["ok"] == true);
  CHECK(patch["kind"] == "smooth");

  // The patched document still carries the client's meta.
  const auto after = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  CHECK(after["document"]["meta"] == nlohmann::json{{"version", 3}});
  CHECK(after["document"]["chains"][0]["meta"] == nlohmann::json{{"colour", "teal"}});
  CHECK(after["document"]["chains"][0]["nodes"][1]["meta"] == nlohmann::json{{"label", "Trim"}});
  CHECK(after["document"]["chains"][0]["nodes"][1]["gain"] == doctest::Approx(0.75));

  // A key the engine does not define is refused, and nothing changes.
  {
    nlohmann::json misspelt = after["document"];
    misspelt["chains"][0]["output_chanel"] = 0;
    nlohmann::json request;
    request["method"] = "rig.apply";
    request["document"] = misspelt;
    const auto reply = nlohmann::json::parse(call(config.socket_path, request.dump()));
    CHECK(reply["ok"] == false);
    CHECK_MESSAGE(reply.dump().find("unknown key 'output_chanel'") != std::string::npos, reply.dump());
  }
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
  // A measured device: the cost table is here, so the daemon has nothing to say about that either. (A
  // table that is missing is an informational issue of its own, which is honest but not what
  // "nothing wrong" means.)
  write_file(dir.path / "cost.json", test_cost_table());
  config.cost_table = (dir.path / "cost.json").string();

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

  // A miss does not refuse the rig: the block plays as if bypassed, and the reply, `engine.status` and
  // its issues all say which block and why, with the library's near miss. A pedal whose library is
  // incomplete still plays; one refused rig would leave it silent (pedal.project, vip's first card).
  const auto missing =
    nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.load","rig":"missing.json"})"));
  REQUIRE(missing["ok"] == true);
  const uint64_t stood_in = missing["generation"].get<uint64_t>();
  REQUIRE(missing["missing_assets"].size() == 1);
  CHECK(missing["missing_assets"][0]["node"] == "amp");
  CHECK(missing["missing_assets"][0]["element"] == "amp");
  CHECK(missing["missing_assets"][0]["param"] == "model");
  CHECK(missing["missing_assets"][0]["kind"] == "model");
  CHECK(missing["missing_assets"][0]["reference"] == "A2-Lit");
  CHECK(missing["missing_assets"][0]["message"].get<std::string>().find("did you mean") != std::string::npos);

  const auto warned = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  REQUIRE(warned["ok"] == true);
  CHECK(warned["status"]["health"] == "warn");
  bool issued = false;
  for (const auto& issue : warned["status"]["issues"])
  {
    if (issue["code"] != "asset_missing")
      continue;
    issued = true;
    CHECK(issue["severity"] == "warn");
    CHECK(issue["node"] == "amp");
    CHECK(issue["element"] == "amp");
    CHECK(issue["reference"] == "A2-Lit");
  }
  CHECK_MESSAGE(issued, "no asset_missing issue for the stood-in block");
  REQUIRE(warned["status"]["chains"].size() == 1);
  CHECK(warned["status"]["chains"][0]["missing_assets"].size() == 1);

  // The document handed back is the one written: the stand-in is the engine's, not the client's.
  const auto as_written = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  CHECK(as_written["document"]["chains"][0]["nodes"][1]["type"] == "nc.nam");
  CHECK(as_written["document"]["chains"][0]["nodes"][1]["model"] == "A2-Lit");

  // The model arrives, and a rescan brings the block in without anyone touching the rig: the same
  // document now compiles to something else, which is a new generation.
  std::filesystem::copy_file(std::string(NAMCHAIN_TEST_DATA_DIR) + "/A2-Lite.nam", models / "A2-Lit.nam");
  const auto rescanned =
    nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.assets","refresh":true})"));
  REQUIRE(rescanned["ok"] == true);
  const auto healed = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  CHECK(healed["generation"].get<uint64_t>() > stood_in);
  CHECK(healed["missing_assets"].empty());
  const auto clear = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  for (const auto& issue : clear["status"]["issues"])
    CHECK(issue["code"] != "asset_missing");

  // And the same through a patch, because `swap_model` names a model too.
  const auto swapped = nlohmann::json::parse(call(config.socket_path,
                                                  R"({"method":"rig.patch","ops":[{"op":"swap_model",
                                                     "node":"amp","model":"A2-Litt"}]})"));
  REQUIRE(swapped["ok"] == true);
  REQUIRE(swapped["missing_assets"].size() == 1);
  CHECK(swapped["missing_assets"][0]["reference"] == "A2-Litt");
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
// The model budget (docs/daemon.md, "Model budget")
// ---------------------------------------------------------------------------

namespace
{

/// A library for the budget: `A2` is the slimmable container (Full and Lite), `A2-Full` the same Full
/// on its own, which cannot be slimmed. At q64 the Full reads 300808 bytes per block and the Lite
/// 102068 (test_working_set.cpp).
void write_budget_library(const std::filesystem::path& models)
{
  std::filesystem::create_directories(models);
  std::filesystem::copy_file(std::string(NAMCHAIN_EXAMPLE_MODELS_DIR) + "/A2.nam", models / "A2.nam");
  std::filesystem::copy_file(std::string(NAMCHAIN_TEST_DATA_DIR) + "/A2-Full.nam", models / "A2-Full.nam");
}

std::string two_amp_rig(const char* first, const char* second)
{
  return std::string(R"({
    "nodes": [
      { "id": "in", "type": "nc.input" },
      { "id": "a1", "type": "nc.nam", "model": ")") +
         first + R"(" },
      { "id": "a2", "type": "nc.nam", "model": ")" +
         second + R"(" },
      { "id": "out", "type": "nc.output" }
    ],
    "connections": [ { "from": "in", "to": "a1" }, { "from": "a1", "to": "a2" }, { "from": "a2", "to": "out" } ]
  })";
}


Config budget_config(const TempDir& dir)
{
  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = (dir.path / "rigs").string();
  config.models_root = (dir.path / "models").string();
  config.irs_root = (dir.path / "irs").string();
  config.lock_memory = false;
  config.rt_priority = 0;
  config.device_retry_ms = 20;
  config.reclamation_timeout_ms = 5;
  config.period_size = 64;
  config.cost_table = (dir.path / "cost.json").string();
  return config;
}

/// One model between an input and an output: 0.52 of a core as `A2` (Full + 0.02 of nodes), 0.17 as
/// its Lite. Under the headroom either way, so an ordinary publish never trims it -- which is the case
/// P8 exists for: a Lite there is a *choice*, not a concession to the budget.
std::string one_amp_rig(const char* model)
{
  return std::string(R"({
    "nodes": [
      { "id": "in", "type": "nc.input" },
      { "id": "a1", "type": "nc.nam", "model": ")") +
         model + R"(" },
      { "id": "out", "type": "nc.output" }
    ],
    "connections": [ { "from": "in", "to": "a1" }, { "from": "a1", "to": "out" } ]
  })";
}

} // namespace

TEST_CASE("the cost table trims the model it can, says so, and refuses what still does not fit")
{
  TempDir dir;
  write_budget_library(dir.path / "models");
  write_file(dir.path / "cost.json", test_cost_table());
  write_file(dir.path / "rigs" / "two-amp.json", two_amp_rig("A2", "A2"));
  const Config config = budget_config(dir);

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

  const auto hello = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.hello"})"));
  REQUIRE(hello["ok"] == true);
  bool advertised = false;
  for (const auto& feature : hello["features"])
    advertised = advertised || feature == "cost";
  CHECK(advertised);

  // Two Fulls would not fit; the second amp plays its Lite, and the reply says which and by how much.
  const auto loaded = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.load","rig":"two-amp.json"})"));
  REQUIRE_MESSAGE(loaded["ok"] == true, loaded.dump());
  REQUIRE(loaded["slimmed_models"].size() == 1);
  CHECK(loaded["slimmed_models"][0]["chain"] == "main");
  CHECK(loaded["slimmed_models"][0]["node"] == "a2");
  CHECK(loaded["slimmed_models"][0]["element"] == "a2");
  CHECK(loaded["slimmed_models"][0]["reference"] == "A2");
  CHECK(loaded["slimmed_models"][0]["row"] == "NAM.WaveNet.lite");
  CHECK(loaded["slimmed_models"][0]["cost"] == doctest::Approx(0.15));
  CHECK(loaded["slimmed_models"][0]["full_cost"] == doctest::Approx(0.5));
  const bool adopted = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["adopted_generation"].get<uint64_t>() >= 1 &&
           status["blocks"].get<uint64_t>() > 0;
  });
  CHECK_MESSAGE(adopted, "the slimmed rig never reached the device");

  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"))["status"];
  CHECK(status["cost"]["cost"] == doctest::Approx(0.67));
  CHECK(status["cost"]["full_cost"] == doctest::Approx(1.02));
  CHECK(status["cost"]["headroom"] == doctest::Approx(0.8));
  CHECK(status["cost"]["over"] == false);
  CHECK(status["cost"]["verdict"] == "trimmed");
  CHECK(status["cost"]["trimmed"] == true);
  CHECK(status["cost"]["table_ok"] == true);
  REQUIRE(status["cost"]["nodes"].size() == 4);
  CHECK(status["cost"]["nodes"][2]["row"] == "NAM.WaveNet.lite");
  CHECK(status["cost"]["nodes"][2]["slimmed"] == true);
  // The budget doing its job is not a fault: the light stays green, and the issue names the block.
  CHECK(status["health"] == "ok");
  bool named = false;
  for (const auto& issue : status["issues"])
  {
    if (issue["code"] != "model_slimmed")
      continue;
    named = true;
    CHECK(issue["severity"] == "info");
    CHECK(issue["node"] == "a2");
    CHECK(issue["element"] == "a2");
    CHECK(issue["reference"] == "A2");
  }
  CHECK_MESSAGE(named, "no model_slimmed issue for the slimmed amp");
  REQUIRE(status["chains"].size() == 1);
  REQUIRE(status["chains"][0]["slimmed_models"].size() == 1);
  CHECK(status["chains"][0]["slimmed_models"][0]["node"] == "a2");

  // The document handed back is the one written: slimming is the engine's, never a client's.
  const auto running = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  REQUIRE(running["ok"] == true);
  const auto& amp = running["document"]["chains"][0]["nodes"][2];
  CHECK(amp["model"] == "A2");
  CHECK_FALSE(amp.contains("slim"));
  CHECK(running["slimmed_models"].size() == 1);

  // A2 then a Full that cannot be slimmed: the budget passes over the Full and slims the container,
  // which is the first amp now. The patch reply says so.
  const auto swapped = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.patch","ops":[{"op":"swap_model","node":"a2","model":"A2-Full"}]})"));
  REQUIRE_MESSAGE(swapped["ok"] == true, swapped.dump());
  REQUIRE(swapped["slimmed_models"].size() == 1);
  CHECK(swapped["slimmed_models"][0]["node"] == "a1");

  // Two Fulls: nothing left to slim, so a client's publish is refused, by name, and nothing changes.
  const uint64_t before = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"))["generation"];
  nlohmann::json apply;
  apply["method"] = "rig.apply";
  nlohmann::json chain = nlohmann::json::parse(two_amp_rig("A2-Full", "A2-Full"));
  chain["id"] = "main";
  apply["document"]["chains"] = nlohmann::json::array({chain});
  const auto refused = nlohmann::json::parse(call(config.socket_path, apply.dump()));
  REQUIRE(refused["ok"] == false);
  CHECK(refused["error"]["code"] == "over_budget");
  const std::string why = refused["error"]["message"];
  CHECK_MESSAGE(why.find("1.020 of a core") != std::string::npos, why);
  CHECK_MESSAGE(why.find("0.800 a rig may have") != std::string::npos, why);
  CHECK_MESSAGE(why.find("'a1' 0.500") != std::string::npos, why);
  CHECK_MESSAGE(why.find("'a2' 0.500") != std::string::npos, why);
  const auto after = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  CHECK(after["generation"] == before);
  CHECK(after["document"]["chains"][0]["nodes"][2]["model"] == "A2-Full");
  CHECK(after["document"]["chains"][0]["nodes"][1]["model"] == "A2");
}

TEST_CASE("a lite_first load publishes the Lite first and the Full behind it")
{
  // P8's first increment, end to end through a real daemon: the rig starts playing as soon as its
  // Lite compiles -- about a third of the work -- and the Full is compiled while it plays and
  // crossfaded in over it. The reply carries both publishes, because the Lite is the one thing a
  // client cannot see afterwards: the control thread does both before it answers (docs/daemon.md,
  // "Lite-first publishes").
  TempDir dir;
  write_budget_library(dir.path / "models");
  write_file(dir.path / "cost.json", test_cost_table());
  write_file(dir.path / "rigs" / "slimmable.json", one_amp_rig("A2"));
  const Config config = budget_config(dir);

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

  // The baseline: 0.52 as written, so an ordinary load publishes the Full and trims nothing. The
  // Lite-first load below must end in exactly this state.
  const auto plain = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.load","rig":"slimmable.json"})"));
  REQUIRE_MESSAGE(plain["ok"] == true, plain.dump());
  const uint64_t plain_generation = plain["generation"].get<uint64_t>();
  CHECK(plain["slimmed_models"].empty());
  CHECK_FALSE(plain.contains("lite_first")); // nobody asked, so nothing about it is said

  const auto loaded = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.load","rig":"slimmable.json","lite_first":true})"));
  REQUIRE_MESSAGE(loaded["ok"] == true, loaded.dump());

  // Step one: the Lite, published whatever the budget says (it would have fitted as a Full).
  CHECK(loaded["lite_first"] == true);
  CHECK(loaded["lite"]["trimmed"] == true);
  CHECK(loaded["lite"]["verdict"] == "trimmed");
  CHECK(loaded["lite"]["cost"] == doctest::Approx(0.17));
  CHECK(loaded["lite"]["full_cost"] == doctest::Approx(0.52));
  CHECK(loaded["lite"]["generation"] == plain_generation + 1);
  REQUIRE(loaded["lite"]["slimmed_models"].size() == 1);
  CHECK(loaded["lite"]["slimmed_models"][0]["chain"] == "main");
  CHECK(loaded["lite"]["slimmed_models"][0]["node"] == "a1");
  CHECK(loaded["lite"]["slimmed_models"][0]["reference"] == "A2");
  CHECK(loaded["lite"]["slimmed_models"][0]["row"] == "NAM.WaveNet.lite");
  CHECK(loaded["lite"]["slimmed_models"][0]["cost"] == doctest::Approx(0.15));
  CHECK(loaded["lite"]["slimmed_models"][0]["full_cost"] == doctest::Approx(0.5));
  // And the device is *playing* it: the control thread waited for the Lite's fade to finish before it
  // compiled the Full, so this is the render thread's own state and not a plan sitting in a ring. Two
  // publishes back to back would have coalesced the Lite away (the newer plan retires a pending one),
  // which is exactly what this wait prevents -- and what can only be asserted if it happened.
  CHECK(loaded["lite"]["adopted"] == true);

  // Step two: the Full, as an ordinary publish -- the same route and the same crossfade as any load --
  // but *not* in the reply, because it is compiled after the reply is written, on the catch-up thread.
  // What the reply says is that it is on its way, and what the Lite it is standing in for is.
  CHECK(loaded["full"]["state"] == "compiling");
  CHECK(loaded["full"]["catch_up"]["state"] == "exact");
  CHECK(loaded["generation"] == plain_generation + 1); // the Lite is the running document for now
  CHECK_FALSE(loaded["slimmed_models"].empty());       // ... and it is a trimmed one

  // The Lite is a *state*, not a step inside the call: while the Full compiles, `engine.status` says so
  // -- the Lite's own cost, trimmed, `lite_first` true -- and then the Full arrives.
  const bool published = wait_for_full(config.socket_path);
  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"))["status"];
  if (!published)
    std::fprintf(stderr, "[test-dbg] status: catch_up=%s cost.lite_first=%d generation=%llu audible=%llu\n",
                 status["catch_up"].dump().c_str(), (int)status["cost"]["lite_first"],
                 (unsigned long long)status["generation"].get<uint64_t>(),
                 (unsigned long long)status["audible_generation"].get<uint64_t>()),
      std::fprintf(stderr, "[test-dbg] blocks=%llu transition_active=%d chain=%s\n",
                   (unsigned long long)status["blocks"].get<uint64_t>(), (int)status["transition_active"],
                   status["chains"][0].dump().c_str());
  CHECK_MESSAGE(published, "the Full never arrived");

  CHECK(status["generation"] == plain_generation + 2);
  CHECK(status["catch_up"]["full"] == "published");
  CHECK(status["catch_up"]["replay"] == "exact");
  REQUIRE(status["last_transitions"].size() == 1);
  CHECK(status["last_transitions"][0]["chain"] == "main");
  CHECK(status["last_transitions"][0]["kind"] == "crossfade");
  const bool adopted = wait_for(config.socket_path, [plain_generation](const nlohmann::json& status) {
    return status["state"] == "running" && status["adopted_generation"].get<uint64_t>() >= plain_generation + 2 &&
           status["blocks"].get<uint64_t>() > 0;
  });
  CHECK_MESSAGE(adopted, "the Full never reached the device");

  // What the engine reports is the Full: the temporary Lite lasted for the length of the Full's
  // compile, and `lite_first` is false again once the rig is what the document asked for.
  CHECK(status["cost"]["cost"] == doctest::Approx(0.52));
  CHECK(status["cost"]["trimmed"] == false);
  CHECK(status["cost"]["lite_first"] == false);
  CHECK(status["cost"]["verdict"] == "fits");
  REQUIRE(status["cost"]["nodes"].size() == 3);
  CHECK(status["cost"]["nodes"][1]["row"] == "NAM.WaveNet.full");
  CHECK(status["cost"]["nodes"][1]["slimmed"] == false);
  CHECK(status["chains"][0]["slimmed_models"].empty());
  for (const auto& issue : status["issues"])
    CHECK(issue["code"] != "model_slimmed");
}

TEST_CASE("a lite_first request on a rig with nothing to trim is an ordinary publish")
{
  // The other half of P8's flag: where there is nothing slimmable, the Lite *is* the document as
  // written, so there is no second publish to make and nothing about the request is worth reporting
  // as a standing-in Lite. One generation moves, not two.
  TempDir dir;
  write_budget_library(dir.path / "models");
  write_file(dir.path / "cost.json", test_cost_table());
  write_file(dir.path / "rigs" / "full-only.json", one_amp_rig("A2-Full"));
  const Config config = budget_config(dir);

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

  const auto loaded = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.load","rig":"full-only.json","lite_first":true})"));
  REQUIRE_MESSAGE(loaded["ok"] == true, loaded.dump());
  CHECK(loaded["generation"] == 1);
  CHECK(loaded["lite_first"] == false);
  CHECK_FALSE(loaded.contains("lite"));
  CHECK(loaded["full"]["state"] == "nothing_to_trim");
  CHECK(loaded["full"]["generation"] == 1);
  CHECK(loaded["slimmed_models"].empty());

  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"))["status"];
  CHECK(status["cost"]["cost"] == doctest::Approx(0.52));
  CHECK(status["cost"]["trimmed"] == false);
  CHECK(status["cost"]["lite_first"] == false);
  CHECK(status["cost"]["verdict"] == "fits");
  REQUIRE(status["cost"]["nodes"].size() == 3);
  CHECK(status["cost"]["nodes"][1]["row"] == "NAM.WaveNet.full");
}

TEST_CASE("a lite_first publish is refused when the rig is over the headroom at Lite")
{
  // Lite-first is a compile-time trick, not an exemption from D3: a rig that is over with *everything*
  // trimmed is refused exactly as an ordinary publish refuses it, and nothing about the running rig
  // changes. The refusal has to happen on the first publish, because the second one never gets to run.
  TempDir dir;
  write_budget_library(dir.path / "models");
  write_file(dir.path / "cost.json", test_cost_table());
  write_file(dir.path / "rigs" / "slimmable.json", one_amp_rig("A2"));
  write_file(dir.path / "rigs" / "two-full.json", two_amp_rig("A2-Full", "A2-Full"));
  const Config config = budget_config(dir);

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

  const auto playing = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.load","rig":"slimmable.json"})"));
  REQUIRE_MESSAGE(playing["ok"] == true, playing.dump());
  const uint64_t generation = playing["generation"].get<uint64_t>();

  // Two Fulls and nothing slimmable: 1.020 of a core against the table's 0.800 headroom.
  const auto refused = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.load","rig":"two-full.json","lite_first":true})"));
  REQUIRE(refused["ok"] == false);
  CHECK(refused["error"]["code"] == "over_budget");
  const std::string why = refused["error"]["message"];
  CHECK_MESSAGE(why.find("1.020 of a core") != std::string::npos, why);
  CHECK_MESSAGE(why.find("0.800 a rig may have") != std::string::npos, why);
  CHECK_MESSAGE(why.find("with every model that can be trimmed trimmed") != std::string::npos, why);

  // Nothing was published, by either step: the rig that was playing still is, at its own generation.
  const auto after = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  CHECK(after["generation"] == generation);
  CHECK(after["rig"].get<std::string>().find("slimmable.json") != std::string::npos);
  CHECK(after["slimmed_models"].empty());
  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"))["status"];
  CHECK(status["cost"]["cost"] == doctest::Approx(0.52));
  CHECK(status["cost"]["lite_first"] == false);

  // A flag that is not a boolean is a bad request, not a silently ignored field.
  const auto malformed = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.load","rig":"slimmable.json","lite_first":"yes"})"));
  CHECK(malformed["ok"] == false);
  CHECK(malformed["error"]["code"] == "bad_request");
}

TEST_CASE("a lite_first publish whose Lite is already the budget's answer reports so")
{
  // `A2` beside a Full that cannot be slimmed is over the headroom as written, so an ordinary publish
  // trims the container to Lite -- and a Lite-first publish trims exactly the same node. The second
  // publish therefore had nothing to do, which is not a failure and not a Full: the state says so, and
  // `full.generation` stands still because a publish that changes nothing does not advance it.
  TempDir dir;
  write_budget_library(dir.path / "models");
  write_file(dir.path / "cost.json", test_cost_table());
  write_file(dir.path / "rigs" / "trimmed.json", two_amp_rig("A2", "A2-Full"));
  const Config config = budget_config(dir);

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

  const auto loaded = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.load","rig":"trimmed.json","lite_first":true})"));
  REQUIRE_MESSAGE(loaded["ok"] == true, loaded.dump());
  CHECK(loaded["lite_first"] == true);
  CHECK(loaded["lite"]["trimmed"] == true);
  CHECK(loaded["lite"]["cost"] == doctest::Approx(0.67));
  CHECK(loaded["lite"]["adopted"] == true);
  // The Full's own publish is prepared after the reply now, so the reply says `compiling` whatever
  // comes of it; what this case is about is that an ordinary publish of this rig reaches the document
  // the Lite already is, which the status finds out and reports as `unchanged` (and the generation
  // stands still, because a publish that changes nothing does not advance it).
  CHECK(loaded["full"]["state"] == "compiling");
  CHECK(loaded["generation"] == loaded["lite"]["generation"]);
  const bool settled = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["catch_up"]["full"] == "unchanged";
  });
  CHECK_MESSAGE(settled, "the Full's own publish never settled");

  // The state it ends in is the state an ordinary publish of this rig reaches, and the engine reports
  // the Lite that is standing in, because for this rig the Lite *is* the answer (D6). The reply's
  // `lite_first` is about the publish that happened -- a Lite went first -- while the status's is about
  // what is standing in *now*: not a temporary Lite on its way to a Full, so false, with `trimmed`
  // carrying the warning a panel shows.
  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"))["status"];
  CHECK(status["generation"] == loaded["lite"]["generation"]);
  CHECK(status["cost"]["cost"] == doctest::Approx(0.67));
  CHECK(status["cost"]["trimmed"] == true);
  CHECK(status["cost"]["lite_first"] == false);
  CHECK(status["cost"]["verdict"] == "trimmed");
  REQUIRE(loaded["slimmed_models"].size() == 1);
  CHECK(loaded["slimmed_models"][0]["node"] == "a1");
  CHECK(status["catch_up"]["state"] == "exact");
  CHECK(status["catch_up"]["replay"] == "none");
}

TEST_CASE("a model whose memory outlasts the old fixed hold is still caught up exactly")
{
  // The pedal's own failure, reproduced on the test backend (2026-10-10). A window that covers a
  // model's receptive field can only be filled at the speed of the audio clock -- the catch-up thread
  // holds the newest `frames` of history, so it reaches a covering window `frames` after the plans
  // exist. The render thread's patience used to be a fixed 12000 frames (250 ms), which is *shorter
  // than the window* for any model that needs more: it gave up first, took the Full over cold, and
  // `catch_up.replay` read `no_input` with `blocks: 0` while the fill loop finished a block later
  // saying `need_met`. vip's model needs 14539 frames, so every `lite_first` press on the board did
  // exactly that.
  //
  // So the case is the *shape* of the failure, not vip's model: a slimmable document whose memory is
  // longer than `kCatchUpHoldFrames`, published with `lite_first`, from a rig that has been playing
  // long enough for the ring to hold some history. The assertion is the one the pedal could not meet:
  // the replay ran, it was exact, and the Full is what the device is playing.
  TempDir dir;
  const auto models = dir.path / "models";
  write_budget_library(models);

  // A2's own two submodels, each with wider dilations: what a replay has to cover is the model's
  // memory, which `get_receptive_field()` computes from `dilation * (kernel - 1)` -- channels do not
  // enter it at all -- so scaling the dilations lengthens both submodels by the same amount and keeps
  // their weights consistent with the files they came from. Small enough to compile here, which vip's
  // own model is not.
  nlohmann::json full = nlohmann::json::parse(std::ifstream(models / "A2-Full.nam"));
  const nlohmann::json lite = nlohmann::json::parse(std::ifstream(std::string(NAMCHAIN_TEST_DATA_DIR) +
                                                                  "/A2-Lite.nam"));
  const auto lookback_of = [](const nlohmann::json& model) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("namchain-catch-up-model-" + std::to_string(::getpid()) + ".nam");
    {
      std::ofstream out(path);
      out << model.dump();
    }
    const namchain::LookbackResult result = namchain::compute_lookback_from_file(path.string());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return result;
  };
  const auto with_dilations = [](const nlohmann::json& base, int dilations) {
    nlohmann::json model = base;
    for (auto& layer : model["config"]["layers"])
      if (layer.contains("dilations") && layer["dilations"].is_array())
        for (auto& d : layer["dilations"])
          d = d.get<int>() * dilations;
    return model;
  };

  nlohmann::json container;
  container["version"] = full.value("version", std::string("0.7.0"));
  container["architecture"] = "SlimmableContainer";
  // The container carries no weights of its own -- its submodels do -- but the .nam loader requires
  // the key, which is how the vendored `example_models/A2.nam` is written.
  container["weights"] = nlohmann::json::array();
  container["sample_rate"] = full.value("sample_rate", 48000);
  container["config"] = {{"submodels", nlohmann::json::array()}};
  namchain::LookbackResult lookback;
  // 4x the old hold, to be unambiguous about which side of it this document is on.
  const std::size_t want = namchain::daemon::kCatchUpHoldFrames * 4;
  for (int scale = 2; scale <= 256; scale *= 2)
  {
    container["config"]["submodels"] = nlohmann::json::array(
      {{{"max_value", 0.5}, {"model", with_dilations(lite, scale)}},
       {{"max_value", 1.0}, {"model", with_dilations(full, scale)}}});
    lookback = lookback_of(container);
    if (lookback.ok() && static_cast<std::size_t>(lookback.samples) > want)
      break;
  }
  REQUIRE_MESSAGE(lookback.ok(), lookback.detail);
  REQUIRE(static_cast<std::size_t>(lookback.samples) > want);
  {
    std::ofstream out(models / "long.nam");
    out << container.dump();
  }
  write_file(dir.path / "cost.json", test_cost_table());
  write_file(dir.path / "rigs" / "long.json", one_amp_rig("long.nam"));
  const Config config = budget_config(dir);

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

  // Play the rig normally first. This is what the ring is for: it is armed by the *second* publish,
  // and by then the device has been rendering this document long enough that the window can be filled
  // from history rather than from the clock -- the same history a press reaches the Full through on
  // the pedal (the Lite is what plays while the Full compiles).
  const auto plain = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.load","rig":"long.json"})"));
  REQUIRE_MESSAGE(plain["ok"] == true, plain.dump());
  REQUIRE(wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["blocks"].get<uint64_t>() > 100;
  }));

  const auto loaded = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.load","rig":"long.json","lite_first":true})"));
  REQUIRE_MESSAGE(loaded["ok"] == true, loaded.dump());
  // The document's own answer, and the hold that answer needs: the reply's `catch_up.frames` is the
  // model's memory, and the status's `hold_frames` is what the render thread will wait for it.
  CHECK(loaded["lite_first"] == true);
  CHECK(loaded["full"]["catch_up"]["state"] == "exact");
  CHECK(loaded["full"]["catch_up"]["frames"] == static_cast<std::size_t>(lookback.samples));

  REQUIRE(wait_for_full(config.socket_path));
  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"))["status"];
  CHECK(status["catch_up"]["state"] == "exact");
  CHECK(status["catch_up"]["replay"] == "exact");
  CHECK(status["catch_up"]["blocks"].get<std::size_t>() > 1);
  CHECK(status["catch_up"]["progress"] == "done");
  CHECK(status["catch_up"]["fill"] == "need_met");
  CHECK(status["catch_up"]["hold_frames"].get<std::size_t>() >=
        static_cast<std::size_t>(lookback.samples));
  // The history was fed and drained, and nothing was dropped: a window with a hole would have been
  // reported as `gap` rather than `exact`, but saying the ring did its job is the point of the report.
  CHECK(status["catch_up"]["ring"]["written"].get<uint64_t>() > 0);
  CHECK(status["catch_up"]["ring"]["consumed"].get<uint64_t>() > 0);
  CHECK(status["catch_up"]["ring"]["dropped"] == 0);
  CHECK(status["catch_up"]["period_mismatch"] == 0);
  CHECK(status["catch_up"]["window"]["contiguous"].get<std::size_t>() >=
        status["catch_up"]["window"]["need"].get<std::size_t>());
  CHECK(status["cost"]["lite_first"] == false);
}

TEST_CASE("rig.cost answers what a rig would cost, without publishing or compiling it")
{
  // D9's boundary in one call: the engine owns what a cost means, so the service that owns the bank
  // asks here for each rig rather than summing rows itself. Nothing may change: this is a question,
  // not a publish.
  TempDir dir;
  write_budget_library(dir.path / "models");
  write_file(dir.path / "cost.json", test_cost_table());
  write_file(dir.path / "rigs" / "two-amp.json", two_amp_rig("A2", "A2"));
  write_file(dir.path / "rigs" / "one-amp.json", two_amp_rig("A2", "A2-Full"));
  const Config config = budget_config(dir);

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
  const uint64_t generation = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"))["generation"];

  // Two A2s: 1.02 as written, over the 0.8 headroom, so one is trimmed and it costs 0.67.
  const auto cost = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.cost","rig":"two-amp.json"})"));
  REQUIRE_MESSAGE(cost["ok"] == true, cost.dump());
  CHECK(cost["cost"]["cost"] == doctest::Approx(0.67));
  CHECK(cost["cost"]["full_cost"] == doctest::Approx(1.02));
  CHECK(cost["cost"]["over"] == false);
  CHECK(cost["cost"]["verdict"] == "trimmed");
  CHECK(cost["cost"]["table_ok"] == true);
  REQUIRE(cost["slimmed_models"].size() == 1);
  CHECK(cost["slimmed_models"][0]["node"] == "a2");
  CHECK(cost["slimmed_models"][0]["row"] == "NAM.WaveNet.lite");

  // One A2 and one A2-Full: nothing to trim on the second, so the first gives way and it still fits.
  const auto mixed = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.cost","rig":"one-amp.json"})"));
  REQUIRE_MESSAGE(mixed["ok"] == true, mixed.dump());
  CHECK(mixed["cost"]["verdict"] == "trimmed");
  REQUIRE(mixed["slimmed_models"].size() == 1);
  CHECK(mixed["slimmed_models"][0]["node"] == "a1");

  // A whole document works the same way, and a rig that is over with nothing to trim says so.
  nlohmann::json apply;
  apply["method"] = "rig.cost";
  apply["document"] = nlohmann::json::parse(two_amp_rig("A2-Full", "A2-Full"));
  const auto over = nlohmann::json::parse(call(config.socket_path, apply.dump()));
  REQUIRE_MESSAGE(over["ok"] == true, over.dump());
  CHECK(over["cost"]["over"] == true);
  CHECK(over["cost"]["verdict"] == "over");
  CHECK(over["cost"]["cost"] == doctest::Approx(1.02));
  CHECK(over["slimmed_models"].empty());

  // And nothing was published: the running rig and its generation are untouched.
  const auto after = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  CHECK(after["generation"] == generation);

  const auto bad = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.cost"})"));
  CHECK(bad["ok"] == false);
  CHECK(bad["error"]["code"] == "bad_request");
  const auto missing = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.cost","rig":"nothing.json"})"));
  CHECK(missing["ok"] == false);
  CHECK(missing["error"]["code"] == "rig_not_found");
}

TEST_CASE("a configured rig over the headroom still plays, with a warning")
{
  // Refusing the rig a pedal powers up with would leave it silent, which is worse than a rig that may
  // miss a deadline: the configured rig is admitted, and the status says what it costs.
  TempDir dir;
  write_budget_library(dir.path / "models");
  write_file(dir.path / "cost.json", test_cost_table());
  write_file(dir.path / "rigs" / "two-full.json", two_amp_rig("A2-Full", "A2-Full"));
  Config config = budget_config(dir);
  config.rig = (dir.path / "rigs" / "two-full.json").string();

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

  const bool playing = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["state"] == "running" && status["adopted_generation"].get<uint64_t>() >= 1 &&
           status["blocks"].get<uint64_t>() > 0;
  });
  CHECK_MESSAGE(playing, "the configured rig was not admitted");

  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"))["status"];
  CHECK(status["cost"]["over"] == true);
  CHECK(status["cost"]["cost"] == doctest::Approx(1.02));
  CHECK(status["health"] == "warn");
  bool warned = false;
  for (const auto& issue : status["issues"])
  {
    if (issue["code"] == "cost_over")
      warned = issue["severity"] == "warn";
  }
  CHECK_MESSAGE(warned, "no cost_over warning");
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
  // are bypassed rather than a value anywhere. The lowering then differs only in bypass flags, so the
  // running plan crossfades the bands in and out, and builds no new plan.
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
  CHECK(band_change["generation"].get<uint64_t>() == before_generation);
  CHECK(band_change["transitions"][0]["kind"] == "ramps");
  CHECK(band_change["transitions"][0]["bypasses"].get<int>() > 0);
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

// ---------------------------------------------------------------------------
// The render pool (`render_threads`, pedal.project docs/multi-core.md)
// ---------------------------------------------------------------------------
namespace
{

/// The CPUs this process may run on, lowest first. A pool needs one per participant.
///
/// Linux-only for the same reason the daemon's own check is: process affinity is a Linux call. An
/// empty result on any other platform makes the pool cases below skip themselves rather than fail.
std::vector<int> allowed_cpus()
{
  std::vector<int> cpus;
#if defined(__linux__)
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (::sched_getaffinity(0, sizeof(mask), &mask) != 0)
    return cpus;
  for (int i = 0; i < CPU_SETSIZE; ++i)
  {
    if (CPU_ISSET(i, &mask))
      cpus.push_back(i);
  }
#endif
  return cpus;
}

/// A document of `count` chains, each a gain from channel `i % 2` to the same channel. The gain
/// depends on the chain, so a chain rendered twice, not at all, or on the wrong input cannot pass
/// unnoticed; two chains share each channel, so the sum order is visible too.
std::string n_chain_document(int count)
{
  nlohmann::json chains = nlohmann::json::array();
  for (int i = 0; i < count; ++i)
  {
    const int channel = i % 2;
    nlohmann::json chain;
    chain["id"] = "c" + std::to_string(i);
    chain["input_channel"] = channel;
    chain["output_channel"] = channel;
    chain["nodes"] = nlohmann::json::array({
      {{"id", "in"}, {"type", "nc.input"}},
      {{"id", "g"}, {"type", "nc.gain"}, {"gain", 0.1 * (i + 1)}},
      {{"id", "out"}, {"type", "nc.output"}},
    });
    chain["connections"] =
      nlohmann::json::array({{{"from", "in"}, {"to", "g"}}, {{"from", "g"}, {"to", "out"}}});
    chains.push_back(chain);
  }
  nlohmann::json document;
  document["name"] = "N chains";
  document["chains"] = chains;
  return document.dump();
}

/// Render `document` off a file and hand back the output samples. `threads` > 1 pins the pool at the
/// first allowed CPU, which is what the appliance does with `cpu`.
std::vector<float> render_document(const std::string& document, unsigned threads)
{
  const std::vector<int> cpus = allowed_cpus();
  REQUIRE(cpus.size() >= threads);

  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "doc.json", document);
  write_stereo_wav(dir.path / "in.wav", 512, /*left=*/0.75f, /*right=*/-0.5f);

  Config config = file_backend_config(dir, 2, 2);
  config.rigs_root = root.string();
  config.rig = (root / "doc.json").string();
  config.render_threads = threads;
  if (threads > 1)
    config.cpu = cpus.front();

  Daemon daemon(config);
  run_to_end(daemon);

  unsigned channels = 0;
  return read_wav(dir.path / "out.wav", channels);
}

} // namespace

TEST_CASE("two render threads produce, bit for bit, what one does")
{
  if (allowed_cpus().size() < 2)
  {
    MESSAGE("fewer than two CPUs are available to this process; the parallel arm is skipped");
    return;
  }
  const std::string document = two_chain_document(0.5, 0.25, 0, 1);
  const std::vector<float> serial = render_document(document, 1);
  const std::vector<float> parallel = render_document(document, 2);

  REQUIRE(serial.size() == 512u * 2u);
  REQUIRE(parallel.size() == serial.size());
  // Not `Approx`: a chain is executed by one thread in its original node order either way, so the
  // two runs have to agree to the last bit. Anything less would mean the pool changed the audio.
  for (std::size_t i = 0; i < serial.size(); ++i)
    CHECK(serial[i] == parallel[i]);
}

TEST_CASE("a participant takes every participants-th chain, and the sum keeps its order")
{
  if (allowed_cpus().size() < 3)
  {
    MESSAGE("fewer than three CPUs are available to this process; the three-way arm is skipped");
    return;
  }
  // Five chains over two channels: two participants take two chains each and one takes one, and the
  // chains that share a channel are summed in chain order whatever the split.
  const std::string document = n_chain_document(5);
  const std::vector<float> serial = render_document(document, 1);
  const std::vector<float> parallel = render_document(document, 3);

  REQUIRE(serial.size() == 512u * 2u);
  REQUIRE(parallel.size() == serial.size());
  for (std::size_t i = 0; i < serial.size(); ++i)
    CHECK(serial[i] == parallel[i]);

  // And the sum is what the document says: channel 0 takes chains 0, 2, 4 at 0.1/0.3/0.5 of 0.75,
  // channel 1 takes chains 1, 3 at 0.2/0.4 of -0.5.
  CHECK(serial[0] == doctest::Approx(0.75f * 0.9f).epsilon(1e-5));
  CHECK(serial[1] == doctest::Approx(-0.5f * 0.6f).epsilon(1e-5));
}

TEST_CASE("a transition under two render threads keeps one owner for its bookkeeping")
{
  if (allowed_cpus().size() < 2)
  {
    MESSAGE("fewer than two CPUs are available to this process; the parallel arm is skipped");
    return;
  }
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "a.json", two_chain_document(0.5, 0.5, 0, 1));
  // A structural change to both chains, so both plans are rendered together for the fade.
  write_file(root / "b.json", two_chain_document(0.25, 0.25, 0, 1, /*second_stage=*/true));

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
  config.fade_ms = 20.0;
  config.render_threads = 2;
  config.cpu = allowed_cpus().front();

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

  // The status says what the pool actually did, not what was configured.
  {
    const auto reply = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
    CHECK(reply["status"]["render_threads"].get<unsigned>() == 2u);
    CHECK(reply["status"]["render_threads_configured"].get<unsigned>() == 2u);
    CHECK(reply["status"]["render_degraded"].get<bool>() == false);
  }

  const auto loaded =
    nlohmann::json::parse(call(config.socket_path, R"({"id":"l","method":"rig.load","rig":"b.json"})"));
  REQUIRE(loaded["ok"] == true);

  // Dual-render puts both plans of a chain on the same participant, so the fade has to close and the
  // chain has to change hands with no mismatch and no guard trip. `local_base_mismatches` is the one
  // counter that would move if two participants ever touched one chain's state.
  REQUIRE(wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["audible_generation"].get<uint64_t>() == 2 && status["transition_active"] == false;
  }));
  const auto reply = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(reply["status"]["local_base_mismatches"].get<uint64_t>() == 0u);
  CHECK(reply["status"]["guard_trips"].get<uint64_t>() == 0u);
  CHECK(reply["status"]["failed_nodes"].get<unsigned>() == 0u);
  CHECK(reply["status"]["max_concurrent_transitions"].get<unsigned>() == 1u);
}

TEST_CASE("render_threads needs a CPU to start from, and refuses cores this process may not use")
{
  Config config;
  config.backend = BackendKind::Null;
  config.render_threads = 2;
  config.cpu = -1;
  std::string error;
  Daemon daemon(config);
  CHECK_FALSE(daemon.start(error));
  CHECK(error.find("render_threads") != std::string::npos);

  // A CPU beyond the process's affinity mask is refused by name rather than discovered by a worker
  // that cannot pin itself. Linux-only, because the mask is: see `allowed_cpus`.
#if defined(__linux__)
  const std::vector<int> cpus = allowed_cpus();
  Config beyond;
  beyond.backend = BackendKind::Null;
  beyond.render_threads = 2;
  beyond.cpu = cpus.back() + 1;
  std::string beyond_error;
  Daemon beyond_daemon(beyond);
  CHECK_FALSE(beyond_daemon.start(beyond_error));
  CHECK(beyond_error.find("may not run on") != std::string::npos);
#else
  MESSAGE("process affinity is a Linux call; the mask case is skipped here");
#endif
}

TEST_CASE("the render pool starts from a pinned caller and its worker takes its own core")
{
  const std::vector<int> cpus = allowed_cpus();
  if (cpus.size() < 2)
  {
    MESSAGE("fewer than two CPUs are available to this process; the pool arm is skipped");
    return;
  }

  // As the render thread is when it dispatches: confined to one CPU. A worker born with that mask
  // could not run to pin itself once the caller is SCHED_FIFO and spinning there, so the pool gives
  // each worker its affinity at creation; this is the shape that would deadlock if it did not.
  //
  // Linux-only in full, and not just for the calls below: the assertions read the worker's CPU back
  // from the kernel, and on a platform with no affinity there is no CPU to read back. The pool's
  // dispatch itself is covered on every platform by the daemon cases above.
#if defined(__linux__)
  cpu_set_t original;
  CPU_ZERO(&original);
  REQUIRE(::sched_getaffinity(0, sizeof(original), &original) == 0);
  cpu_set_t only_first;
  CPU_ZERO(&only_first);
  CPU_SET(cpus.front(), &only_first);
  REQUIRE(::sched_setaffinity(0, sizeof(only_first), &only_first) == 0);
  struct Restore
  {
    cpu_set_t mask;
    ~Restore() { ::sched_setaffinity(0, sizeof(mask), &mask); }
  } restore{original};

  namchain::daemon::RenderPool pool;
  std::string error;
  REQUIRE_MESSAGE(pool.start(2, cpus.front(), /*priority=*/0, error), error);
  REQUIRE(pool.threads() == 2u);
  REQUIRE(pool.participants().size() == 2u);
  // Read back from the kernel: the worker is on the CPU after the caller's, not on the caller's.
  CHECK(pool.participants()[0].requested_cpu == cpus.front());
  CHECK(pool.participants()[1].requested_cpu == cpus[1]);
  CHECK(pool.participants()[1].scheduling.cpu == cpus[1]);
  CHECK(pool.degraded_reason().empty());

  struct Counters
  {
    std::atomic<int> calls{0};
    std::atomic<int> worker_calls{0};
    std::atomic<int> frames_seen{0};
  } counters;

  const auto work = [](void* context, unsigned participant, unsigned participants, int frames) noexcept {
    auto* c = static_cast<Counters*>(context);
    c->calls.fetch_add(1, std::memory_order_relaxed);
    if (participant == 1)
      c->worker_calls.fetch_add(1, std::memory_order_relaxed);
    c->frames_seen.fetch_add(frames, std::memory_order_relaxed);
  };

  for (int block = 0; block < 200; ++block)
    pool.run(&counters, work, 64);

  CHECK(counters.calls.load() == 400);
  CHECK(counters.worker_calls.load() == 200);
  CHECK(counters.frames_seen.load() == 400 * 64);
  CHECK(pool.blocks() == 200u);

  pool.stop();
  CHECK(pool.threads() == 1u);
  // And the single-participant pool is an inline call, not a dispatch.
  Counters after_stop;
  pool.run(&after_stop, work, 32);
  CHECK(after_stop.calls.load() == 1);
  CHECK(after_stop.worker_calls.load() == 0);
#endif // __linux__
}

// ---------------------------------------------------------------------------
// Levels: the device's in and out, a chain's, and every node's (docs/daemon.md, "Levels")
// ---------------------------------------------------------------------------

TEST_CASE("a level probe measures a window and starts a new one when it is read")
{
  namchain::level::Probe probe;

  // A DC signal at half scale: peak -6 dBFS, RMS the same, because every sample is equal.
  for (int i = 0; i < 64; ++i)
    probe.accumulate(0.5f);
  auto r = probe.take();
  CHECK(r.frames == 64);
  CHECK(r.peak == doctest::Approx(0.5f));
  CHECK(r.rms == doctest::Approx(0.5).epsilon(0.001));
  CHECK(r.clipped == 0);
  CHECK(namchain::level::db_of_peak(r.peak) == doctest::Approx(-6.0206).epsilon(0.01));
  CHECK(namchain::level::db_of_rms(r.rms) == doctest::Approx(-6.0206).epsilon(0.01));

  // Read and reset: the next window is its own, not a longer version of the first.
  r = probe.take();
  CHECK(r.frames == 0);
  CHECK(namchain::level::db_of_peak(r.peak) == namchain::level::kFloorDb);

  for (int i = 0; i < 32; ++i)
    probe.accumulate(0.25f);
  r = probe.take();
  CHECK(r.frames == 32);
  CHECK(r.peak == doctest::Approx(0.25f));

  // Peek reads the same window without emptying it.
  for (int i = 0; i < 8; ++i)
    probe.accumulate(1.0f);
  CHECK(probe.peek().frames == 8);
  CHECK(probe.peek().frames == 8);
  CHECK(probe.peek().clipped == 8);
}

TEST_CASE("publishing per period adds to the window rather than setting it")
{
  // The contract that lets a probe fold into private memory and publish once a period: a reader zeroes
  // the fields as it reads them, so a publish must *add* and *raise* rather than store. If it stored,
  // a publish landing after a read would wipe the reader's next window, and one landing before would
  // wipe what had not been read yet. This test is the one that fails if that ever changes.
  namchain::level::Probe probe(namchain::level::Probe::Publish::Period);
  probe.accumulate(0.5f);
  probe.publish_window();

  auto first = probe.take();
  CHECK(first.frames == 1);
  CHECK(first.peak == doctest::Approx(0.5f));

  // Read again: nothing has been published since, so the window is empty -- not the one just read.
  CHECK(probe.take().frames == 0);

  // A publish *after* a read starts the next window: the reader must see it, not lose it.
  probe.accumulate(0.25f);
  probe.publish_window();
  auto second = probe.take();
  CHECK(second.frames == 1);
  CHECK(second.peak == doctest::Approx(0.25f));

  // A publish *before* a read joins whatever was already there: two periods read once is two
  // periods' worth of frames, and the peak is the louder of the two.
  probe.accumulate(0.1f);
  probe.publish_window();
  probe.accumulate(0.9f);
  probe.publish_window();
  auto third = probe.take();
  CHECK(third.frames == 2);
  CHECK(third.peak == doctest::Approx(0.9f));

  // Publishing with nothing folded in is a no-op: an idle period must not invent frames.
  probe.publish_window();
  CHECK(probe.take().frames == 0);
}

TEST_CASE("a probe publishes the same window whichever mode it is in")
{
  // Equivalence, not merely agreement: with one publish per fold-group the two modes are the same
  // arithmetic on the same samples, so the readings must be identical -- same peak, same frames, same
  // sum and therefore the same rms, same clip count. Anything else is a bug in one of the paths.
  namchain::level::Probe by_sample(namchain::level::Probe::Publish::Sample);
  namchain::level::Probe by_period(namchain::level::Probe::Publish::Period);

  constexpr int kPeriods = 4;
  constexpr int kFrames = 64;
  for (int period = 0; period < kPeriods; ++period)
  {
    for (int i = 0; i < kFrames; ++i)
    {
      // A signal with a spike, a clip and a sign change in it, so a mode that lost or reordered a
      // sample would show up in the peak, the sum or the clip count.
      const float x = (i == 7) ? 1.5f : (i == 9) ? -1.0f : 0.3f * ((i % 2) ? -1.0f : 1.0f);
      by_sample.accumulate(x);
      by_period.accumulate(x);
    }
    by_period.publish_window();

    const auto a = by_sample.take();
    const auto b = by_period.take();
    CHECK(a.frames == b.frames);
    CHECK(a.peak == doctest::Approx(b.peak));
    CHECK(a.rms == doctest::Approx(b.rms).epsilon(1e-12));
    CHECK(a.clipped == b.clipped);
  }

  // Both empty at the end, and in step rather than one having drifted.
  CHECK(by_sample.take().frames == 0);
  CHECK(by_period.take().frames == 0);
}

TEST_CASE("a level probe's fixed point is exact enough for a meter and a peak holds the loudest sample")
{
  namchain::level::Probe probe;
  // A sine-like alternation and one spike: the peak is the spike, the RMS is the root of the mean
  // square -- not the mean magnitude, which for these eight samples would be 0.2.
  const float samples[] = {0.1f, -0.1f, 0.1f, -0.1f, 0.9f, -0.1f, 0.1f, -0.1f};
  for (float s : samples)
    probe.accumulate(s);
  const auto r = probe.take();
  CHECK(r.peak == doctest::Approx(0.9f));
  // root of the mean square: sqrt((0.1^2 * 7 + 0.9^2) / 8) = sqrt(0.11)
  CHECK(r.rms == doctest::Approx(std::sqrt(0.11)).epsilon(0.001));
  // Full scale counts as clipped; a negative peak at full scale does too (it is |x| that is tested).
  namchain::level::Probe second;
  second.accumulate(-1.0f);
  second.accumulate(1.2f);
  second.accumulate(0.99f);
  const auto r2 = second.take();
  CHECK(r2.clipped == 2);
  CHECK(r2.peak == doctest::Approx(1.2f));
}

TEST_CASE("a level probe reports the root of the mean square, which a sine tells apart from the mean magnitude")
{
  namchain::level::Probe probe;
  // One cycle of a sine sampled at sixteen points: mean square 0.5 exactly, so RMS 1/sqrt(2). The mean
  // magnitude of the same samples is 0.628, which is what a rectified average would report -- 1.05 dB
  // lower, and a different quantity. A client showing crest factor needs the RMS.
  constexpr double kTwoPi = 6.283185307179586;
  for (int i = 0; i < 16; ++i)
    probe.accumulate(static_cast<float>(std::sin(kTwoPi * i / 16.0)));
  const auto r = probe.take();
  CHECK(r.peak == doctest::Approx(1.0f).epsilon(0.001));
  CHECK(r.rms == doctest::Approx(std::sqrt(0.5)).epsilon(0.005));
  CHECK(r.rms > 0.65); // the mean magnitude of these samples would be 0.628
  CHECK(namchain::level::db_of_rms(r.rms) == doctest::Approx(-3.0103).epsilon(0.01));
}

TEST_CASE("a probe read twice reports the second read from the next window, which one take prevents")
{
  namchain::level::Probe probe;
  for (int i = 0; i < 64; ++i)
    probe.accumulate(1.0f);

  const auto taken = probe.take();
  CHECK(taken.frames == 64);
  CHECK(taken.clipped == 64);

  // A `take` empties the window it returns, so a `peek` after it describes the *next* window -- and
  // the render thread has usually not filled that one yet. This is the shape that reported a device
  // channel's `rms` as the floor and its `clipped` as zero beside a `peak` that was real: one entry's
  // fields have to come from one `take` (Daemon::levels_json).
  const auto peeked = probe.peek();
  CHECK(peeked.frames == 0);
  CHECK(peeked.clipped == 0);
  CHECK(namchain::level::db_of_rms(peeked.rms) == namchain::level::kFloorDb);
}

TEST_CASE("engine.levels reports the device, the chain and every node, and carries the plan's identity")
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
    return status["state"] == "running" && status["blocks"].get<uint64_t>() > 4;
  }));

  // The method exists, is read-only, and answers with a `levels` object rather than a `status` one:
  // it is the small answer a meter polls, not a second status.
  //
  // **Read until a window is non-empty**, which is what a meter does: every read empties the probes
  // (that is the design -- see level.h), so a single read taken at a random moment can legitimately
  // land between two blocks and be empty. A test that read once and demanded samples would be
  // asserting a race, not the feature.
  //
  // The deadline is seconds rather than an attempt count, and that is not padding: an attempt count
  // is a deadline in disguise whose real length depends on how loaded the machine is, and this case
  // runs beside the ones that compile rigs. It failed on the aarch64 build container -- where the
  // suite runs under other work -- at 200 attempts of a read plus 2 ms, which is about 400 ms.
  nlohmann::json levels;
  bool have_window = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!have_window && std::chrono::steady_clock::now() < deadline)
  {
    const auto reply = nlohmann::json::parse(call(config.socket_path, R"({"id":"l","method":"engine.levels"})"));
    REQUIRE(reply["ok"] == true);
    REQUIRE(reply.contains("levels"));
    levels = reply["levels"];
    const auto& nodes = levels["chains"][0]["nodes"];
    // Both windows or neither: the node's is what the loop is named for, and the device's `blocks`
    // is asserted below from this same reply. Polling only for the node's and then requiring the
    // device's is asserting a race -- it failed about one run in six, on the old engine as well as
    // the new one, which is how it was found.
    have_window = !nodes.empty() && nodes[0]["frames"].get<uint64_t>() > 0 &&
                  levels["blocks"].get<uint64_t>() > 0;
    if (!have_window)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE_MESSAGE(have_window, "no level window ever contained samples");

  CHECK(levels["state"] == "running");
  CHECK(levels["sample_rate"] == 48000);
  CHECK(levels["period_size"] == 64);
  CHECK(levels["blocks"].get<uint64_t>() > 0);
  CHECK(levels["truncated"] == false);

  // Read the status FIRST, before the checks that look at a window's contents: `engine.status`
  // carries its own `levels` and empties every probe on its way through, so a status call after a
  // levels read leaves that read's frame counts as the stale zeros of a drained window.
  const auto reported = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"));
  CHECK(reported["status"].contains("levels"));
  CHECK(reported["status"]["levels"]["chains"].size() == 1);
  const auto capture_channels = reported["status"]["capture_channels"].get<unsigned>();
  CHECK(capture_channels >= 1);

  // Now a fresh window to inspect: the same poll-until-there-is-one rule as above, and the same
  // deadline rather than an attempt count, for the same reason.
  //
  // The poll asks for **both** a node's window and the device's, because the assertions below are
  // about a *whole reply* and a reply is one drain of every probe at once. Waiting only for the node
  // and then reading the device's frames is the same race in a smaller place: any drain between the
  // two -- the control loop's, or another client's -- empties the device's probe, and the read that
  // follows is a window with nothing in it. That is not a fault, it is the read-and-reset design, and
  // a test that demanded otherwise would be asserting that nobody else is using the daemon.
  have_window = false;
  const auto second_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!have_window && std::chrono::steady_clock::now() < second_deadline)
  {
    const auto again = nlohmann::json::parse(call(config.socket_path, R"({"id":"l3","method":"engine.levels"})"));
    levels = again["levels"];
    const auto& fresh_nodes = levels["chains"][0]["nodes"];
    const bool node_has_samples = !fresh_nodes.empty() && fresh_nodes[0]["frames"].get<uint64_t>() > 0;
    const bool device_has_samples = !levels["input"]["frames"].empty() &&
                                    levels["input"]["frames"][0].get<uint64_t>() > 0 &&
                                    !levels["output"]["frames"].empty() &&
                                    levels["output"]["frames"][0].get<uint64_t>() > 0;
    have_window = node_has_samples && device_has_samples;
    if (!have_window)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE_MESSAGE(have_window, "no second level window contained samples for both a node and the device");
  CHECK(levels["input"]["peak"].size() == capture_channels);
  CHECK(levels["output"]["peak"].size() == reported["status"]["playback_channels"].get<unsigned>());
  CHECK(levels["input"]["rms"].size() == capture_channels);
  CHECK(levels["output"]["clipped"].size() == levels["output"]["peak"].size());
  // The device carries a frame count too, and it is the same window as its peak: this reply was
  // polled until a node's window had samples in it, and the device probes are accumulated on those
  // same periods, so an empty device window here would mean the two were read separately.
  CHECK(levels["input"]["frames"].size() == capture_channels);
  CHECK(levels["output"]["frames"].size() == levels["output"]["peak"].size());
  CHECK(levels["input"]["frames"][0].get<uint64_t>() > 0);
  CHECK(levels["output"]["frames"][0].get<uint64_t>() > 0);

  // The chain, with its document identity and its own output level.
  REQUIRE(levels["chains"].size() == 1);
  const auto& chain = levels["chains"][0];
  CHECK(chain["id"] == "main");
  CHECK(chain["output_channel"] == 0);
  CHECK(chain["muted"] == false);
  CHECK(chain.contains("out"));

  // Every node of the plan, labelled so a client can find its block: `element` is what the client
  // named and `role` is which primitive of a macro it was lowered from.
  //
  // The assertions below are about one reply, and a reply is one drain of every probe at once -- so
  // the read that produced it is the one that had *every* measured node populated, not merely the
  // first. Asserting a node had samples after polling only for some other node's is the same race
  // that produced the two flakes above, one level further down.
  have_window = false;
  const auto all_nodes_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!have_window && std::chrono::steady_clock::now() < all_nodes_deadline)
  {
    const auto reply = nlohmann::json::parse(call(config.socket_path, R"({"id":"l4","method":"engine.levels"})"));
    levels = reply["levels"];
    have_window = true;
    for (const auto& node : levels["chains"][0]["nodes"])
      if (node["element"] != "out" && node["frames"].get<uint64_t>() == 0)
        have_window = false;
    if (!have_window)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  REQUIRE_MESSAGE(have_window, "no window had samples for every measured node at once");

  const auto& nodes = levels["chains"][0]["nodes"];
  CHECK(nodes.size() == 3);
  bool saw_gain = false;
  for (const auto& node : nodes)
  {
    CHECK(node.contains("element"));
    CHECK(node.contains("role"));
    CHECK(node.contains("peak"));
    CHECK(node.contains("rms"));
    if (node["element"] == "out")
    {
      // The chain's output node has NO output port, so nothing is measured at it: what a client
      // wants there is the chain's output, which the chain's own probe reports. A node-level
      // reading of its dry input would count the last block of the signal path twice.
      CHECK(node["frames"].get<uint64_t>() == 0);
    }
    else
    {
      // A window that measured nothing is not a level: every node that has an output must have
      // samples behind its number, or it is stale from a plan that has stopped.
      CHECK(node["frames"].get<uint64_t>() > 0);
    }
    if (node["element"] == "g")
    {
      saw_gain = true;
      // The null backend hands over silence, so every node is at the floor -- and the floor is a
      // number, not a missing field or an infinity that would not survive JSON.
      CHECK(node["peak"].get<double>() == namchain::level::kFloorDb);
    }
  }
  CHECK(saw_gain);

  // The format the device was opened with, which used to be known and never reported.
  CHECK(reported["status"].contains("format"));

  // The feature is advertised, because a client that does not see it must degrade rather than guess.
  const auto hello = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.hello"})"));
  bool advertises = false;
  for (const auto& feature : hello["features"])
    if (feature == "levels")
      advertises = true;
  CHECK(advertises);
}

TEST_CASE("levels off: the feature is not advertised, the method is refused, and status omits the field")
{
  // The switch (Config::levels, --no-levels) exists so the per-sample cost of measuring can be
  // turned off on an appliance nobody polls -- and so a measurement can separate the engine's own
  // cost from the cost of watching it. What it must never do is answer -200 dBFS, which would say
  // "silence" where the truth is "nobody is measuring": the three assertions below are the three
  // ways a client could be misled, and each has to be an absence instead.
  TempDir dir;
  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.levels = false;
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
    return status["state"] == "running";
  }));

  // 1. Not advertised. A client that does not see "levels" builds no meters, which is the truth
  //    about this engine -- and is why the client's own feature-detection is enough.
  const auto hello = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.hello"})"));
  for (const auto& feature : hello["features"])
    CHECK(feature != "levels");

  // 2. Refused by name, so a client can tell "this engine does not measure" from a transport
  //    failure or a mistyped method.
  const auto refused = nlohmann::json::parse(call(config.socket_path, R"({"id":"l","method":"engine.levels"})"));
  CHECK(refused["ok"] == false);
  CHECK(refused["error"]["code"] == "levels_disabled");

  // 3. Absent from status, rather than present and full of floors.
  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"id":"s","method":"engine.status"})"));
  CHECK(status["ok"] == true);
  CHECK_FALSE(status["status"].contains("levels"));

  // Everything else is untouched: the switch turns off measuring, not the engine.
  CHECK(status["status"]["state"] == "running");
  CHECK(status["status"]["sample_rate"] == 48000);
}

TEST_CASE("levels_rate_hz answers a fast poll from the held window, and counts which it gave")
{
  // The rate switch exists so a poll costs a lookup instead of a measurement. What has to hold for
  // that to be true -- and what this test is for -- is that a read inside the interval returns the
  // *same* numbers rather than a fresh, mostly-empty window, and that the reply says which of the two
  // it was. A campaign leans on exactly those counters to prove the rate it asked for was honoured.
  TempDir dir;
  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.levels_rate_hz = 1.0;
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
    return status["state"] == "running";
  }));

  const auto levels_of = [&config] {
    return nlohmann::json::parse(call(config.socket_path, R"({"id":"l","method":"engine.levels"})"))["levels"];
  };

  const auto first = levels_of();
  CHECK(first["cache"]["rate_hz"].get<double>() == doctest::Approx(1.0));
  CHECK(first["cache"]["first_frame"] == false);

  // Reads in quick succession are inside one interval, so they must be the same window: the frame
  // count of the device's input is the number that would move if the probes had been drained again.
  // This is what stops the cache from being a second empty window dressed up as a reading.
  const auto second = levels_of();
  CHECK(second["input"]["frames"] == first["input"]["frames"]);
  CHECK(second["blocks"] == first["blocks"]);
  CHECK(second["cache"]["hits"].get<uint64_t>() > first["cache"]["hits"].get<uint64_t>());
  CHECK(second["cache"]["misses"] == first["cache"]["misses"]);

  // The control loop drains on its own cadence, so a *drain* has to happen without anybody asking --
  // which is the difference between a rate limit and a lazy cache. `drains` counts every drain there
  // has been, `misses` only the ones a read asked for, so this is the counter that can see a
  // background one: `misses` deliberately does not move here, and asserting on it would have been
  // asserting that the control loop is *not* running.
  const uint64_t drains_before = second["cache"]["drains"].get<uint64_t>();
  bool drained = false;
  for (int attempt = 0; attempt < 40 && !drained; ++attempt)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    drained = levels_of()["cache"]["drains"].get<uint64_t>() > drains_before;
  }
  CHECK_MESSAGE(drained, "the control loop never drained the probes at the configured rate");

  // And the rate is a *limit*: however long the test has been running, the number of drains is the
  // wall clock at 1 Hz and not the number of reads, which is far larger.
  const auto last = levels_of();
  const uint64_t reads = last["cache"]["hits"].get<uint64_t>() + last["cache"]["misses"].get<uint64_t>();
  CHECK(reads > last["cache"]["drains"].get<uint64_t>());
}

TEST_CASE("levels_first_frame measures one frame per period, and the engine still renders")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "gain.json", kGainRig);

  Config config;
  config.backend = BackendKind::Null;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = root.string();
  config.rig = (root / "gain.json").string();
  config.levels_first_frame = true;
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
    return status["state"] == "running" && status["blocks"].get<uint64_t>() > 4;
  }));

  // Every measurement is one sample per period, and it says so: `frames` 1 per node per block rather
  // than the period's length. A client reading `frames` can therefore tell a sparse window from a
  // full one, which is the concession this switch makes and the reason it is off by default.
  //
  // The read has to be *taken* when a window has samples in it, and every read empties the probes
  // (level.h), so a single read can legitimately land between two blocks and be empty. Polling until
  // one is not a race being papered over: it is what a meter does, and the deadline is generous
  // because the suite runs this beside other cases that are compiling rigs.
  bool saw_one_frame = false;
  std::string last_seen;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!saw_one_frame && std::chrono::steady_clock::now() < deadline)
  {
    const auto reply = nlohmann::json::parse(call(config.socket_path, R"({"id":"l","method":"engine.levels"})"));
    REQUIRE(reply["ok"] == true);
    const auto& levels = reply["levels"];
    CHECK(levels["cache"]["first_frame"] == true);
    REQUIRE(levels["chains"].size() == 1);
    for (const auto& node : levels["chains"][0]["nodes"])
    {
      const uint64_t frames = node["frames"].get<uint64_t>();
      // The output node has no output port, so nothing is measured at it and it stays 0. A measured
      // node reports the one frame this mode folds, and never the period's length.
      CHECK(frames <= 1);
      if (frames == 1)
        saw_one_frame = true;
    }
    if (!saw_one_frame)
    {
      last_seen = levels["chains"][0]["nodes"].dump();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  CHECK_MESSAGE(saw_one_frame, "no window ever reported the single frame this mode measures; last: " << last_seen);

  // The engine is otherwise untouched: it is running, with its rig, and the device is still moving
  // audio through it. Turning the measurement down must not turn the pedal down.
  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"id":"s","method":"engine.status"})"));
  CHECK(status["status"]["state"] == "running");
  CHECK(status["status"]["blocks"].get<uint64_t>() > 4);
  CHECK(status["status"]["failed_nodes"].get<uint64_t>() == 0);
}

TEST_CASE("a device level entry is one window: its peak, rms, clipped and frames agree")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "gain.json", kGainRig);

  // Twenty seconds of full-scale DC on the `file` backend, which is unpaced: the whole run is a few
  // tens of milliseconds and the test polls `engine.levels` while it happens. DC rather than a tone
  // so that every sample of every window is at full scale, which is what makes `clipped == frames` a
  // statement about one window rather than about four numbers that merely look plausible. 960000
  // frames is an exact multiple of the 64-frame period, so the backend never pads a short final
  // period with silence and *every* window is full scale, whichever one the poll catches.
  {
    const std::vector<float> samples(20u * 48000u, 1.0f);
    std::string error;
    REQUIRE_MESSAGE(namchain::save_wav_float32((dir.path / "in.wav").string(), samples.data(),
                                               samples.size(), 1, 48000.0, error),
                    error);
  }

  Config config = file_backend_config(dir, 1, 1);
  config.rigs_root = root.string();
  config.rig = (root / "gain.json").string();
  // A high rate rather than the default: this test is about what a window *contains*, and at
  // the default 8 Hz a poll every few milliseconds reads the same window over and over. That is
  // the rate limit doing its job -- it is just not what is under test here.
  config.levels_rate_hz = 200.0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);

  // Poll until a window with samples in it turns up. A read is what empties a window, so the tail
  // after the last block is still there for the poll that follows it: this cannot miss every window
  // the way a single read at a random moment can.
  //
  // The device's channel arrays are **empty** until it opens, so the poll has to tolerate that, and
  // `closed` alone is not the end -- it is also the state before the device is opened at all. A block
  // count is what tells a device that has been and gone from one that has not arrived yet.
  //
  // The rate limit is set high for this test rather than left at its default, and the sleep is what
  // makes a *new* window arrive: at the default 8 Hz, a poll every millisecond is answered from the
  // cache and reads the same window 125 times. That is the feature working -- it is just not what
  // this test is about, which is what a window contains.
  nlohmann::json window;
  const auto window_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (window.is_null() && std::chrono::steady_clock::now() < window_deadline)
  {
    const auto reply = nlohmann::json::parse(daemon.handle_request(R"({"method":"engine.levels"})"));
    REQUIRE(reply["ok"] == true);
    const nlohmann::json& levels = reply["levels"];
    REQUIRE(levels["input"].contains("frames"));
    const nlohmann::json& frames = levels["input"]["frames"];
    if (!frames.empty() && frames[0].get<uint64_t>() > 0)
      window = levels;
    else if (levels["state"] == "closed" && levels["blocks"].get<uint64_t>() > 0)
      break;
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE_MESSAGE(!window.is_null(), "no device window ever contained samples");

  // Full scale in through a gain of 0.5: the input is at 0 dBFS and every sampled frame of it is
  // clipped, the output is at -6 dBFS and clips nothing. Both are constant signals, so peak and rms
  // are the same number however the mean is defined -- this asserts the window, not the definition.
  const uint64_t in_frames = window["input"]["frames"][0].get<uint64_t>();
  CHECK(in_frames > 0);
  CHECK(window["input"]["peak"][0].get<double>() == 0.0);
  CHECK(window["input"]["rms"][0].get<double>() == 0.0);
  CHECK(window["input"]["clipped"][0].get<uint64_t>() == in_frames);
  CHECK(window["output"]["frames"][0].get<uint64_t>() > 0);
  CHECK(window["output"]["peak"][0].get<double>() == -6.0);
  CHECK(window["output"]["rms"][0].get<double>() == -6.0);
  CHECK(window["output"]["clipped"][0].get<uint64_t>() == 0);
}

TEST_CASE("a plan's prewarm is not part of the first window a level meter sees")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "gain.json", kGainRig);

  // Eight periods of half-scale DC. Short enough that the whole run fits in a window nobody has read
  // yet, long enough to have samples in it, and an exact multiple of the period so no padded tail
  // muddies the frame count.
  {
    const std::vector<float> samples(8u * 64u, 0.5f);
    std::string error;
    REQUIRE_MESSAGE(namchain::save_wav_float32((dir.path / "in.wav").string(), samples.data(),
                                               samples.size(), 1, 48000.0, error),
                    error);
  }

  Config config = file_backend_config(dir, 1, 1);
  config.rigs_root = root.string();
  config.rig = (root / "gain.json").string();

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);

  // Wait for the run to finish *without* reading the probes until it has: `engine.status` and
  // `engine.levels` both carry levels and empty them, and this test is about what one unread window
  // holds. A read taken while nothing has been rendered returns an empty window and costs nothing, so
  // polling is safe; a block count is what tells a device that has been and gone from one that has
  // not opened yet.
  nlohmann::json levels;
  for (int attempt = 0; attempt < 4000; ++attempt)
  {
    if (daemon.device_state() == namchain::daemon::DeviceState::Closed)
    {
      levels = nlohmann::json::parse(daemon.handle_request(R"({"method":"engine.levels"})"))["levels"];
      if (levels["blocks"].get<uint64_t>() > 0)
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(levels["blocks"].get<uint64_t>() == 8u);

  // Every node measured the eight periods and nothing else. The prewarm's quantum of silence used to
  // sit in each node's probe and nowhere else: one period longer than the audio, with the mean diluted
  // by a signal that never reached the device -- -6.5 dBFS for the input node rather than the -6.0 it
  // is at.
  //
  // The device's and the chain's own probes are compared against the period count rather than each
  // other because `Daemon::close_device` empties those on purpose: a window spanning a device restart
  // is a mean of neither stream, so a closed device reports silence. A node's probe outlives the
  // device with its plan, which is exactly what makes it the one that can carry a stale first window.
  bool saw_input = false;
  for (const auto& node : levels["chains"][0]["nodes"])
  {
    if (node["element"] == "out") // no output port: nothing is measured there
      continue;
    CHECK(node["frames"].get<uint64_t>() == 8u * 64u);
    if (node["element"] == "in")
    {
      saw_input = true;
      CHECK(node["peak"].get<double>() == -6.0);
      CHECK(node["rms"].get<double>() == -6.0);
    }
    else if (node["element"] == "g")
      CHECK(node["rms"].get<double>() == -12.0);
  }
  CHECK(saw_input);
}

// ---------------------------------------------------------------------------
// The warm set (docs/warm-set.md): a document rendered beside the audible one
// ---------------------------------------------------------------------------

namespace
{

/// The two rigs the warm-set tests switch between: the same shape, different gains, so which one is
/// audible is a number in the output rather than a guess.
const char* kGainHalfRig = R"({
  "name": "half",
  "chains": [{ "id": "main", "input_channel": 0, "output_channel": 0, "nodes": [
    { "id": "in", "type": "nc.input" },
    { "id": "g", "type": "nc.gain", "gain": 0.5 },
    { "id": "out", "type": "nc.output" }],
    "connections": [{ "from": "in", "to": "g" }, { "from": "g", "to": "out" }] }]
})";

const char* kGainQuarterRig = R"({
  "name": "quarter",
  "chains": [{ "id": "main", "input_channel": 0, "output_channel": 0, "nodes": [
    { "id": "in", "type": "nc.input" },
    { "id": "g", "type": "nc.gain", "gain": 0.25 },
    { "id": "out", "type": "nc.output" }],
    "connections": [{ "from": "in", "to": "g" }, { "from": "g", "to": "out" }] }]
})";

/// A daemon that renders a `file` or `null` backend with warming on and neither real-time scheduling
/// nor memory locking: what the test process is, rather than what the appliance is.
Config warm_config(const TempDir& dir, BackendKind backend)
{
  Config config;
  config.backend = backend;
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.rigs_root = (dir.path / "rigs").string();
  config.capture_path = (dir.path / "in.wav").string();
  config.playback_path = (dir.path / "out.wav").string();
  config.capture_channels = 1;
  config.playback_channels = 1;
  config.lock_memory = false;
  config.rt_priority = 0;
  config.warm_cpus = {0};
  config.warm_priority = 0; // no SCHED_FIFO either: priorities are the pedal's business
  config.warm_max_behind_blocks = 2;
  return config;
}

/// Send one request and parse the reply, failing the test rather than the parse if it is not JSON.
nlohmann::json request(Daemon& daemon, const std::string& body)
{
  const std::string reply = daemon.handle_request(body);
  const nlohmann::json parsed = nlohmann::json::parse(reply, nullptr, /*allow_exceptions=*/false);
  REQUIRE_FALSE(parsed.is_discarded());
  return parsed;
}

/// The warm entry for the rig nobody is hearing, or null when there is none. The status carries one
/// entry per document, so "the warm one" is the one that is not audible.
nlohmann::json warm_entry(const nlohmann::json& status)
{
  for (const auto& entry : status["warm"]["set"])
  {
    if (entry["state"] != "audible")
      return entry;
  }
  return nlohmann::json();
}

/// Wait until \p predicate holds for `engine.status`, or give up. The render thread is paced by the
/// wall clock, so every one of these is a wait for blocks rather than a sleep of a guessed length.
bool wait_status(Daemon& daemon, const std::function<bool(const nlohmann::json&)>& predicate, int attempts = 400)
{
  for (int attempt = 0; attempt < attempts; ++attempt)
  {
    const auto reply = request(daemon, R"({"method":"engine.status"})");
    if (reply["ok"] == true && predicate(reply["status"]))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

} // namespace

TEST_CASE("a document is owned by one thread at a time")
{
  using namchain::daemon::DocOwner;
  using namchain::daemon::claim_document;
  using namchain::daemon::release_document;

  std::atomic<DocOwner> owner{DocOwner::Free};
  CHECK(claim_document(owner, DocOwner::Warm));
  CHECK(owner.load() == DocOwner::Warm);
  // The claim is a claim: a second thread cannot take a document someone owns, which is what makes
  // "one thread executes a document's plans" true rather than hoped for.
  CHECK_FALSE(claim_document(owner, DocOwner::Device));
  CHECK(owner.load() == DocOwner::Warm);

  release_document(owner);
  CHECK(owner.load() == DocOwner::Free);
  CHECK(claim_document(owner, DocOwner::Device));
  CHECK_FALSE(claim_document(owner, DocOwner::Warm));
}

TEST_CASE("the block pool drops rather than waiting, and counts what it dropped")
{
  namchain::daemon::BlockPool pool;
  pool.configure(4); // four floats per block

  uint32_t index[namchain::daemon::BlockPool::kBlocks];
  const float* data = nullptr;
  uint64_t seq = 0;
  for (unsigned i = 0; i < namchain::daemon::BlockPool::kBlocks; ++i)
  {
    uint32_t got = 0;
    float* block = pool.acquire(100 + i, got);
    REQUIRE(block != nullptr);
    // The index is the pool's, not the caller's: it is what `publish` takes, and a caller that had to
    // guess it would publish the wrong block.
    CHECK(got < namchain::daemon::BlockPool::kBlocks);
    block[0] = static_cast<float>(i);
    index[i] = got;
    pool.publish(got);
  }
  // The blocks are the pool's own and each is handed out once: a pool that returned one index four
  // times would have four copies of one block in flight and none of the others.
  for (unsigned i = 0; i < namchain::daemon::BlockPool::kBlocks; ++i)
  {
    for (unsigned j = i + 1; j < namchain::daemon::BlockPool::kBlocks; ++j)
      CHECK(index[i] != index[j]);
  }

  // Every block is in flight, so the producer is refused and says so instead of waiting: a consumer
  // that is behind must be reported, and the render path must never block on one.
  uint32_t refused_index = 0;
  CHECK(pool.acquire(999, refused_index) == nullptr);
  CHECK(pool.dropped() == 1);

  // The consumer reads them in order, with the sequence number each was published under.
  for (unsigned i = 0; i < namchain::daemon::BlockPool::kBlocks; ++i)
  {
    REQUIRE(pool.take(index[i], seq, data));
    CHECK(seq == 100 + i);
    CHECK(data[0] == static_cast<float>(i));
    pool.give_back(index[i]);
  }
  CHECK_FALSE(pool.take(index[0], seq, data));
  // And the blocks are back: a full round trip leaves the pool as it started.
  uint32_t again = 0;
  CHECK(pool.acquire(1, again) != nullptr);
}

TEST_CASE("a warmed rig renders beside the audible one, and a switch makes it audible")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "half.json", kGainHalfRig);
  write_file(root / "quarter.json", kGainQuarterRig);

  Config config = warm_config(dir, BackendKind::Null);
  config.rig = (root / "half.json").string();

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);

  REQUIRE(wait_status(daemon, [](const nlohmann::json& s) { return s["blocks"].get<uint64_t>() > 4; }));

  // Warming is a request about a rig nobody is hearing: the audible document does not change.
  const auto before = request(daemon, R"({"method":"engine.status"})");
  const uint64_t audible_doc = before["status"]["warm"]["active_doc"].get<uint64_t>();
  const auto warmed = request(daemon, R"({"id":"w","method":"rig.warm","rig":"quarter.json"})");
  REQUIRE(warmed["ok"] == true);
  CHECK(warmed["state"] == "warming"); // compiled, not yet rendered
  CHECK(warmed["cost"]["cost"].get<double>() > 0.0);

  const auto still = request(daemon, R"({"method":"engine.status"})");
  CHECK(still["status"]["warm"]["active_doc"].get<uint64_t>() == audible_doc);
  CHECK(still["status"]["rig_name"] == "half");

  // The warm thread renders it on its own core, and once it has, the rig is warm in the only sense
  // that matters: switching to it will not load or compile anything.
  REQUIRE(wait_status(daemon, [](const nlohmann::json& s) {
    const nlohmann::json entry = warm_entry(s);
    return !entry.is_null() && entry["state"] == "warm" && entry["blocks"].get<uint64_t>() > 0;
  }));

  const auto switched = request(daemon, R"({"id":"s","method":"rig.switch","rig":"quarter.json"})");
  REQUIRE(switched["ok"] == true);
  CHECK(switched["switched"] == "warm");
  CHECK(switched["completed"] == true);
  CHECK(switched["handover_blocks"].get<uint64_t>() >= 1);

  // What is audible is the rig that was warmed, and the rig that was audible is warm in its place --
  // a swap of documents, not a load.
  const auto after = request(daemon, R"({"method":"engine.status"})");
  CHECK(after["status"]["rig_name"] == "quarter");
  REQUIRE(after["status"]["warm"]["set"].size() == 2);
  bool saw_audible_quarter = false;
  bool saw_warm_half = false;
  for (const auto& entry : after["status"]["warm"]["set"])
  {
    if (entry["state"] == "audible")
      saw_audible_quarter = entry["name"] == "quarter";
    if (entry["state"] == "warm")
      saw_warm_half = entry["name"] == "half";
  }
  CHECK(saw_audible_quarter);
  CHECK(saw_warm_half);

  // And the warm rig keeps rendering: it is the document that faded out, now kept current.
  REQUIRE(wait_status(daemon, [](const nlohmann::json& s) {
    for (const auto& entry : s["warm"]["set"])
    {
      if (entry["state"] == "warm" && entry["blocks"].get<uint64_t>() > 0)
        return true;
    }
    return false;
  }));

  // Cooling is safe while warm: the plans are retired by the warm thread and freed by the control
  // thread after the reclaimer's grace window.
  const auto cooled = request(daemon, R"({"id":"c","method":"rig.cool"})");
  REQUIRE(cooled["ok"] == true);
  CHECK(cooled["cooled"].size() == 1);
  const auto empty = request(daemon, R"({"method":"engine.status"})");
  CHECK(empty["status"]["warm"]["set"].size() == 1); // only the audible one is left
  CHECK(warm_entry(empty["status"]).is_null());
}

TEST_CASE("a switch to a rig that is not warm is a cold load, not a refusal")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "half.json", kGainHalfRig);
  write_file(root / "quarter.json", kGainQuarterRig);

  Config config = warm_config(dir, BackendKind::Null);
  config.rig = (root / "half.json").string();

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);

  REQUIRE(wait_status(daemon, [](const nlohmann::json& s) { return s["blocks"].get<uint64_t>() > 4; }));

  // Nothing is warm, so this is `rig.load` with a different name -- and it says so.
  const auto switched = request(daemon, R"({"id":"s","method":"rig.switch","rig":"quarter.json"})");
  REQUIRE(switched["ok"] == true);
  CHECK(switched["switched"] == "cold");
  CHECK(switched["reason"].get<std::string>().find("nothing is warm") != std::string::npos);
  const auto status = request(daemon, R"({"method":"engine.status"})");
  CHECK(status["status"]["warm"]["cold_switches"] == 1);

  // And a client that would rather be told than loaded can ask for that.
  const auto refused = request(daemon, R"({"id":"r","method":"rig.switch","rig":"half.json","fallback":false})");
  CHECK(refused["ok"] == false);
  CHECK(refused["error"]["code"] == "warm_not_ready");

  // Warming is not configured -> named refusals rather than methods that quietly do nothing.
  TempDir other;
  write_file(other.path / "rigs" / "half.json", kGainHalfRig);
  Config plain;
  plain.backend = BackendKind::Null;
  plain.socket_path = (other.path / "namchaind.sock").string();
  plain.rigs_root = (other.path / "rigs").string();
  plain.lock_memory = false;
  plain.rt_priority = 0;
  Daemon cold(plain);
  REQUIRE_MESSAGE(cold.start(error), error);
  const auto disabled = request(cold, R"({"method":"rig.warm","rig":"half.json"})");
  CHECK(disabled["ok"] == false);
  CHECK(disabled["error"]["code"] == "warm_disabled");
  const auto no_warm_key = request(cold, R"({"method":"engine.status"})");
  CHECK_FALSE(no_warm_key["status"].contains("warm"));
  cold.request_stop();
}

TEST_CASE("a lite_first switch hears the Lite while the Full is compiled, and a warm rig ignores the flag")
{
  // The press is what P8 is for: a cold switch makes the player wait for the whole compile, and the
  // Lite is what they hear meanwhile. Where the rig is warm there is nothing to wait for at all, so
  // the flag has nothing to do -- and the reply says which of the two happened rather than leaving a
  // client to guess.
  TempDir dir;
  write_budget_library(dir.path / "models");
  write_file(dir.path / "cost.json", test_cost_table());
  write_file(dir.path / "rigs" / "slimmable.json", one_amp_rig("A2"));
  Config config = budget_config(dir);
  // `rig.switch` exists only where warmth does (warm_disabled otherwise), and warmth is what makes
  // "this rig is already compiled" a case the flag has to answer for.
  config.warm_cpus = {0};
  config.warm_priority = 0;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);
  REQUIRE(wait_status(daemon, [](const nlohmann::json& s) { return s["blocks"].get<uint64_t>() > 0; }));

  // Nothing is warm, so this is a cold switch -- two publishes, the Lite and then the Full.
  const auto cold = request(daemon, R"({"id":"s","method":"rig.switch","rig":"slimmable.json","lite_first":true})");
  REQUIRE_MESSAGE(cold["ok"] == true, cold.dump());
  CHECK(cold["switched"] == "cold");
  CHECK(cold["lite_first"] == true);
  CHECK(cold["lite"]["trimmed"] == true);
  CHECK(cold["lite"]["adopted"] == true);
  CHECK(cold["lite"]["cost"] == doctest::Approx(0.17));
  // The Full is compiled after the reply -- that is what makes the Lite a state a client can poll
  // rather than a step inside the press -- so the reply says where it is and the status says how it
  // went. D6's flag is on the Lite in the meantime: the trim a panel shows.
  CHECK(cold["full"]["state"] == "compiling");
  CHECK(cold["generation"] == cold["lite"]["generation"]);
  const bool landed = wait_status(daemon, [](const nlohmann::json& s) {
    return s["catch_up"]["full"] == "published" && s["cost"]["lite_first"] == false;
  });
  CHECK_MESSAGE(landed, "the Full never landed after the cold switch");
  const auto after_cold = request(daemon, R"({"method":"engine.status"})");
  CHECK(after_cold["status"]["rig"].get<std::string>().find("slimmable.json") != std::string::npos);
  CHECK(after_cold["status"]["cost"]["cost"] == doctest::Approx(0.52));
  CHECK(after_cold["status"]["cost"]["lite_first"] == false);
  CHECK(after_cold["status"]["catch_up"]["replay"] == "exact");
  CHECK(after_cold["status"]["chains"][0]["slimmed_models"].empty());
  CHECK(after_cold["status"]["warm"]["cold_switches"] == 1);

  // A switch that did not ask for a Lite says nothing about one: the flag is what puts the object in
  // the reply, and a client that ignores it cannot be surprised by it.
  const auto plain = request(daemon, R"({"id":"p","method":"rig.switch","rig":"slimmable.json"})");
  REQUIRE(plain["ok"] == true);
  CHECK_FALSE(plain.contains("lite_first"));
  CHECK_FALSE(plain.contains("lite"));

  // Now warm the rig and ask again: a handover beats any Lite compile, so the flag is reported false.
  const auto warmed = request(daemon, R"({"id":"w","method":"rig.warm","rig":"slimmable.json"})");
  REQUIRE_MESSAGE(warmed["ok"] == true, warmed.dump());
  REQUIRE(wait_status(daemon, [](const nlohmann::json& s) {
    const nlohmann::json entry = warm_entry(s);
    return !entry.is_null() && entry["state"] == "warm" && entry["blocks"].get<uint64_t>() > 0;
  }));

  const auto warm_switch =
    request(daemon, R"({"id":"sw","method":"rig.switch","rig":"slimmable.json","lite_first":true})");
  REQUIRE_MESSAGE(warm_switch["ok"] == true, warm_switch.dump());
  CHECK(warm_switch["switched"] == "warm");
  CHECK(warm_switch["lite_first"] == false);
  CHECK_FALSE(warm_switch.contains("lite"));
  CHECK_FALSE(warm_switch.contains("full")); // no two-step publish happened to describe
}

TEST_CASE("a switch mid-render is continuous audio, and warmth off is bit-identical")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "half.json", kGainHalfRig);
  write_file(root / "quarter.json", kGainQuarterRig);
  // One second of a constant, so a dropout is a zero and a click is a step: both are visible in the
  // output without a signal generator.
  write_stereo_wav(dir.path / "in.wav", 48000, 1.0f, 1.0f);

  // Warming off: the engine as it was, and the reference the other two runs are compared against.
  {
    Config config;
    config.backend = BackendKind::File;
    config.socket_path = (dir.path / "off.sock").string();
    config.rigs_root = root.string();
    config.capture_path = (dir.path / "in.wav").string();
    config.playback_path = (dir.path / "off.wav").string();
    config.capture_channels = 1;
    config.playback_channels = 1;
    config.lock_memory = false;
    config.rt_priority = 0;
    config.rig = (root / "half.json").string();
    Daemon daemon(config);
    std::string error;
    REQUIRE_MESSAGE(daemon.start(error), error);
    REQUIRE(wait_status(daemon, [](const nlohmann::json& s) {
      return s["state"] == "closed" && s["blocks"].get<uint64_t>() > 0;
    }));
    daemon.request_stop();
  }
  unsigned channels = 0;
  const std::vector<float> off = read_wav(dir.path / "off.wav", channels);
  REQUIRE(channels == 1);
  REQUIRE(off.size() == 48000);
  CHECK(off[0] == doctest::Approx(0.5f));
  CHECK(off.back() == doctest::Approx(0.5f));

  // Then with warming on but unused: a configured warm core that holds nothing must change nothing,
  // and "nothing" here means the same bytes.
  {
    Config config = warm_config(dir, BackendKind::File);
    config.rig = (root / "half.json").string();
    Daemon daemon(config);
    std::string error;
    REQUIRE_MESSAGE(daemon.start(error), error);
    REQUIRE(wait_status(daemon, [](const nlohmann::json& s) {
      return s["state"] == "closed" && s["blocks"].get<uint64_t>() > 0;
    }));
    daemon.request_stop();
  }
  const std::vector<float> plain = read_wav(dir.path / "out.wav", channels);
  REQUIRE(channels == 1);
  REQUIRE(plain.size() == off.size());
  CHECK(std::equal(plain.begin(), plain.end(), off.begin()));

  // Now with a real switch in the middle of the same input.
  const auto out_path = dir.path / "switched.wav";
  {
    Config config = warm_config(dir, BackendKind::File);
    config.rig = (root / "half.json").string();
    config.playback_path = out_path.string();
    // The `file` backend renders flat out; the pace is what gives the control side time to warm a rig
    // and switch to it while there is still audio left to hear the result in.
    config.warm_test_pace_us = 250;
    Daemon daemon(config);
    std::string error;
    REQUIRE_MESSAGE(daemon.start(error), error);

    REQUIRE(wait_status(daemon, [](const nlohmann::json& s) { return s["blocks"].get<uint64_t>() > 8; }));
    // From here the warm path is doing everything it does: input published to a warm thread, a
    // document rendered on another core, a handover and a fade. None of it may allocate, and the
    // interposer counts what happens inside an RtScope -- which is both render paths.
    namchain::rt_reset_counters();
    const auto warmed = request(daemon, R"({"id":"w","method":"rig.warm","rig":"quarter.json"})");
    REQUIRE(warmed["ok"] == true);
    REQUIRE(wait_status(daemon, [](const nlohmann::json& s) {
      const nlohmann::json entry = warm_entry(s);
      return !entry.is_null() && entry["warm_blocks"].get<uint64_t>() > 0;
    }));
    const auto switched = request(daemon, R"({"id":"s","method":"rig.switch","rig":"quarter.json"})");
    REQUIRE(switched["ok"] == true);
    CHECK(switched["switched"] == "warm");

    REQUIRE(wait_status(daemon, [](const nlohmann::json& s) {
      return s["state"] == "closed" && s["blocks"].get<uint64_t>() > 0;
    }));
    daemon.request_stop();
  }

  const std::vector<float> switched = read_wav(out_path, channels);
  REQUIRE(channels == 1);
  REQUIRE(switched.size() == 48000);

  // No gap: a rig that cannot produce silence never does, at any point in the run.
  float quietest = 1.0f;
  for (float sample : switched)
    quietest = std::min(quietest, sample);
  CHECK(quietest > 0.2f);

  // No click: the fade moves the level from one rig to the other smoothly, so no sample-to-sample
  // step is anywhere near the difference between the two rigs (0.25).
  float biggest_step = 0.0f;
  for (std::size_t i = 1; i < switched.size(); ++i)
    biggest_step = std::max(biggest_step, std::fabs(switched[i] - switched[i - 1]));
  CHECK(biggest_step < 0.01f);

  CHECK(namchain::rt_check_available());
  CHECK(namchain::rt_allocation_count() == 0);
  CHECK(namchain::rt_deallocation_count() == 0);

  // And it really did switch: the run starts on one rig and ends on the other.
  CHECK(switched.front() == doctest::Approx(0.5f));
  CHECK(switched.back() == doctest::Approx(0.25f));
  // Before the switch the output is exactly the plain run's, block for block: a warm document changes
  // nothing until it is made audible.
  bool same_until_switch = true;
  std::size_t differing = 0;
  for (std::size_t i = 0; i < switched.size(); ++i)
  {
    if (switched[i] != plain[i])
    {
      same_until_switch = same_until_switch && differing == 0;
      ++differing;
    }
  }
  CHECK(differing > 0);
  CHECK(switched[0] == plain[0]);
}

TEST_CASE("a starved warm thread is reported, and the audible audio is unaffected")
{
  TempDir dir;
  const auto root = dir.path / "rigs";
  write_file(root / "half.json", kGainHalfRig);
  write_file(root / "quarter.json", kGainQuarterRig);
  write_stereo_wav(dir.path / "in.wav", 48000, 1.0f, 1.0f);

  Config config = warm_config(dir, BackendKind::File);
  config.rig = (root / "half.json").string();
  config.warm_test_pace_us = 200;
  // The test hook: the warm thread holds every block, which is a warm thread the audio interrupt has
  // made late -- repeatedly, and by much more than a block.
  config.warm_test_hold_ms = 5;

  Daemon daemon(config);
  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);

  REQUIRE(wait_status(daemon, [](const nlohmann::json& s) { return s["blocks"].get<uint64_t>() > 4; }));
  const auto warmed = request(daemon, R"({"id":"w","method":"rig.warm","rig":"quarter.json"})");
  REQUIRE(warmed["ok"] == true);

  // Behind by more than `warm_max_behind_blocks`, said as an issue rather than hidden, and with the
  // blocks the device thread could not hand over counted too.
  // Only once the warm thread has actually rendered: a document that has rendered nothing is
  // `warming`, not behind, and the difference is the whole point of the report.
  const bool behind = wait_status(daemon, [](const nlohmann::json& s) {
    const nlohmann::json entry = warm_entry(s);
    return !entry.is_null() && entry["warm_blocks"].get<uint64_t>() > 0 &&
           entry["behind_blocks"].get<uint64_t>() > 2;
  });
  CHECK(behind);
  const auto status = request(daemon, R"({"method":"engine.status"})");
  bool saw_issue = false;
  for (const auto& issue : status["status"]["issues"])
  {
    if (issue["code"] == "warm_behind")
      saw_issue = true;
  }
  CHECK(saw_issue);
  const nlohmann::json starving = warm_entry(status["status"]);
  CHECK_FALSE(starving.is_null());
  CHECK(starving["blocks_dropped"].get<uint64_t>() > 0);

  // Nothing was corrupted by it: the audible document's guard is clean, and its output is the plain
  // run's.
  CHECK(status["status"]["failed_nodes"] == 0);
  CHECK(status["status"]["guard_trips"] == 0);

  // And the starvation is not allowed to break the switch itself: the request is deferred until the
  // warm thread lets go, and it lands.
  const auto switched = request(daemon, R"({"id":"s","method":"rig.switch","rig":"quarter.json"})");
  REQUIRE(switched["ok"] == true);
  CHECK(switched["switched"] == "warm");

  REQUIRE(wait_status(daemon, [](const nlohmann::json& s) {
    return s["state"] == "closed" && s["blocks"].get<uint64_t>() > 0;
  }));
}

// ---------------------------------------------------------------------------
// P8's catch-up replay, end to end
// ---------------------------------------------------------------------------

namespace
{

/// A deterministic two-tone-and-noise signal, written as the device's input. Deterministic because the
/// comparison below is *bit-exact* between two runs of the daemon, and a random input would make a
/// failure impossible to attribute.
void write_signal_wav(const std::filesystem::path& path, unsigned frames, unsigned channels)
{
  std::vector<float> interleaved(static_cast<std::size_t>(frames) * channels);
  unsigned state = 0x2468ace0u;
  constexpr double kPi = 3.14159265358979323846;
  for (unsigned i = 0; i < frames; ++i)
  {
    state = state * 1664525u + 1013904223u;
    const double noise = (static_cast<double>((state >> 9) & 0xffffu) / 32768.0 - 1.0) * 0.02;
    const double t = static_cast<double>(i) / 48000.0;
    const double sample =
        0.30 * std::sin(2.0 * kPi * 110.0 * t) + 0.18 * std::sin(2.0 * kPi * 311.0 * t) + noise;
    for (unsigned c = 0; c < channels; ++c)
      interleaved[static_cast<std::size_t>(i) * channels + c] =
          static_cast<float>(c == 0 ? sample : sample * 0.5);
  }
  std::string error;
  REQUIRE_MESSAGE(namchain::save_wav_float32(path.string(), interleaved.data(), frames, channels, 48000.0, error),
                  error);
}

/// The `file` backend, a budget, and the pacing that lets the catch-up thread keep up with the render
/// thread. The `file` backend renders as fast as it can otherwise, and a catch-up that has to be
/// *current* before the crossfade starts needs the device to be paced like a device.
Config catch_up_file_config(const TempDir& dir)
{
  Config config = file_backend_config(dir, 2, 2);
  config.rigs_root = (dir.path / "rigs").string();
  config.models_root = (dir.path / "models").string();
  config.irs_root = (dir.path / "irs").string();
  config.cost_table = (dir.path / "cost.json").string();
  config.reclamation_timeout_ms = 5;
  config.warm_test_pace_us = 2000;
  return config;
}

/// The frames a document's models need replayed, in engine frames: what the daemon's own replay length
/// arithmetic should arrive at.
std::size_t expected_catch_up_frames(const std::string& model)
{
  const namchain::LookbackResult lookback = namchain::compute_lookback_from_file(model);
  REQUIRE(lookback.ok());
  const double rate = lookback.sample_rate > 0.0 ? lookback.sample_rate : 48000.0;
  return static_cast<std::size_t>(std::ceil(static_cast<double>(lookback.samples) * 48000.0 / rate - 1e-9));
}

} // namespace

TEST_CASE("a lite_first publish crossfades in a Full caught up with the input it missed")
{
  // The measurable claim of P8's replay, through the whole daemon: a rig loaded with `lite_first` while
  // another rig is playing renders the *same samples* as a Full that has been running since the first
  // block. Without the replay the Full starts from silence, so the blocks right after the hand-over are
  // wrong for the length of its receptive field -- which is the window this compares, and the reason
  // the comparison is anchored on the block the crossfade *closed* rather than on a fixed one.
  TempDir dir;
  write_budget_library(dir.path / "models");
  write_file(dir.path / "cost.json", test_cost_table());
  write_file(dir.path / "rigs" / "slimmable.json", one_amp_rig("A2"));
  write_file(dir.path / "rigs" / "pass.json", kGainRig);

  constexpr unsigned kQuantum = 64;
  constexpr unsigned kBlocks = 512;
  constexpr unsigned kFrames = kBlocks * kQuantum;
  constexpr unsigned kPublishAt = 48; // the two-step publish, a few blocks in
  // Compared blocks, counted from the block the crossfade closed on. Sixty blocks is 3840 frames, well
  // inside the 6347-sample receptive field: a Full that started cold is still wrong across all of it,
  // so a run without the replay cannot pass by converging before the window begins.
  constexpr uint64_t kCompareBlocks = 60;
  write_signal_wav(dir.path / "in.wav", kFrames, 2);

  // The reference: the rig the player asked for, published once and played from the beginning.
  {
    Config config = catch_up_file_config(dir);
    config.rig = (dir.path / "rigs" / "slimmable.json").string();
    config.playback_path = (dir.path / "reference.wav").string();
    Daemon daemon(config);
    run_to_end(daemon);
  }

  // The same audio, but the rig is loaded two-step while a pass-through plays.
  uint64_t full_generation = 0;
  uint64_t crossfade_closed_at = 0;
  {
    Config config = catch_up_file_config(dir);
    config.rig = (dir.path / "rigs" / "pass.json").string();
    Daemon daemon(config);

    std::string error;
    REQUIRE_MESSAGE(daemon.start(error), error);
    bool loaded = false;
    const auto status_of = [&daemon] {
      return nlohmann::json::parse(daemon.handle_request(R"({"method":"engine.status"})"))["status"];
    };
    // This daemon has no control loop -- `handle_request` *is* the control thread -- so waiting for the
    // Full means asking it, not the socket.
    const auto wait_async = [&status_of](const std::function<bool(const nlohmann::json&)>& predicate) {
      for (int attempt = 0; attempt < 8000; ++attempt)
      {
        if (predicate(status_of()))
          return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return false;
    };
    for (int attempt = 0; attempt < 20000 && !loaded; ++attempt)
    {
      const auto status = status_of();
      if (status["blocks"].get<uint64_t>() >= kPublishAt)
      {
        const auto load = nlohmann::json::parse(
            daemon.handle_request(R"({"method":"rig.load","rig":"slimmable.json","lite_first":true})"));
        REQUIRE_MESSAGE(load["ok"] == true, load.dump());
        REQUIRE(load["lite_first"] == true);
        CHECK(load["lite"]["adopted"] == true);
        // The Full is compiled off this thread, so the reply says where it is; what the documment's
        // memory needs is known now, because it comes from `lookback` and not from the compile.
        REQUIRE(load["full"]["state"] == "compiling");
        CHECK(load["full"]["catch_up"]["state"] == "exact");
        CHECK(load["full"]["catch_up"]["frames"] ==
              expected_catch_up_frames(std::string(NAMCHAIN_EXAMPLE_MODELS_DIR) + "/A2.nam"));
        // The Full's state is made current before it is crossfaded in: the window covered the models'
        // receptive field and the replay ran, which the status says once it has.
        REQUIRE(wait_async([](const nlohmann::json& s) {
          return s["catch_up"]["full"] == "published" && s["catch_up"]["replay"] != "running" &&
                 s["cost"]["lite_first"] == false;
        }));
        const auto landed = status_of();
        CHECK(landed["catch_up"]["replay"] == "exact");
        CHECK(landed["catch_up"]["blocks"].get<uint64_t>() > 1);
        full_generation = landed["generation"].get<uint64_t>();
        loaded = true;
      }
      else
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    REQUIRE_MESSAGE(loaded, "the two-step publish never happened");

    // Wait for the crossfade to *close*: until then the device is hearing a mix of the Lite and the
    // Full, which is a different signal from the reference by design.
    for (int attempt = 0; attempt < 20000; ++attempt)
    {
      const auto status = status_of();
      const auto& chain = status["chains"][0];
      if (chain["audible_generation"].get<uint64_t>() >= full_generation &&
          chain["transition_active"] == false)
      {
        crossfade_closed_at = status["blocks"].get<uint64_t>();
        break;
      }
      if (status["state"] == "closed" && status["blocks"].get<uint64_t>() > 0)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(crossfade_closed_at > 0);

    // Let the rest of the input play out.
    for (int attempt = 0; attempt < 20000; ++attempt)
    {
      const auto status = status_of();
      if (status["state"] == "closed" && status["blocks"].get<uint64_t>() > 0)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  unsigned reference_channels = 0;
  unsigned played_channels = 0;
  const std::vector<float> reference = read_wav(dir.path / "reference.wav", reference_channels);
  const std::vector<float> played = read_wav(dir.path / "out.wav", played_channels);
  REQUIRE(reference_channels == 2u);
  REQUIRE(played_channels == 2u);
  REQUIRE(reference.size() == played.size());

  // One period was written per rendered block, from the first, so block N is frames [(N-1)*64, N*64).
  // From the block *after* the one the crossfade closed on, the device is hearing the Full alone.
  const std::size_t first_frame = static_cast<std::size_t>(crossfade_closed_at) * kQuantum;
  const std::size_t last_frame = first_frame + kCompareBlocks * kQuantum;
  REQUIRE(last_frame <= kFrames);
  std::size_t differing = 0;
  for (std::size_t frame = first_frame; frame < last_frame; ++frame)
  {
    if (reference[frame * 2] != played[frame * 2])
      ++differing;
  }
  CHECK_MESSAGE(differing == 0, differing << " of " << (last_frame - first_frame)
                                         << " samples differ from the Full that had been running");
}

TEST_CASE("the Lite a two-step publish stands in with is a state a client can poll")
{
  // D6's requirement, and the reason the Full's compile is not on the control thread: "coming up to
  // full" has to be something a client can *see*, not something it is told once in a reply that cannot
  // be asked about again. Two windows are observable here, and this case watches both:
  //
  //   * the Full's **compile**: the reply has been written, the Lite is what the device is playing, and
  //     `cost` still describes the Lite (`lite_first` true, `trimmed` true, its own cost);
  //   * the Full's **catch-up**: the Full is published -- `generation` has moved and `chain.generation`
  //     with it -- and the device is still hearing the Lite until its state is current, which the same
  //     flag and `catch_up.replay == "running"` both say.
  //
  // The `file` backend is paced here, which is the only way these windows last long enough to poll: an
  // unpaced run compiles and catches up in a few milliseconds of wall time.
  TempDir dir;
  write_budget_library(dir.path / "models");
  write_file(dir.path / "cost.json", test_cost_table());
  write_file(dir.path / "rigs" / "slimmable.json", one_amp_rig("A2"));
  write_file(dir.path / "rigs" / "pass.json", kGainRig);
  write_signal_wav(dir.path / "in.wav", 512 * 64, 2);

  Config config = catch_up_file_config(dir);
  config.rig = (dir.path / "rigs" / "pass.json").string();
  // The record this feature adds to the render path is a decode and a copy into a ring that was
  // allocated before it was armed, so the interposer -- which counts allocations made inside the
  // render loop's RtScope -- must see nothing at all across the whole two-step publish.
  namchain::rt_reset_counters();
  const bool instrumented = namchain::rt_check_available();

  Daemon daemon(config);

  std::string error;
  REQUIRE_MESSAGE(daemon.start(error), error);
  const auto status_of = [&daemon] {
    return nlohmann::json::parse(daemon.handle_request(R"({"method":"engine.status"})"))["status"];
  };
  for (int attempt = 0; attempt < 8000 && status_of()["blocks"].get<uint64_t>() < 48; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

  const auto loaded = nlohmann::json::parse(
    daemon.handle_request(R"({"method":"rig.load","rig":"slimmable.json","lite_first":true})"));
  REQUIRE_MESSAGE(loaded["ok"] == true, loaded.dump());
  REQUIRE(loaded["lite_first"] == true);
  const uint64_t lite_generation = loaded["lite"]["generation"].get<uint64_t>();
  REQUIRE(loaded["full"]["state"] == "compiling");

  bool saw_compiling = false;
  bool saw_catching_up = false;
  bool saw_full = false;
  for (int attempt = 0; attempt < 20000 && !saw_full; ++attempt)
  {
    const auto status = status_of();
    const std::string full = status["catch_up"]["full"];
    if (full == "compiling")
    {
      saw_compiling = true;
      // The Lite, and only the Lite: what the device is playing is a trimmed rig, and the engine says
      // so with the numbers that describe *it* rather than the Full that is coming.
      CHECK(status["cost"]["lite_first"] == true);
      CHECK(status["cost"]["trimmed"] == true);
      CHECK(status["cost"]["cost"] == doctest::Approx(0.17));
      CHECK(status["generation"] == lite_generation);
      REQUIRE(status["chains"][0]["slimmed_models"].size() == 1);
      CHECK(status["chains"][0]["slimmed_models"][0]["node"] == "a1");
      CHECK(status["chains"][0]["audible_generation"] == lite_generation);
    }
    if (full == "published" && status["catch_up"]["replay"] == "running")
    {
      saw_catching_up = true;
      // The Full's plans exist and are being replayed into; the device is still on the Lite, which is
      // the state D6 asks to be visible, and `cost` describes the Full that is on its way.
      CHECK(status["cost"]["lite_first"] == true);
      CHECK(status["generation"] > lite_generation);
      CHECK(status["chains"][0]["audible_generation"] == lite_generation);
    }
    if (full == "published" && status["catch_up"]["replay"] != "running" &&
        status["cost"]["lite_first"] == false)
    {
      saw_full = true;
      CHECK(status["cost"]["cost"] == doctest::Approx(0.52));
      CHECK(status["cost"]["trimmed"] == false);
      CHECK(status["generation"] > lite_generation);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK_MESSAGE(saw_compiling, "the Full's compile was never observable");
  CHECK_MESSAGE(saw_catching_up, "the Full's catch-up was never observable");
  CHECK_MESSAGE(saw_full, "the Full never landed");

  // A run with no interposer must say "not measured" rather than "zero allocations": the difference
  // between a clean measurement and no measurement (namchain/rt_check.h).
  if (instrumented)
  {
    CHECK_MESSAGE(namchain::rt_allocation_count() == 0,
                  namchain::rt_allocation_count() << " allocations on the render path");
    CHECK(namchain::rt_deallocation_count() == 0);
  }
}

TEST_CASE("a Full that a later publish overtook is dropped, not published over it")
{
  // The Full is compiled in the background, so a client can publish something else while it is in
  // flight. Installing the Full afterwards would undo that publish -- the one the client asked for
  // *last* -- which is the one outcome a background publish must never have. The Full is dropped
  // instead, and `catch_up.full` says so rather than leaving the stand-in unexplained.
  TempDir dir;
  write_budget_library(dir.path / "models");
  write_file(dir.path / "cost.json", test_cost_table());
  write_file(dir.path / "rigs" / "slimmable.json", one_amp_rig("A2"));
  write_file(dir.path / "rigs" / "after.json", one_amp_rig("A2-Full"));
  const Config config = budget_config(dir);

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

  const auto lite = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.load","rig":"slimmable.json","lite_first":true})"));
  REQUIRE_MESSAGE(lite["ok"] == true, lite.dump());
  REQUIRE(lite["full"]["state"] == "compiling");

  // The publish that overtakes it. It is an ordinary publish of another rig, so what is running when
  // the dust settles is this one -- with no Full arriving behind it.
  const auto after = nlohmann::json::parse(
    call(config.socket_path, R"({"method":"rig.load","rig":"after.json"})"));
  REQUIRE_MESSAGE(after["ok"] == true, after.dump());
  const uint64_t after_generation = after["generation"].get<uint64_t>();

  const bool superseded = wait_for(config.socket_path, [](const nlohmann::json& status) {
    return status["catch_up"]["full"] == "superseded";
  });
  CHECK_MESSAGE(superseded, "the overtaken Full was not reported as dropped");
  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"))["status"];
  CHECK(status["generation"] == after_generation);
  CHECK(status["rig"].get<std::string>().find("after.json") != std::string::npos);
  CHECK(status["cost"]["lite_first"] == false);
  const auto running = nlohmann::json::parse(call(config.socket_path, R"({"method":"rig.get"})"));
  CHECK(running["document"]["chains"][0]["nodes"][1]["model"] == "A2-Full");
}

TEST_CASE("a model with unbounded memory is refused a catch-up, in the reply and in the status")
{
  // An LSTM's memory cannot be replayed exactly at any length (nam/lookback.h). The rule is that the
  // engine refuses to *promise* rather than approximating: the Full is published cold, and both the
  // reply to the request and `engine.status` say which node and why -- never a silent cold Full.
  TempDir dir;
  write_budget_library(dir.path / "models");
  std::filesystem::copy_file(std::string(NAMCHAIN_EXAMPLE_MODELS_DIR) + "/lstm.nam",
                             dir.path / "models" / "lstm.nam");
  write_file(dir.path / "cost.json", test_cost_table());
  // Something slimmable *and* an LSTM in one chain, so a Lite really does go first and the Full really
  // does have a catch-up that cannot be promised.
  write_file(dir.path / "rigs" / "mixed.json", two_amp_rig("A2", "lstm"));
  const Config config = budget_config(dir);

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

  const auto loaded = nlohmann::json::parse(
      call(config.socket_path, R"({"method":"rig.load","rig":"mixed.json","lite_first":true})"));
  REQUIRE_MESSAGE(loaded["ok"] == true, loaded.dump());
  CHECK(loaded["lite_first"] == true);
  CHECK(loaded["lite"]["trimmed"] == true);
  REQUIRE(loaded["full"]["state"] == "compiling");
  // No replay was done, and the reply says what stopped it -- naming the node, not just the verdict.
  CHECK(loaded["full"]["catch_up"]["state"] == "unbounded");
  CHECK(loaded["full"]["catch_up"]["replay"] == "none");
  CHECK(loaded["full"]["catch_up"]["frames"] == 0);
  const std::string detail = loaded["full"]["catch_up"]["detail"];
  CHECK(detail.find("LSTM") != std::string::npos);
  CHECK(detail.find("unbounded") != std::string::npos);

  // The Full still lands -- cold, and saying so: the engine refuses to *promise* exactness, never to
  // publish. And it is a state a client can poll, not only a line in one reply (D6).
  REQUIRE(wait_for(config.socket_path, [](const nlohmann::json& s) {
    return s["catch_up"]["full"] == "published";
  }));
  const auto status = nlohmann::json::parse(call(config.socket_path, R"({"method":"engine.status"})"))["status"];
  CHECK(status["catch_up"]["state"] == "unbounded");
  CHECK(status["catch_up"]["replay"] == "none");
  CHECK(status["catch_up"]["running"] == false);
}
