// namchainctl: the reference client for the daemon's control protocol.
//
// Two jobs, and the second is the reason it exists. The first is to be the thing an operator reaches
// for when they want to know what the daemon is doing without opening a socket by hand. The second is
// to be the executable documentation for whoever writes the Swift client: the framing, the token
// handshake, the request shapes and the error handling are all here, in a file short enough to read in
// one sitting, over both transports.
//
// It is deliberately not a library. The engine's copy of the protocol lives in the daemon and is
// tested there; a client that shares code with the server proves less about the server.
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <json.hpp>

#include "namchain/daemon/framing.h"
#include "namchain/daemon/platform.h"
#include "namchain/version.h"

namespace
{

using namchain::daemon::append_frame;
using namchain::daemon::frame_length;
using namchain::daemon::kFrameHeaderBytes;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;  ///< the command line, or the connection, did not work
constexpr int kExitRefused = 2; ///< the daemon answered, and said no

/// The receive buffer for one response. Generous, because the largest answer (`rig.get` on a large
/// document) is the one an operator asks for while something is wrong.
constexpr std::size_t kReceiveBytes = 4u << 20;

void print_usage(std::FILE* stream)
{
  std::fprintf(
    stream,
    "namchainctl %s -- talk to namchaind\n"
    "\n"
    "usage: namchainctl [options] <command> [command options]\n"
    "\n"
    "transport:\n"
    "  --socket PATH          Unix control socket (default /run/namchain/namchaind.sock)\n"
    "  --host HOST            use TCP instead, e.g. 127.0.0.1 or studio.local\n"
    "  --port N               TCP port (default 47810)\n"
    "  --token TOKEN          the transport token a TCP daemon requires\n"
    "  --token-file FILE      read the token from FILE (first line); preferred, because a token on\n"
    "                         the command line is visible to every local user in `ps`\n"
    "\n"
    "commands:\n"
    "  hello                  negotiate the protocol and print what the daemon offers\n"
    "  status                 the device, the document, the chains, and engine health\n"
    "  schema                 the node and parameter catalogue (see docs/schema.md)\n"
    "  assets [--refresh]     the model and impulse-response library as ids, optionally rescanning it\n"
    "                         first (the scan is cached by size and mtime, so this is cheap)\n"
    "  get                    the document the engine is running, as the engine parsed it\n"
    "  apply FILE             apply a whole document (FILE, or - for stdin)\n"
    "  patch FILE             apply a patch: either an ops array or {\"chain\":..., \"ops\":[...]}\n"
    "  set --node N --param P --value V [--node-port N] [--chain C]\n"
    "                         the common case: one parameter, one commit\n"
    "  mute [on|off] [--chain C]   mute the engine, or one chain\n"
    "  panic                  mute unconditionally\n"
    "\n"
    "output:\n"
    "  --compact              one line of JSON instead of the indented form\n"
    "  --quiet                nothing on success (the exit code is the answer)\n"
    "  -h, --help             this text\n"
    "  --version              print the version and exit\n"
    "\n"
    "Exit codes: 0 the daemon said yes; 1 the command line or the connection failed; 2 the daemon\n"
    "answered with an error (its code and message go to stderr).\n",
    NAMCHAIN_VERSION_STRING);
}

/// \brief One connection to the daemon, over either transport.
///
/// The only difference the caller sees is whether a message is framed. Everything above this is the
/// same JSON on both, which is the property the two transports are supposed to have and the thing a
/// reference client should demonstrate rather than assert.
struct Transport
{
  int fd = -1;
  bool framed = false;

  ~Transport() { close(); }

  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;
  Transport() = default;

  void close()
  {
    if (fd >= 0)
      ::close(fd);
    fd = -1;
  }

  bool connect_unix(const std::string& path, std::string& error)
  {
    fd = ::socket(AF_UNIX, namchain::daemon::platform::local_socket_type(), 0);
    if (fd < 0)
    {
      error = std::string("socket: ") + std::strerror(errno);
      return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
    {
      error = "socket path is too long: " + path;
      return false;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
      error = "connect " + path + ": " + std::strerror(errno);
      return false;
    }
    // Datagrams where the local socket is SOCK_SEQPACKET, the length prefix where it is a
    // stream -- the same choice the daemon makes, from the same constant, so the two ends
    // cannot disagree about it (namchain/daemon/platform.h).
    framed = namchain::daemon::platform::local_transport_is_stream;
    return true;
  }

  bool connect_tcp(const std::string& host, unsigned port, std::string& error)
  {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* candidates = nullptr;
    const std::string service = std::to_string(port);
    const int resolved = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &candidates);
    if (resolved != 0)
    {
      error = "cannot resolve '" + host + "': " + ::gai_strerror(resolved);
      return false;
    }
    std::string last;
    for (addrinfo* candidate = candidates; candidate != nullptr; candidate = candidate->ai_next)
    {
      fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
      if (fd < 0)
      {
        last = std::strerror(errno);
        continue;
      }
      if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0)
        break;
      last = std::strerror(errno);
      ::close(fd);
      fd = -1;
    }
    ::freeaddrinfo(candidates);
    if (fd < 0)
    {
      error = "connect " + host + ":" + service + ": " + (last.empty() ? "no usable address" : last);
      return false;
    }
    framed = true;
    return true;
  }

