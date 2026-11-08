// Tests for namchaind's events (docs/daemon.md, "Events"): a subscriber is told what changed and who
// changed it, values are coalesced without losing the last one or the order, a slow reader is bounded
// and resynchronised, and a connection that never subscribed never hears anything it did not ask for.
//
// Every case runs the real control loop against the paced `null` backend and talks to it over the
// local socket -- SEQPACKET on Linux, a framed stream on the Mac -- because events are a transport
// property: a unit test of the request handler would never see a frame that is not a reply.
#include <doctest.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>

#include "namchain/daemon/config.h"
#include "namchain/daemon/daemon.h"
#include "namchain/daemon/framing.h"
#include "namchain/daemon/platform.h"

namespace
{

using namchain::BackendKind;
using namchain::daemon::Config;
using namchain::daemon::Daemon;
namespace platform = namchain::daemon::platform;

constexpr std::chrono::seconds kWaitBudget{120};

struct TempDir
{
  TempDir()
  {
    static std::atomic<unsigned> counter{0};
    path = std::filesystem::temp_directory_path() /
           ("namchaind-events-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)));
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

std::string gain_rig(const char* name, double gain)
{
  return std::string(R"({"name": ")") + name + R"(", "nodes": [
    { "id": "in", "type": "nc.input" },
    { "id": "g", "type": "nc.gain", "gain": )" + std::to_string(gain) + R"( },
    { "id": "out", "type": "nc.output" }
  ], "connections": [ { "from": "in", "to": "g" }, { "from": "g", "to": "out" } ]})";
}

const char* const kMacroRig = R"({
  "name": "macros",
  "nodes": [
    { "id": "in", "type": "nc.input" },
    { "element": "dly", "type": "fx.delay.feedback", "time": 200, "feedback": 0.4, "mix": 0.5 },
    { "id": "out", "type": "nc.output" }
  ],
  "connections": [ { "from": "in", "to": "dly" }, { "from": "dly", "to": "out" } ]
})";

/// \brief One connection that stays open, so it can hear frames it did not ask for.
///
/// Replies and events arrive on the same socket. `call` sends a request and reads until the reply
/// with its id, keeping every event it passes in `events`, in arrival order.
struct Client
{
  int fd = -1;
  bool framed = platform::local_transport_is_stream;
  std::vector<char> pending;
  std::deque<nlohmann::json> events;
  unsigned next_id = 0;

  explicit Client(const std::string& path, int receive_buffer = 0)
  {
    fd = ::socket(AF_UNIX, platform::local_socket_type(), 0);
    REQUIRE(fd >= 0);
    if (receive_buffer > 0)
      ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  }
  ~Client()
  {
    if (fd >= 0)
      ::close(fd);
  }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void send(const nlohmann::json& message)
  {
    const std::string body = message.dump();
    std::vector<char> bytes;
    if (framed)
      namchain::daemon::append_frame(bytes, body);
    else
      bytes.assign(body.begin(), body.end());
    std::size_t sent = 0;
    while (sent < bytes.size())
    {
      const ssize_t wrote = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
      if (wrote < 0 && errno == EINTR)
        continue;
      REQUIRE(wrote > 0);
      sent += static_cast<std::size_t>(wrote);
    }
  }

