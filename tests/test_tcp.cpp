// Tests for the TCP control transport (docs/ui-integration-plan.md slice C, docs/daemon.md §"Two
// transports"): the framing, the token, and the caps.
//
// The cases here are the ones a stream socket makes possible and a datagram socket makes impossible,
// which is why they are a file of their own rather than more of tests/test_daemon.cpp: a message split
// across packets, two messages in one packet, a message that declares more than it may, a reader that
// stops reading, a client that vanishes half way through a message, and a peer that is not
// authenticated. The existing UDS cases stay where they are and are expected to be unchanged -- the
// body on the wire is the same on both transports, and that is the claim these two files make
// together.
#include <doctest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
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
using namchain::daemon::append_frame;
using namchain::daemon::Config;
using namchain::daemon::Daemon;
using namchain::daemon::frame_length;
using namchain::daemon::kFrameHeaderBytes;

/// Comfortably past \ref namchain::daemon::kMinTokenLength, because the daemon refuses a token a
/// person could have chosen.
const char* const kToken = "0123456789abcdef0123456789abcdef";

/// A unique directory under the system temp dir, removed on destruction. Unique per process and per
/// instance so the test binary can run twice at once (CI does).
struct TempDir
{
  TempDir()
  {
    static std::atomic<unsigned> counter{0};
    path = std::filesystem::temp_directory_path() /
           ("namchaind-tcp-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)));
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

int milliseconds_since(const std::chrono::steady_clock::time_point& start)
{
  return static_cast<int>(
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

/// \brief A daemon with both transports up, its control loop on a thread, and a TCP port the kernel
/// picked (`tcp_port: 0`), so two runs of this binary cannot collide on a fixed port.
///
/// The daemon is built by \ref start rather than by the constructor, so a test can adjust \ref config
/// first: every cap this file exercises (the connection count, the message size, the authentication
/// window) is configuration.
struct Server
{
  Server()
  {
    config.backend = BackendKind::Null;
    config.lock_memory = false;
    config.rt_priority = 0;
    config.device_retry_ms = 20;
    config.reclamation_timeout_ms = 5;
    config.rigs_root = root.string();
    config.socket_path = (root / "namchaind.sock").string();
    config.rig = (root / "gain.json").string();
    config.tcp_listen = true;
    config.tcp_address = "127.0.0.1";
    config.tcp_port = 0;
    config.tcp_token = kToken;
  }

  ~Server()
  {
    if (daemon)
      daemon->request_stop();
    if (loop.joinable())
      loop.join();
  }

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  /// Start the daemon and its control loop. \return the error, or an empty string.
  std::string start()
  {
    write_file(root / "gain.json", kGainRig);
    daemon = std::make_unique<Daemon>(config);
    std::string error;
    if (!daemon->start(error))
      return error;
    loop = std::thread([this] { daemon->run(); });
    return {};
  }

  unsigned port() const { return daemon->tcp_port(); }

  TempDir dir;
  std::filesystem::path root = dir.path / "rigs";
  Config config;
  std::unique_ptr<Daemon> daemon;
  std::thread loop;
};

/// A TCP client. The daemon's side is non-blocking, but a client that reads what it asked for has no
/// reason to be, and a test that blocks is a test whose failures are timeouts.
struct Client
{
  /// \param receive_buffer if non-zero, the receive buffer is set *before* connecting, so the size is
  /// advertised in the handshake rather than negotiated down afterwards.
  Client(unsigned port, bool non_blocking = false, int receive_buffer = 0)
  {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    if (receive_buffer > 0)
      REQUIRE(::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    if (non_blocking)
    {
      const int flags = ::fcntl(fd, F_GETFL, 0);
      REQUIRE(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    }
  }

  ~Client()
  {
    if (fd >= 0)
      ::close(fd);
  }

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  /// \return false when the peer would not take the bytes: a non-blocking flood, not a failure.
  bool send_all(const std::string& bytes)
  {
    std::size_t sent = 0;
    while (sent < bytes.size())
    {
      const ssize_t written = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
      if (written > 0)
      {
        sent += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno == EINTR)
        continue;
      return false;
    }
    return true;
  }

  void send_frame(const std::string& body)
  {
    std::vector<char> framed;
    append_frame(framed, body);
    REQUIRE(send_all(std::string(framed.data(), framed.size())));
  }

  /// One framed reply, or nothing if the connection ended or the deadline passed first.
  std::optional<std::string> receive_frame(int timeout_ms = 5000)
  {
    const auto start = std::chrono::steady_clock::now();
    char header[kFrameHeaderBytes];
    if (!receive_exactly(header, sizeof(header), start, timeout_ms))
      return std::nullopt;
    const std::uint32_t length = frame_length(header);
    std::string body(length, '\0');
    if (length > 0 && !receive_exactly(body.data(), length, start, timeout_ms))
      return std::nullopt;
    return body;
  }

  /// One reply, parsed. A null JSON object means the connection ended first.
  nlohmann::json receive_json(int timeout_ms = 5000)
  {
    const auto reply = receive_frame(timeout_ms);
    if (!reply)
      return nlohmann::json();
    return nlohmann::json::parse(*reply);
  }

  /// True when the peer has closed: a readable socket that reads zero bytes.
  bool is_closed(int timeout_ms = 5000)
  {
    pollfd p{fd, POLLIN, 0};
    const int ready = ::poll(&p, 1, timeout_ms);
    if (ready <= 0)
      return false;
    char byte = 0;
    return ::recv(fd, &byte, 1, 0) == 0;
  }

  bool receive_exactly(char* data, std::size_t size, const std::chrono::steady_clock::time_point& start,
                       int timeout_ms)
  {
    std::size_t got = 0;
    while (got < size)
    {
      const int left = timeout_ms - milliseconds_since(start);
      if (left <= 0)
        return false;
      pollfd p{fd, POLLIN, 0};
      const int ready = ::poll(&p, 1, left);
      if (ready <= 0)
        return false;
      const ssize_t bytes = ::recv(fd, data + got, size - got, 0);
      if (bytes > 0)
      {
        got += static_cast<std::size_t>(bytes);
        continue;
      }
      if (bytes < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      return false;
    }
    return true;
  }

  int fd = -1;
};

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

/// The same exchange, parsed, for the cases that compare the two transports or read status
/// while every TCP slot is busy.
nlohmann::json local_call(const std::string& socket_path, const std::string& request)
{
  return nlohmann::json::parse(local_exchange(socket_path, request));
}

std::string hello_body(const std::string& id, const std::string& token)
{
  nlohmann::json request;
  request["id"] = id;
  request["method"] = "engine.hello";
  if (!token.empty())
    request["token"] = token;
  return request.dump();
}

} // namespace

// ---------------------------------------------------------------------------
// The token
// ---------------------------------------------------------------------------

TEST_CASE("the TCP transport authenticates engine.hello and nothing else")
{
  Server server;
  server.config.tcp_auth_timeout_ms = 5000; // this case is about the token, not the window
  REQUIRE(server.start().empty());

  Client client(server.port());

  // Anything but hello on an unauthenticated connection is refused by name, and the refusal names the
  // method rather than the token, because "you may not do this yet" is the actionable half.
  client.send_frame(R"({"id":"s","method":"engine.status"})");
  auto reply = client.receive_json();
  REQUIRE(reply["ok"] == false);
  CHECK(reply["error"]["code"] == "unauthorized");
  CHECK(reply["id"] == "s");

  // A hello with no token and a hello with the wrong token are the same answer: a peer that does not
  // hold the token learns nothing about which half of its guess was right.
  client.send_frame(hello_body("h1", ""));
  reply = client.receive_json();
  CHECK(reply["error"]["code"] == "unauthorized");

  client.send_frame(hello_body("h2", "0123456789abcdef0123456789abcdeF"));
  reply = client.receive_json();
  CHECK(reply["error"]["code"] == "unauthorized");

  // That was the third failure. The refusal is delivered and then the connection is closed, which is
  // what makes the cost of guessing three attempts per connection rather than a stream of them.
  CHECK(client.is_closed());

  // A fresh connection with the right token authenticates, and reports itself as authenticated on the
  // TCP transport -- the difference between "wrong token" and "wrong method" a client has to be able
  // to tell apart.
  Client good(server.port());
  good.send_frame(hello_body("h3", kToken));
  reply = good.receive_json();
  REQUIRE(reply["ok"] == true);
  CHECK(reply["transport"] == "tcp");
  CHECK(reply["authenticated"] == true);
  CHECK(reply["protocol"] == 2);

  good.send_frame(R"({"id":"s2","method":"engine.status"})");
  reply = good.receive_json();
  REQUIRE(reply["ok"] == true);
  CHECK(reply["id"] == "s2");
}

TEST_CASE("an unauthenticated TCP connection is closed when its window passes")
{
  Server server;
  server.config.tcp_auth_timeout_ms = 200;
  REQUIRE(server.start().empty());

  Client idle(server.port());
  // Connected, says nothing, and holds one of the two slots this daemon was configured with. Without
  // a deadline, two of these would be an outage that needs no token at all.
  CHECK(idle.is_closed(5000));

  // The slot is genuinely free again, not merely closed on the client's side.
  Client good(server.port());
  good.send_frame(hello_body("h", kToken));
  const auto reply = good.receive_json();
  REQUIRE(reply["ok"] == true);
}

TEST_CASE("the token can come from a file, and a token too short to be one is refused")
{
  TempDir dir;
  const auto token_file = dir.path / "token";
  write_file(token_file, std::string(kToken) + "\n");
  // 0600, which is what the daemon's warning asks for: a token file other local users can read is a
  // token they have. (The warning itself is not asserted here -- it is a line on stderr, and this test
  // is about the token that comes out of the file.)
  std::filesystem::permissions(token_file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

  Config config;
  config.tcp_listen = true;
  config.tcp_token_file = token_file.string();
  std::string error;
  REQUIRE_MESSAGE(namchain::daemon::resolve_tcp_token(config, error), error);
  CHECK(config.tcp_token == kToken); // the trailing newline a text editor leaves is not the token

  Config both;
  both.tcp_listen = true;
  both.tcp_token = kToken;
  both.tcp_token_file = token_file.string();
  CHECK_FALSE(namchain::daemon::resolve_tcp_token(both, error));
  CHECK(error.find("not both") != std::string::npos);

  Config short_token;
  short_token.tcp_listen = true;
  short_token.tcp_token = "hunter2";
  CHECK_FALSE(namchain::daemon::resolve_tcp_token(short_token, error));
  CHECK(error.find("at least") != std::string::npos);

  Config missing;
  missing.tcp_listen = true;
  CHECK_FALSE(namchain::daemon::resolve_tcp_token(missing, error));
  CHECK(error.find("needs a token") != std::string::npos);

  // An unreadable file, and one that exists but holds nothing: two failures with two different fixes,
  // so they are two different messages.
  Config unreadable;
  unreadable.tcp_listen = true;
  unreadable.tcp_token_file = (dir.path / "not-there").string();
  CHECK_FALSE(namchain::daemon::resolve_tcp_token(unreadable, error));
  CHECK(error.find("cannot read") != std::string::npos);

  const auto empty_file = dir.path / "empty-token";
  write_file(empty_file, "\n");
  Config empty;
  empty.tcp_listen = true;
  empty.tcp_token_file = empty_file.string();
  CHECK_FALSE(namchain::daemon::resolve_tcp_token(empty, error));
  CHECK(error.find("is empty") != std::string::npos);
}

TEST_CASE("a listener without a token does not start")
{
  TempDir dir;
  Config config;
  config.backend = BackendKind::Null;
  config.lock_memory = false;
  config.rt_priority = 0;
  config.rigs_root = dir.path.string();
  config.socket_path = (dir.path / "namchaind.sock").string();
  config.tcp_listen = true;
  config.tcp_port = 0;

  Daemon daemon(config);
  std::string error;
  CHECK_FALSE(daemon.start(error));
  CHECK(error.find("token") != std::string::npos);

  // The refusal is complete: the token is resolved before anything is bound, so the same config with
  // a token starts -- on the same socket path, rather than tripping over leftovers from the attempt.
  config.tcp_token = kToken;
  Daemon second(config);
  std::string second_error;
  REQUIRE_MESSAGE(second.start(second_error), second_error);
  CHECK(second.tcp_port() != 0);
  second.request_stop();
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

TEST_CASE("a framed message delivers one byte at a time and two messages deliver in one write")
{
  Server server;
  REQUIRE(server.start().empty());

  Client client(server.port());

  // The worst case a stream socket allows: the header itself arrives in pieces, and so does the body.
  // Nothing may be assumed about where a recv ends.
  std::vector<char> framed;
  append_frame(framed, hello_body("slow", kToken));
  for (const char byte : framed)
  {
    REQUIRE(client.send_all(std::string(1, byte)));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  auto reply = client.receive_json();
  REQUIRE(reply["ok"] == true);
  CHECK(reply["id"] == "slow");

  // The opposite case, which a byte-at-a-time parser gets wrong in the other direction: one write
  // carrying two whole messages must produce two answers, in order.
  std::vector<char> batched;
  append_frame(batched, R"({"id":"a","method":"engine.status"})");
  append_frame(batched, R"({"id":"b","method":"engine.status"})");
  REQUIRE(client.send_all(std::string(batched.data(), batched.size())));
  const auto first = client.receive_json();
  const auto second = client.receive_json();
  REQUIRE(first["ok"] == true);
  REQUIRE(second["ok"] == true);
  CHECK(first["id"] == "a");
  CHECK(second["id"] == "b");

  // A message that is not JSON at all is answered rather than fatal, on this transport as on the
  // other: a client bug must not cost the daemon its control loop.
  client.send_frame("this is not json");
  reply = client.receive_json();
  CHECK(reply["ok"] == false);
  CHECK(reply["error"]["code"] == "bad_json");
}

// ---------------------------------------------------------------------------
// Caps
// ---------------------------------------------------------------------------

TEST_CASE("an oversized message is refused before a byte of it is buffered")
{
  Server server;
  server.config.tcp_max_message_bytes = 4096;
  REQUIRE(server.start().empty());

  Client client(server.port());
  client.send_frame(hello_body("h", kToken));
  REQUIRE(client.receive_json()["ok"] == true);

  // The length prefix alone, declaring more than the limit. The test never sends the body: the point
  // is that the daemon answers from the header, so an oversized message costs a rejection instead of
  // the buffer it asked for. A framing that read first and checked later could not answer at all
  // until it had the bytes.
  const std::uint32_t declared = 5000;
  const char header[kFrameHeaderBytes] = {static_cast<char>(declared & 0xffu),
                                          static_cast<char>((declared >> 8) & 0xffu),
                                          static_cast<char>((declared >> 16) & 0xffu),
                                          static_cast<char>((declared >> 24) & 0xffu)};
  REQUIRE(client.send_all(std::string(header, sizeof(header))));

  const auto reply = client.receive_json();
  REQUIRE(reply["ok"] == false);
  CHECK(reply["error"]["code"] == "message_too_large");
  CHECK(reply["error"]["message"].get<std::string>().find("4096") != std::string::npos);

  // A stream cannot be resynchronised after a framing error, so the connection ends with the refusal
  // rather than being left to misread the middle of a message as the start of one.
  CHECK(client.is_closed(5000));
}

TEST_CASE("a reply larger than the request limit arrives whole, and the connection stays open")
{
  Server server;
  // A request limit far below what `engine.schema` answers with. Replies are bounded apart from
  // requests, because they list what the daemon holds rather than what a client sent: a real asset
  // library makes `engine.assets` and `engine.schema` about a megabyte each, which is the default
  // request limit.
  server.config.tcp_max_message_bytes = 4096;
  REQUIRE(server.start().empty());

  Client client(server.port());
  client.send_frame(hello_body("h", kToken));
  REQUIRE(client.receive_json()["ok"] == true);

  client.send_frame(R"({"id":"s","method":"engine.schema"})");
  const auto schema = client.receive_frame();
  REQUIRE(schema.has_value());
  CHECK(schema->size() > 4096);
  const auto parsed = nlohmann::json::parse(*schema);
  CHECK(parsed["ok"] == true);
  CHECK(parsed["id"] == "s");

  // Still there to be asked again. Before replies had their own limit, this one was sent and the
  // connection closed straight after it.
  client.send_frame(R"({"id":"t","method":"engine.status"})");
  CHECK(client.receive_json()["id"] == "t");
}

TEST_CASE("the connection cap answers rather than hanging up")
{
  Server server;
  server.config.tcp_max_connections = 1;
  REQUIRE(server.start().empty());

  Client first(server.port());
  first.send_frame(hello_body("h", kToken));
  REQUIRE(first.receive_json()["ok"] == true);

  // One connection is the configured limit, so the second is told why. A refusal that says nothing is
  // indistinguishable from a daemon that is down, and the two need different fixes.
  Client second(server.port());
  const auto reply = second.receive_json();
  REQUIRE(reply["ok"] == false);
  CHECK(reply["error"]["code"] == "too_many_connections");
  CHECK(second.is_closed(5000));

  // The cap counts connections, not authenticated ones: the first is still served.
  first.send_frame(R"({"id":"s","method":"engine.status"})");
  CHECK(first.receive_json()["id"] == "s");
}

// ---------------------------------------------------------------------------
// A client that is slow, or gone
// ---------------------------------------------------------------------------

TEST_CASE("a client that stops reading does not stall the control loop")
{
  Server server;
  REQUIRE(server.start().empty());

  // The slow reader: a small receive window, authenticated, and then a flood of requests whose
  // answers it never reads. Its socket buffer fills, the kernel stops taking bytes from the daemon,
  // and every reply after that sits in the daemon's per-connection outbox.
  Client slow(server.port(), /*non_blocking=*/true, /*receive_buffer=*/2048);
  slow.send_frame(hello_body("slow", kToken));
  REQUIRE(slow.receive_json()["ok"] == true);

  const std::string request = R"({"id":"flood","method":"engine.status"})";
  std::vector<char> framed;
  append_frame(framed, request);
  const std::string flood(framed.data(), framed.size());
  int sent = 0;
  for (int i = 0; i < 250 && slow.send_all(flood); ++i)
    ++sent;
  CHECK(sent > 50); // more replies than the daemon's send buffer will hold, and more than it will take

  // Now the assertion that matters. A control loop that wrote with a blocking `send` would be sitting
  // inside that write, waiting for the slow reader, and this second client would get nothing until it
  // read. Non-blocking writes plus the outbox are what make the answer arrive anyway.
  const auto start = std::chrono::steady_clock::now();
  Client fast(server.port());
  fast.send_frame(hello_body("fast", kToken));
  const auto hello = fast.receive_json(5000);
  REQUIRE(hello["ok"] == true);
  fast.send_frame(R"({"id":"q","method":"engine.status"})");
  const auto status = fast.receive_json(5000);
  REQUIRE(status["ok"] == true);
  CHECK(status["id"] == "q");
  CHECK(milliseconds_since(start) < 5000);

  // The slow client was not disconnected for being slow: its answers are queued, and draining some of
  // them still works. (Backpressure, not a dropped connection -- it is a client, not an attacker.)
  slow.send_frame(R"({"id":"later","method":"engine.status"})");
  const auto queued = slow.receive_json(5000);
  REQUIRE(queued["ok"] == true);
  CHECK(queued["id"] == "flood"); // oldest first, which is what a queue means
}

TEST_CASE("a client that vanishes mid-message is forgotten")
{
  Server server;
  REQUIRE(server.start().empty());

  {
    Client vanishing(server.port());
    vanishing.send_frame(hello_body("v", kToken));
    REQUIRE(vanishing.receive_json()["ok"] == true);

    // A header promising a hundred bytes and ten of them, then gone: exactly the state that leaves a
    // half-assembled message behind.
    const char header[kFrameHeaderBytes] = {100, 0, 0, 0};
    REQUIRE(vanishing.send_all(std::string(header, sizeof(header)) + "0123456789"));
  } // the socket closes here

  // The slot is released, and the daemon is still answering. Polled rather than assumed, because the
  // close is noticed by the control loop, not by this thread.
  bool released = false;
  for (int attempt = 0; attempt < 200 && !released; ++attempt)
  {
    const auto status = local_call(server.config.socket_path, R"({"method":"engine.status"})");
    released = status["ok"] == true && status["status"]["control"]["tcp_connections"] == 0;
    if (!released)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK_MESSAGE(released, "the vanished client's connection was never released");

  Client next(server.port());
  next.send_frame(hello_body("n", kToken));
  CHECK(next.receive_json()["ok"] == true);
}

TEST_CASE("two concurrent clients are independent")
{
  Server server;
  REQUIRE(server.start().empty());

  Client one(server.port());
  Client two(server.port());
  one.send_frame(hello_body("one", kToken));
  two.send_frame(hello_body("two", kToken));
  REQUIRE(one.receive_json()["ok"] == true);
  REQUIRE(two.receive_json()["ok"] == true);

  // Interleaved, so a connection state kept in the daemon rather than in the connection would show up
  // as one client's id coming back to the other.
  one.send_frame(R"({"id":"1a","method":"engine.status"})");
  two.send_frame(R"({"id":"2a","method":"engine.status"})");
  one.send_frame(R"({"id":"1b","method":"engine.status"})");
  CHECK(one.receive_json()["id"] == "1a");
  CHECK(two.receive_json()["id"] == "2a");
  CHECK(one.receive_json()["id"] == "1b");

  const auto status = local_call(server.config.socket_path, R"({"method":"engine.status"})");
  CHECK(status["status"]["control"]["tcp_connections"] == 2);
  CHECK(status["status"]["control"]["tcp_port"] == server.port());
}

// ---------------------------------------------------------------------------
// One protocol, two transports
// ---------------------------------------------------------------------------

TEST_CASE("the same request over either transport gives the same answer")
{
  Server server;
  REQUIRE(server.start().empty());

  Client remote(server.port());
  remote.send_frame(hello_body("r", kToken));
  const auto remote_hello = remote.receive_json();

  const auto local_hello = local_call(server.config.socket_path, R"({"id":"r","method":"engine.hello"})");

  // The framing and the token are the difference; the body is not. `transport` and `authenticated`
  // are the only fields a client should see differ, and a local session is authorised from accept.
  CHECK(remote_hello["protocol"] == local_hello["protocol"]);
  CHECK(remote_hello["version"] == local_hello["version"]);
  CHECK(remote_hello["features"] == local_hello["features"]);
  CHECK(local_hello["transport"] == "unix");
  CHECK(local_hello["authenticated"] == true);

  const char* const status_request = R"({"method":"engine.status"})";
  remote.send_frame(status_request);
  const auto remote_status = remote.receive_json();
  const auto local_status = local_call(server.config.socket_path, status_request);
  CHECK(remote_status["status"]["sample_rate"] == local_status["status"]["sample_rate"]);
  CHECK(remote_status["status"]["backend"] == local_status["status"]["backend"]);
  CHECK(remote_status["status"]["max_chains"] == local_status["status"]["max_chains"]);
}

TEST_CASE("a network client may not name an absolute path, and a local one may")
{
  Server server;
  REQUIRE(server.start().empty());

  const auto document = [](const char* model) {
    nlohmann::json body;
    body["name"] = "absolute";
    body["chains"] = nlohmann::json::array();
    nlohmann::json chain;
    chain["id"] = "main";
    chain["nodes"] = {{{"id", "in"}, {"type", "nc.input"}},
                      {{"id", "amp"}, {"type", "nc.nam"}, {"model", model}},
                      {{"id", "out"}, {"type", "nc.output"}}};
    chain["connections"] = {{{"from", "in"}, {"to", "amp"}}, {{"from", "amp"}, {"to", "out"}}};
    body["chains"].push_back(std::move(chain));

    nlohmann::json request;
    request["method"] = "rig.apply";
    request["document"] = std::move(body);
    return request.dump();
  };

  // The local operator is the one who chose this daemon's filesystem; an absolute path is theirs.
  const auto local = local_call(server.config.socket_path, document("/etc/hostname"));
  CHECK(local["ok"] == false);
  CHECK(local["error"]["code"] != "path_refused"); // it fails later, on the model itself

  // A network client is not the operator, and an absolute path here is a file the daemon will open on
  // request. Refused before anything is parsed for the plan, with the path quoted back.
  Client remote(server.port());
  remote.send_frame(hello_body("h", kToken));
  REQUIRE(remote.receive_json()["ok"] == true);
  remote.send_frame(document("/etc/hostname"));
  const auto refused = remote.receive_json();
  REQUIRE(refused["ok"] == false);
  CHECK(refused["error"]["code"] == "path_refused");
  CHECK(refused["error"]["message"].get<std::string>().find("/etc/hostname") != std::string::npos);

  // The same rule on a patch, because `swap_model` names a path too.
  remote.send_frame(R"({"method":"rig.patch","ops":[{"op":"swap_model","node":"g","model":"/etc/hostname"}]})");
  const auto patched = remote.receive_json();
  REQUIRE(patched["ok"] == false);
  CHECK(patched["error"]["code"] == "path_refused");
}

TEST_CASE("a network client names models by id, because a path is refused")
{
  Server server;
  // The generated fixtures are a real library: one WaveNet model and two more .nam files. The
  // registry only reads headers, so pointing it at the build's fixture directory costs nothing.
  server.config.models_root = NAMCHAIN_TEST_DATA_DIR;
  server.config.irs_root = NAMCHAIN_TEST_DATA_DIR;
  REQUIRE(server.start().empty());

  Client client(server.port());
  client.send_frame(hello_body("h", kToken));
  REQUIRE(client.receive_json()["ok"] == true);

  // The library as a network client sees it: ids, not paths to depend on. It also carries `path`,
  // which is what an operator looking at a local daemon wants -- and what a client must *not* put in
  // a document, which the rest of this case is about.
  client.send_frame(R"({"method":"engine.assets"})");
  const auto assets = client.receive_json();
  REQUIRE(assets["ok"] == true);
  REQUIRE(assets["models"].size() >= 1);
  bool found = false;
  for (const auto& model : assets["models"])
    found = found || model["id"] == "A2-Lite";
  CHECK(found);

  const auto document = [](const char* model) {
    nlohmann::json body;
    body["name"] = "by id";
    body["chains"] = nlohmann::json::array();
    nlohmann::json chain;
    chain["id"] = "main";
    chain["nodes"] = {{{"id", "in"}, {"type", "nc.input"}},
                      {{"id", "amp"}, {"type", "nc.nam"}, {"model", model}},
                      {{"id", "out"}, {"type", "nc.output"}}};
    chain["connections"] = {{{"from", "in"}, {"to", "amp"}}, {{"from", "amp"}, {"to", "out"}}};
    body["chains"].push_back(std::move(chain));

    nlohmann::json request;
    request["method"] = "rig.apply";
    request["document"] = std::move(body);
    return request.dump();
  };

  // The id compiles into a plan: the engine resolved it to a file under its own models root, which is
  // the whole reason a client never has to know where that root is.
  client.send_frame(document("A2-Lite"));
  const auto applied = client.receive_json();
  REQUIRE(applied["ok"] == true);
  CHECK(applied["generation"].get<uint64_t>() >= 1);

  // An id that is not in the library is refused with the near miss, over this transport as over the
  // other -- and the refusal is about the library, not about permissions.
  client.send_frame(document("A2-Lit"));
  const auto missing = client.receive_json();
  REQUIRE(missing["ok"] == false);
  CHECK(missing["error"]["code"] == "asset_missing");
  CHECK(missing["error"]["message"].get<std::string>().find("A2-Lite") != std::string::npos);

  // The path that would work locally is refused here, and the id that works here is the answer: this
  // is the trade a network client makes, in one place.
  const std::string in_library =
    (std::filesystem::path(NAMCHAIN_TEST_DATA_DIR) / "A2-Lite.nam").string();
  client.send_frame(document(in_library.c_str()));
  const auto refused = client.receive_json();
  REQUIRE(refused["ok"] == false);
  CHECK(refused["error"]["code"] == "path_refused");
}