  bool send_message(const std::string& body, std::string& error)
  {
    std::vector<char> bytes;
    if (framed)
      append_frame(bytes, body);
    else
      bytes.assign(body.begin(), body.end());

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
      error = std::string("send: ") + std::strerror(errno);
      return false;
    }
    return true;
  }

  /// Read exactly \p size bytes, because a stream does not preserve the boundaries the sender wrote.
  bool read_exactly(char* data, std::size_t size, std::string& error)
  {
    std::size_t got = 0;
    while (got < size)
    {
      const ssize_t bytes = ::recv(fd, data + got, size - got, 0);
      if (bytes > 0)
      {
        got += static_cast<std::size_t>(bytes);
        continue;
      }
      if (bytes < 0 && errno == EINTR)
        continue;
      error = bytes == 0 ? "the daemon closed the connection" : std::string("recv: ") + std::strerror(errno);
      return false;
    }
    return true;
  }

  bool receive_message(std::string& body, std::string& error)
  {
    if (framed)
    {
      char header[kFrameHeaderBytes];
      if (!read_exactly(header, sizeof(header), error))
        return false;
      const std::uint32_t length = frame_length(header);
      if (length > kReceiveBytes)
      {
        error = "the daemon sent a " + std::to_string(length) + " byte message";
        return false;
      }
      body.assign(length, '\0');
      return length == 0 || read_exactly(body.data(), length, error);
    }

    std::vector<char> buffer(kReceiveBytes);
    const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (got <= 0)
    {
      error = got == 0 ? "the daemon closed the connection" : std::string("recv: ") + std::strerror(errno);
      return false;
    }
    body.assign(buffer.data(), static_cast<std::size_t>(got));
    return true;
  }
};

std::string read_file(const std::string& path, bool& ok)
{
  ok = true;
  if (path == "-")
  {
    std::ostringstream contents;
    contents << std::cin.rdbuf();
    return contents.str();
  }
  std::ifstream file(path);
  if (!file)
  {
    ok = false;
    return {};
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

/// The protocol method a command name stands for. The command line is a shorthand (`status`), the
/// protocol is namespaced (`engine.status`), and this table is the whole of the difference -- kept
/// explicit rather than derived, because a method name guessed from a word is how a client ends up
/// sending `{"method":"hello"}` and being told, correctly, that there is no such thing.
const char* method_for(const std::string& command)
{
  if (command == "hello")
    return "engine.hello";
  if (command == "status")
    return "engine.status";
  if (command == "schema")
    return "engine.schema";
  if (command == "assets")
    return "engine.assets";
  if (command == "get")
    return "rig.get";
  if (command == "apply")
    return "rig.apply";
  if (command == "patch" || command == "set")
    return "rig.patch";
  if (command == "mute")
    return "engine.mute";
  if (command == "panic")
    return "engine.panic";
  return nullptr;
}

/// A parameter value as the user typed it: a number or a boolean if it parses as one, a string
/// otherwise. `--value delay` is a mode; `--value 0.7` is a number, and neither needs a flag.
nlohmann::json parse_value(const std::string& text)
{
  nlohmann::json parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!parsed.is_discarded() && (parsed.is_number() || parsed.is_boolean() || parsed.is_null()))
    return parsed;
  return text;
}

/// The command line, parsed into global options and a command with its own options. One flat pass,
/// because the daemon's own front end is one flat pass and two dialects for one program is a bug
/// waiting to be reported.
struct Options
{
  std::string socket_path = "/run/namchain/namchaind.sock";
  std::string host;
  std::string token;
  std::string token_file;
  unsigned port = 47810;
  bool compact = false;
  bool quiet = false;
  bool refresh = false;

  std::string command;
  std::vector<std::string> positional;
  std::string chain;
  std::string node;
  std::string param;
  std::string value;
  int node_port = -1;
};

} // namespace