  /// The next message, or null when none arrives within \p timeout_ms.
  nlohmann::json receive(int timeout_ms)
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
      if (framed && pending.size() >= namchain::daemon::kFrameHeaderBytes)
      {
        const std::uint32_t length = namchain::daemon::frame_length(pending.data());
        if (pending.size() >= namchain::daemon::kFrameHeaderBytes + length)
        {
          const std::string body(pending.data() + namchain::daemon::kFrameHeaderBytes, length);
          pending.erase(pending.begin(),
                        pending.begin() + static_cast<std::ptrdiff_t>(namchain::daemon::kFrameHeaderBytes + length));
          return nlohmann::json::parse(body);
        }
      }
      const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (left <= 0)
        return nlohmann::json();
      pollfd entry{fd, POLLIN, 0};
      const int ready = ::poll(&entry, 1, static_cast<int>(left));
      if (ready <= 0)
        continue;
      std::vector<char> buffer(1 << 20);
      const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
      REQUIRE(got > 0);
      if (!framed)
        return nlohmann::json::parse(std::string(buffer.data(), static_cast<std::size_t>(got)));
      pending.insert(pending.end(), buffer.data(), buffer.data() + got);
    }
  }

  nlohmann::json call(nlohmann::json request)
  {
    const std::string id = "r" + std::to_string(next_id++);
    request["id"] = id;
    send(request);
    const auto deadline = std::chrono::steady_clock::now() + kWaitBudget;
    while (std::chrono::steady_clock::now() < deadline)
    {
      nlohmann::json message = receive(1000);
      if (message.is_null())
        continue;
      if (message.contains("event"))
      {
        CHECK_FALSE(message.contains("id")); // an event never pretends to be a reply
        events.push_back(std::move(message));
        continue;
      }
      REQUIRE(message.value("id", std::string()) == id);
      return message;
    }
    FAIL("no reply to " << request.dump());
    return nlohmann::json();
  }

  /// Read until an event named \p name arrives (keeping the others), or give up after \p timeout_ms.
  nlohmann::json wait_event(const std::string& name, int timeout_ms = 5000)
  {
    for (auto it = events.begin(); it != events.end(); ++it)
    {
      if ((*it)["event"] == name)
      {
        nlohmann::json found = *it;
        events.erase(it);
        return found;
      }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
      nlohmann::json message = receive(50);
      if (message.is_null())
        continue;
      if (message["event"] == name)
        return message;
      events.push_back(std::move(message));
    }
    return nlohmann::json();
  }

  /// Everything that arrives in the next \p ms, added to `events`.
  void drain(int ms)
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
      nlohmann::json message = receive(10);
      if (!message.is_null())
        events.push_back(std::move(message));
    }
  }

  std::vector<nlohmann::json> take(const std::string& name)
  {
    std::vector<nlohmann::json> out;
    for (auto it = events.begin(); it != events.end();)
    {
      if ((*it)["event"] == name)
      {
        out.push_back(*it);
        it = events.erase(it);
      }
      else
        ++it;
    }
    return out;
  }

  nlohmann::json hello(const std::string& name)
  {
    const auto reply = call({{"method", "engine.hello"}, {"client", {{"name", name}, {"version", "test"}}}});
    REQUIRE(reply["ok"] == true);
    return reply;
  }
};

/// A running daemon on the null backend, with its loop thread, stopped on destruction.
struct Running
{
  TempDir dir;
  Config config;
  std::unique_ptr<Daemon> daemon;
  std::thread loop;

  explicit Running(const std::function<void(Config&, const std::filesystem::path&)>& setup = nullptr)
  {
    const auto root = dir.path / "rigs";
    write_file(root / "clean.json", gain_rig("Clean", 0.5));
    write_file(root / "crunch.json", gain_rig("Crunch", 0.25));
    write_file(root / "macros.json", kMacroRig);
    config.backend = BackendKind::Null;
    config.socket_path = (dir.path / "namchaind.sock").string();
    config.rigs_root = root.string();
    config.rig = (root / "clean.json").string();
    config.device_retry_ms = 20;
    config.reclamation_timeout_ms = 5;
    config.lock_memory = false;
    config.rt_priority = 0;
    if (setup)
      setup(config, root);
    daemon = std::make_unique<Daemon>(config);
    std::string error;
    REQUIRE_MESSAGE(daemon->start(error), error);
    loop = std::thread([this] { daemon->run(); });
  }
  ~Running()
  {
    daemon->request_stop();
    if (loop.joinable())
      loop.join();
  }

  void wait_running(Client& client)
  {
    const auto deadline = std::chrono::steady_clock::now() + kWaitBudget;
    while (std::chrono::steady_clock::now() < deadline)
    {
      const auto status = client.call({{"method", "engine.status"}});
      if (status["status"]["state"] == "running" && status["status"]["blocks"].get<uint64_t>() > 0)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    FAIL("the engine never reached running");
  }
};

nlohmann::json set_gain(double value, const std::string& tag = std::string())
{
  nlohmann::json request = {{"method", "rig.patch"},
                            {"ops", {{{"op", "set_param"}, {"node", "g"}, {"param", "gain"}, {"value", value}}}}};
  if (!tag.empty())
    request["tag"] = tag;
  return request;
}

} // namespace

TEST_CASE("events: hello names the connection, and a change carries its origin and tag")
{
  Running running;
  Client viewer(running.config.socket_path);
  Client pedald(running.config.socket_path);
  running.wait_running(viewer);

  const auto seen = viewer.hello("rigpa");
  const auto actor = pedald.hello("pedald");
  CHECK(seen["connection"].get<uint64_t>() > 0);
  CHECK(actor["connection"] != seen["connection"]);
  for (const char* feature : {"events", "client_name", "op_tag", "if_seq"})
    CHECK(std::find(seen["features"].begin(), seen["features"].end(), feature) != seen["features"].end());

  const auto subscribed = viewer.call({{"method", "engine.subscribe"}});
  REQUIRE(subscribed["ok"] == true);
  CHECK(subscribed["subscribed"].size() == 4);
  const uint64_t seq = subscribed["seq"].get<uint64_t>();
  CHECK(seq >= 1); // the startup rig is the first change

  // A footswitch, as pedald would send it: the cause and the result arrive together.
  const auto switched = pedald.call({{"method", "rig.load"}, {"rig", "crunch.json"}, {"tag", "footswitch:81"}});
  REQUIRE_MESSAGE(switched["ok"] == true, switched.dump());
  CHECK(switched["seq"] == seq + 1);
  const auto replaced = viewer.wait_event("rig.replaced");
  REQUIRE_FALSE(replaced.is_null());
  CHECK(replaced["why"] == "load");
  CHECK(replaced["seq"] == seq + 1);
  CHECK(replaced["name"] == "Crunch");
  CHECK(replaced["origin"]["client"] == "pedald");
  CHECK(replaced["origin"]["connection"] == actor["connection"]);
  CHECK(replaced["origin"]["tag"] == "footswitch:81");
  CHECK(replaced["document"]["chains"][0]["nodes"].size() == 3);

  // rig.get agrees with the event about where the document is.
  const auto got = viewer.call({{"method", "rig.get"}});
  CHECK(got["seq"] == seq + 1);

  // Applying what is already playing is a fixed point: no seq, no event.
  const auto same = pedald.call({{"method", "rig.load"}, {"rig", "crunch.json"}});
  REQUIRE(same["ok"] == true);
  CHECK(same["seq"] == seq + 1);
  viewer.drain(150);
  CHECK(viewer.take("rig.replaced").empty());
}

TEST_CASE("events: a ramped value names the document node it landed on, a macro's included")
{
  Running running([](Config& config, const std::filesystem::path& root) {
    config.rig = (root / "macros.json").string();
  });
  Client viewer(running.config.socket_path);
  Client editor(running.config.socket_path);
  running.wait_running(viewer);
  viewer.hello("viewer");
  editor.hello("ljoss");
  REQUIRE(viewer.call({{"method", "engine.subscribe"}, {"events", {"params"}}})["ok"] == true);

  const auto patched = editor.call(
    {{"method", "rig.patch"},
     {"tag", "knob"},
     {"ops", {{{"op", "set_param"}, {"element", "dly"}, {"param", "feedback"}, {"value", 0.6}}}}});
  REQUIRE_MESSAGE(patched["ok"] == true, patched.dump());
  CHECK(patched["kind"] == "smooth");
  const auto params = viewer.wait_event("rig.params");
  REQUIRE_FALSE(params.is_null());
  REQUIRE(params["values"].size() == 1);
  const auto& value = params["values"][0];
  CHECK(value["param"] == "feedback");
  CHECK(value["value"].get<double>() == doctest::Approx(0.6));
  CHECK(value["address"]["element"] == "dly");
  CHECK(value["origin"]["tag"] == "knob");
  CHECK(value["origin"]["client"] == "ljoss");
  CHECK(value["seq"] == patched["seq"]);
  // The node the value landed on is the macro's own node in the document.
  const auto document = viewer.call({{"method", "rig.get"}})["document"];
  std::string macro_node;
  for (const auto& node : document["chains"][0]["nodes"])
  {
    if (node.value("element", std::string()) == "dly")
      macro_node = node["id"].get<std::string>();
  }
  REQUIRE_FALSE(macro_node.empty());
  CHECK(value["node"] == macro_node);
}