int main(int argc, char** argv)
{
  Options options;
  std::vector<std::string> words;
  for (int i = 1; i < argc; ++i)
    words.push_back(argv[i]);

  for (std::size_t i = 0; i < words.size(); ++i)
  {
    const std::string& word = words[i];
    if (word == "-h" || word == "--help")
    {
      print_usage(stdout);
      return kExitOk;
    }
    if (word == "--version")
    {
      std::printf("namchainctl %s\n", NAMCHAIN_VERSION_STRING);
      return kExitOk;
    }
    const auto value_of = [&](std::string& out) {
      if (i + 1 >= words.size())
      {
        std::fprintf(stderr, "namchainctl: %s needs a value\n", word.c_str());
        return false;
      }
      out = words[++i];
      return true;
    };

    if (word == "--compact")
      options.compact = true;
    else if (word == "--refresh")
      options.refresh = true;
    else if (word == "--quiet")
      options.quiet = true;
    else if (word == "--socket")
    {
      if (!value_of(options.socket_path))
        return kExitUsage;
    }
    else if (word == "--host")
    {
      if (!value_of(options.host))
        return kExitUsage;
    }
    else if (word == "--token")
    {
      if (!value_of(options.token))
        return kExitUsage;
    }
    else if (word == "--token-file")
    {
      if (!value_of(options.token_file))
        return kExitUsage;
    }
    else if (word == "--port")
    {
      std::string text;
      if (!value_of(text))
        return kExitUsage;
      options.port = static_cast<unsigned>(std::strtoul(text.c_str(), nullptr, 10));
    }
    else if (word == "--chain")
    {
      if (!value_of(options.chain))
        return kExitUsage;
    }
    else if (word == "--node")
    {
      if (!value_of(options.node))
        return kExitUsage;
    }
    else if (word == "--param")
    {
      if (!value_of(options.param))
        return kExitUsage;
    }
    else if (word == "--value")
    {
      if (!value_of(options.value))
        return kExitUsage;
    }
    else if (word == "--node-port")
    {
      std::string text;
      if (!value_of(text))
        return kExitUsage;
      options.node_port = std::atoi(text.c_str());
    }
    else if (!word.empty() && word[0] == '-' && word != "-")
    {
      std::fprintf(stderr, "namchainctl: unknown option '%s' (try --help)\n", word.c_str());
      return kExitUsage;
    }
    else if (options.command.empty())
      options.command = word;
    else
      options.positional.push_back(word);
  }

  if (options.command.empty())
  {
    print_usage(stderr);
    return kExitUsage;
  }

  if (!options.token_file.empty())
  {
    bool ok = false;
    const std::string contents = read_file(options.token_file, ok);
    if (!ok)
    {
      std::fprintf(stderr, "namchainctl: cannot read token file '%s'\n", options.token_file.c_str());
      return kExitUsage;
    }
    options.token = contents.substr(0, contents.find('\n'));
    while (!options.token.empty() && (options.token.back() == '\r' || options.token.back() == ' '))
      options.token.pop_back();
  }
  if (!options.host.empty() && options.token.empty())
  {
    std::fprintf(stderr, "namchainctl: a TCP daemon requires --token or --token-file\n");
    return kExitUsage;
  }

  // --- build the request ---------------------------------------------------
  const char* const method = method_for(options.command);
  if (method == nullptr)
  {
    std::fprintf(stderr, "namchainctl: unknown command '%s' (try --help)\n", options.command.c_str());
    return kExitUsage;
  }

  nlohmann::json request;
  request["method"] = method;
  if (!options.chain.empty())
    request["chain"] = options.chain;

  const auto needs_one_argument = [&](const char* what) {
    if (options.positional.size() != 1)
    {
      std::fprintf(stderr, "namchainctl: %s needs exactly one %s\n", options.command.c_str(), what);
      return false;
    }
    return true;
  };

  if (options.command == "hello" || options.command == "status" || options.command == "schema" ||
      options.command == "get" || options.command == "panic" || options.command == "assets")
  {
    // No arguments; the method is the whole request, except for `assets --refresh`, which asks the
    // daemon to look at the disk again rather than answer from its scan.
    if (options.refresh)
      request["refresh"] = true;
  }
  else if (options.command == "apply")
  {
    if (!needs_one_argument("file"))
      return kExitUsage;
    bool ok = false;
    const std::string text = read_file(options.positional[0], ok);
    if (!ok)
    {
      std::fprintf(stderr, "namchainctl: cannot read '%s'\n", options.positional[0].c_str());
      return kExitUsage;
    }
    nlohmann::json document = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded())
    {
      std::fprintf(stderr, "namchainctl: '%s' is not valid JSON\n", options.positional[0].c_str());
      return kExitUsage;
    }
    request["document"] = std::move(document);
  }
  else if (options.command == "patch")
  {
    if (!needs_one_argument("file"))
      return kExitUsage;
    bool ok = false;
    const std::string text = read_file(options.positional[0], ok);
    if (!ok)
    {
      std::fprintf(stderr, "namchainctl: cannot read '%s'\n", options.positional[0].c_str());
      return kExitUsage;
    }
    nlohmann::json patch = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (patch.is_discarded())
    {
      std::fprintf(stderr, "namchainctl: '%s' is not valid JSON\n", options.positional[0].c_str());
      return kExitUsage;
    }
    // Either a bare ops array or a whole rig.patch body, so a patch can be kept as a file that is
    // already the request.
    if (patch.is_array())
      request["ops"] = std::move(patch);
    else if (patch.is_object() && patch.contains("ops"))
    {
      request["ops"] = patch["ops"];
      if (patch.contains("chain") && !request.contains("chain"))
        request["chain"] = patch["chain"];
    }
    else
    {
      std::fprintf(stderr, "namchainctl: a patch file is an ops array, or an object with 'ops'\n");
      return kExitUsage;
    }
  }
  else if (options.command == "set")
  {
    if (options.node.empty() || options.param.empty() || options.value.empty())
    {
      std::fprintf(stderr, "namchainctl: set needs --node, --param and --value\n");
      return kExitUsage;
    }
    nlohmann::json op;
    op["op"] = "set_param";
    op["node"] = options.node;
    op["param"] = options.param;
    op["value"] = parse_value(options.value);
    if (options.node_port >= 0)
      op["port"] = options.node_port;
    request["ops"] = nlohmann::json::array({std::move(op)});
  }
  else if (options.command == "mute")
  {
    bool muted = true;
    if (!options.positional.empty())
    {
      const std::string& word = options.positional[0];
      if (word == "on" || word == "true" || word == "1")
        muted = true;
      else if (word == "off" || word == "false" || word == "0")
        muted = false;
      else
      {
        std::fprintf(stderr, "namchainctl: mute takes on or off, not '%s'\n", word.c_str());
        return kExitUsage;
      }
      if (options.positional.size() > 1)
      {
        std::fprintf(stderr, "namchainctl: mute takes at most one argument\n");
        return kExitUsage;
      }
    }
    request["mute"] = muted;
  }
  else
  {
    std::fprintf(stderr, "namchainctl: unknown command '%s' (try --help)\n", options.command.c_str());
    return kExitUsage;
  }

  // --- connect, negotiate, run ---------------------------------------------
  Transport transport;
  std::string error;
  const bool connected = options.host.empty() ? transport.connect_unix(options.socket_path, error)
                                              : transport.connect_tcp(options.host, options.port, error);
  if (!connected)
  {
    std::fprintf(stderr, "namchainctl: %s\n", error.c_str());
    return kExitUsage;
  }

  // `engine.hello` is both the handshake and a command. A client that is not asking for it still has
  // to send it on TCP -- an unauthenticated connection may not call anything else -- and on the Unix
  // socket it costs one round trip to learn the daemon's protocol version before sending a request
  // that assumes one.
  std::string reply;
  nlohmann::json hello;
  hello["method"] = "engine.hello";
  hello["protocol"] = 2;
  if (!options.token.empty())
    hello["token"] = options.token;

  const bool asked_for_hello = options.command == "hello";
  if (asked_for_hello)
  {
    request["token"] = options.token;
  }
  else
  {
    if (!transport.send_message(hello.dump(), error) || !transport.receive_message(reply, error))
    {
      std::fprintf(stderr, "namchainctl: %s\n", error.c_str());
      return kExitUsage;
    }
    const auto handshake = nlohmann::json::parse(reply, nullptr, false);
    if (handshake.is_discarded() || handshake["ok"] != true)
    {
      // The code as well as the message, in the same shape as a refused command: "unauthorized" is
      // what a script branches on, and the prose is what a person reads.
      if (handshake.is_discarded())
        std::fprintf(stderr, "namchainctl: the daemon sent something that is not JSON: %s\n", reply.c_str());
      else
        std::fprintf(stderr, "namchainctl: handshake refused: %s: %s\n",
                     handshake["error"]["code"].get<std::string>().c_str(),
                     handshake["error"]["message"].get<std::string>().c_str());
      return kExitRefused;
    }
  }

  if (!transport.send_message(request.dump(), error) || !transport.receive_message(reply, error))
  {
    std::fprintf(stderr, "namchainctl: %s\n", error.c_str());
    return kExitUsage;
  }

  const nlohmann::json response = nlohmann::json::parse(reply, nullptr, false);
  if (response.is_discarded())
  {
    std::fprintf(stderr, "namchainctl: the daemon sent something that is not JSON: %s\n", reply.c_str());
    return kExitUsage;
  }
  if (response["ok"] != true)
  {
    std::fprintf(stderr, "namchainctl: %s: %s\n", response["error"]["code"].get<std::string>().c_str(),
                 response["error"]["message"].get<std::string>().c_str());
    return kExitRefused;
  }
  if (!options.quiet)
    std::cout << response.dump(options.compact ? -1 : 2) << "\n";
  return kExitOk;
}