TEST_CASE("events: a structural patch carries its ops and the document, after any waiting values")
{
  Running running;
  Client viewer(running.config.socket_path);
  Client editor(running.config.socket_path);
  running.wait_running(viewer);
  viewer.hello("viewer");
  editor.hello("ljoss");
  // A long interval, so the value is still waiting when the structural change comes.
  REQUIRE(viewer.call({{"method", "engine.subscribe"}, {"min_interval_ms", 1000}})["ok"] == true);
  viewer.drain(50);
  viewer.events.clear();

  REQUIRE(editor.call(set_gain(0.3))["ok"] == true);  // goes at once (first after quiet)
  REQUIRE(editor.call(set_gain(0.31))["ok"] == true); // waits for the interval
  const auto bypass = editor.call({{"method", "rig.patch"}, {"ops", {{{"op", "bypass"}, {"node", "g"}, {"bypass", true}}}}});
  REQUIRE_MESSAGE(bypass["ok"] == true, bypass.dump());
  CHECK(bypass["kind"] == "structural");

  viewer.drain(300);
  std::vector<std::string> order;
  for (const auto& event : viewer.events)
    order.push_back(event["event"].get<std::string>());
  REQUIRE(order.size() >= 3);
  // The waiting 0.31 is sent before the patch, though its interval had not passed.
  CHECK(order[0] == "rig.params");
  CHECK(order[1] == "rig.params");
  CHECK(order[2] == "rig.patched");
  CHECK(viewer.events[1]["values"][0]["value"].get<double>() == doctest::Approx(0.31));
  const auto& patch = viewer.events[2];
  CHECK(patch["ops"][0]["op"] == "bypass");
  CHECK(patch["seq"] == bypass["seq"]);
  bool bypassed = false;
  for (const auto& node : patch["document"]["chains"][0]["nodes"])
  {
    if (node["id"] == "g")
      bypassed = node.value("bypass", false);
  }
  CHECK(bypassed);
}

TEST_CASE("events: coalescing keeps the last value and bounds the frames")
{
  Running running;
  Client viewer(running.config.socket_path);
  Client editor(running.config.socket_path);
  running.wait_running(viewer);
  viewer.hello("viewer");
  editor.hello("ljoss");
  REQUIRE(viewer.call({{"method", "engine.subscribe"}, {"events", {"params"}}, {"min_interval_ms", 100}})["ok"] ==
          true);

  // 100 values, as fast as they can go; the test's own budget is 50 ms, and a slow host is given the
  // frames its time allows rather than a failure.
  const auto started = std::chrono::steady_clock::now();
  for (int i = 1; i <= 100; ++i)
    REQUIRE(editor.call(set_gain(i / 100.0))["ok"] == true);
  const auto took = std::chrono::steady_clock::now() - started;
  viewer.drain(400);
  const auto frames = viewer.take("rig.params");
  REQUIRE_FALSE(frames.empty());
  const auto allowed = 2 + std::chrono::duration_cast<std::chrono::milliseconds>(took).count() / 100;
  CHECK(static_cast<long long>(frames.size()) <= allowed);
  if (took < std::chrono::milliseconds(50))
    CHECK(frames.size() <= 2);
  // The last value is always the one that arrives last, and the seq only climbs.
  CHECK(frames.back()["values"][0]["value"].get<double>() == doctest::Approx(1.0));
  uint64_t last = 0;
  for (const auto& frame : frames)
  {
    CHECK(frame["seq"].get<uint64_t>() > last);
    last = frame["seq"].get<uint64_t>();
  }
}

TEST_CASE("events: echo false leaves out a connection's own changes")
{
  Running running;
  Client editor(running.config.socket_path);
  Client other(running.config.socket_path);
  running.wait_running(editor);
  editor.hello("ljoss");
  other.hello("rigpa");
  REQUIRE(editor.call({{"method", "engine.subscribe"}, {"echo", false}, {"events", {"params", "rig"}}})["ok"] ==
          true);

  REQUIRE(editor.call(set_gain(0.2))["ok"] == true);
  editor.drain(200);
  CHECK(editor.take("rig.params").empty());

  REQUIRE(other.call(set_gain(0.4))["ok"] == true);
  const auto heard = editor.wait_event("rig.params");
  REQUIRE_FALSE(heard.is_null());
  CHECK(heard["values"][0]["origin"]["client"] == "rigpa");
}

TEST_CASE("events: if_seq refuses a change made against a document that has moved on")
{
  Running running;
  Client a(running.config.socket_path);
  Client b(running.config.socket_path);
  running.wait_running(a);
  const uint64_t seq = a.call({{"method", "rig.get"}})["seq"].get<uint64_t>();

  auto first = set_gain(0.3);
  first["if_seq"] = seq;
  const auto ok = a.call(first);
  REQUIRE(ok["ok"] == true);
  CHECK(ok["seq"] == seq + 1);

  auto stale = set_gain(0.7);
  stale["if_seq"] = seq; // b never saw a's change
  const auto refused = b.call(stale);
  CHECK(refused["ok"] == false);
  CHECK(refused["error"]["code"] == "conflict");
  const auto got = b.call({{"method", "rig.get"}});
  CHECK(got["seq"] == seq + 1);
  for (const auto& node : got["document"]["chains"][0]["nodes"])
  {
    if (node["id"] == "g")
      CHECK(node["gain"].get<double>() == doctest::Approx(0.3));
  }
}

TEST_CASE("events: mute, state and assets")
{
  Running running;
  Client viewer(running.config.socket_path);
  Client actor(running.config.socket_path);
  running.wait_running(viewer);
  viewer.hello("viewer");
  actor.hello("pedald");
  REQUIRE(viewer.call({{"method", "engine.subscribe"}, {"events", {"state", "assets"}}})["ok"] == true);

  // A state subscriber starts from the state as it is.
  const auto initial = viewer.wait_event("engine.state");
  REQUIRE_FALSE(initial.is_null());
  CHECK(initial["state"] == "running");
  CHECK(initial["name"] == "Clean");
  CHECK(initial["mute"] == false);
  CHECK_FALSE(initial.contains("blocks")); // nothing that moves every block

  REQUIRE(actor.call({{"method", "engine.mute"}, {"mute", true}, {"tag", "panel"}})["ok"] == true);
  const auto muted = viewer.wait_event("engine.mute");
  REQUIRE_FALSE(muted.is_null());
  CHECK(muted["mute"] == true);
  CHECK(muted["origin"]["tag"] == "panel");
  const auto changed = viewer.wait_event("engine.state", 2000);
  REQUIRE_FALSE(changed.is_null());
  CHECK(changed["mute"] == true);

  REQUIRE(actor.call({{"method", "engine.panic"}})["ok"] == true);
  const auto panic = viewer.wait_event("engine.mute");
  REQUIRE_FALSE(panic.is_null());
  CHECK(panic["panic"] == true);

  // Nothing moved, so nothing more is sent: state is a change, not a heartbeat.
  REQUIRE(actor.call({{"method", "engine.mute"}, {"mute", true}})["ok"] == true);
  viewer.drain(350);
  viewer.take("engine.mute");
  CHECK(viewer.take("engine.state").empty());

  REQUIRE(actor.call({{"method", "engine.assets"}, {"refresh", true}})["ok"] == true);
  const auto assets = viewer.wait_event("engine.assets");
  REQUIRE_FALSE(assets.is_null());
  CHECK(assets.contains("models"));
  CHECK(assets["origin"]["client"] == "pedald");
}

TEST_CASE("events: rig.replaced on apply, documents false, and a warm and a cold switch")
{
  Running running([](Config& config, const std::filesystem::path&) {
    config.warm_cpus = {0};
    config.warm_priority = 0;
  });
  Client viewer(running.config.socket_path);
  Client slim(running.config.socket_path);
  Client actor(running.config.socket_path);
  running.wait_running(viewer);
  viewer.hello("viewer");
  slim.hello("light");
  actor.hello("pedald");
  REQUIRE(viewer.call({{"method", "engine.subscribe"}, {"events", {"rig"}}})["ok"] == true);
  REQUIRE(slim.call({{"method", "engine.subscribe"}, {"events", {"rig"}}, {"documents", false}})["ok"] == true);

  const auto applied = actor.call({{"method", "rig.apply"}, {"document", nlohmann::json::parse(gain_rig("Applied", 0.9))}});
  REQUIRE_MESSAGE(applied["ok"] == true, applied.dump());
  const auto replaced = viewer.wait_event("rig.replaced");
  REQUIRE_FALSE(replaced.is_null());
  CHECK(replaced["why"] == "apply");
  CHECK(replaced["source"] == "applied");
  CHECK(replaced.contains("document"));
  const auto light = slim.wait_event("rig.replaced");
  REQUIRE_FALSE(light.is_null());
  CHECK(light["name"] == "Applied");
  CHECK_FALSE(light.contains("document"));

  // A cold switch: nothing is warm.
  const auto cold = actor.call({{"method", "rig.switch"}, {"rig", "crunch.json"}, {"tag", "footswitch:81"}});
  REQUIRE_MESSAGE(cold["ok"] == true, cold.dump());
  CHECK(cold["switched"] == "cold");
  const auto cold_event = viewer.wait_event("rig.replaced");
  REQUIRE_FALSE(cold_event.is_null());
  CHECK(cold_event["why"] == "switch");
  CHECK(cold_event["name"] == "Crunch");
  CHECK(cold_event["origin"]["tag"] == "footswitch:81");

  // A warm one: the handover changes the running document without a publish.
  REQUIRE(actor.call({{"method", "rig.warm"}, {"rig", "clean.json"}})["ok"] == true);
  const auto deadline = std::chrono::steady_clock::now() + kWaitBudget;
  bool warm = false;
  while (!warm && std::chrono::steady_clock::now() < deadline)
  {
    const auto status = actor.call({{"method", "engine.status"}});
    for (const auto& entry : status["status"]["warm"]["set"])
      warm = warm || (entry["state"] == "warm" && entry["blocks"].get<uint64_t>() > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(warm);
  const auto warm_switch = actor.call({{"method", "rig.switch"}, {"rig", "clean.json"}, {"tag", "footswitch:80"}});
  REQUIRE_MESSAGE(warm_switch["ok"] == true, warm_switch.dump());
  CHECK(warm_switch["switched"] == "warm");
  const auto warm_event = viewer.wait_event("rig.replaced");
  REQUIRE_FALSE(warm_event.is_null());
  CHECK(warm_event["why"] == "switch");
  CHECK(warm_event["name"] == "Clean");
  CHECK(warm_event["origin"]["tag"] == "footswitch:80");
  CHECK(warm_event["seq"] == warm_switch["seq"]);
}

TEST_CASE("events: a connection that never subscribed hears only its replies, and unsubscribe stops them")
{
  Running running;
  Client quiet(running.config.socket_path);
  Client listener(running.config.socket_path);
  Client actor(running.config.socket_path);
  running.wait_running(quiet);
  REQUIRE(listener.call({{"method", "engine.subscribe"}})["ok"] == true);

  for (int i = 0; i < 5; ++i)
    REQUIRE(actor.call(set_gain(0.1 * (i + 1)))["ok"] == true);
  REQUIRE(actor.call({{"method", "rig.load"}, {"rig", "crunch.json"}})["ok"] == true);
  REQUIRE(actor.call({{"method", "engine.mute"}, {"mute", true}})["ok"] == true);

  // The quiet connection asks something and gets exactly its reply, then nothing at all.
  REQUIRE(quiet.call({{"method", "engine.hello"}})["ok"] == true);
  quiet.drain(300);
  CHECK(quiet.events.empty());

  listener.drain(200);
  CHECK_FALSE(listener.events.empty());
  REQUIRE(listener.call({{"method", "engine.unsubscribe"}})["subscribed"].empty());
  listener.drain(150);
  listener.events.clear();
  REQUIRE(actor.call({{"method", "rig.load"}, {"rig", "clean.json"}})["ok"] == true);
  listener.drain(300);
  CHECK(listener.events.empty());
}

TEST_CASE("events: a reader that stops reading is bounded, then resynchronised")
{
  Running running;
  // A small receive buffer, so the daemon's side fills quickly when this client does not read.
  Client slow(running.config.socket_path, 4096);
  Client actor(running.config.socket_path);
  running.wait_running(actor);
  REQUIRE(slow.call({{"method", "engine.subscribe"}, {"events", {"rig"}}})["ok"] == true);

  // Each replacement carries a whole document; this many is well past 256 KiB of them.
  const std::string padding(4000, 'x');
  for (int i = 0; i < 160; ++i)
  {
    nlohmann::json document = nlohmann::json::parse(gain_rig("Big", 0.5 + (i % 2) * 0.1));
    document["meta"] = {{"padding", padding}, {"i", i}};
    REQUIRE(actor.call({{"method", "rig.apply"}, {"document", document}})["ok"] == true);
  }
  // The daemon is still answering everyone else promptly.
  const auto status = actor.call({{"method", "engine.status"}});
  REQUIRE(status["ok"] == true);

  // Now read: some replacements, then a resync, then the stream again.
  bool resynced = false;
  int replaced_after = 0;
  int replaced_before = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline)
  {
    const auto message = slow.receive(200);
    if (message.is_null())
    {
      if (resynced)
        break;
      continue;
    }
    if (message["event"] == "resync")
      resynced = true;
    else if (message["event"] == "rig.replaced")
      (resynced ? replaced_after : replaced_before) += 1;
  }
  CHECK(resynced);
  CHECK(replaced_before < 160); // some were dropped rather than held
  CHECK(replaced_before > 0);

  // After the resync, events flow again.
  REQUIRE(actor.call({{"method", "rig.load"}, {"rig", "crunch.json"}})["ok"] == true);
  CHECK_FALSE(slow.wait_event("rig.replaced").is_null());
}
