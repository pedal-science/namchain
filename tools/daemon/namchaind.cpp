// namchaind: the reference daemon (plan.md §8.2).
//
// A thin front end: parse the configuration, hand it to namchain::daemon::Daemon, and let the
// daemon own the threads. Everything testable lives in the library so the unit tests do not have to
// spawn a process; what is here is argument handling and the exit code.
#include <cstdio>
#include <cstring>
#include <string>

#include "namchain/daemon/config.h"
#include "namchain/daemon/daemon.h"
#include "namchain/version.h"

namespace
{

void print_usage(std::FILE* stream)
{
  std::fprintf(stream,
               "namchaind %s -- the namchain reference daemon\n"
               "\n"
               "usage: namchaind [--config FILE] [options]\n"
               "\n"
               "  -c, --config FILE            read options from a JSON file first\n"
               "      --backend NAME           alsa_raw | null | file | pipewire (default alsa_raw)\n"
               "      --device SPEC            ALSA device, e.g. hw:4,0\n"
               "      --sample-rate HZ         default 48000\n"
               "      --period-size FRAMES     default 64\n"
               "      --periods N              device buffer length in periods, default 4\n"
               "      --capture-channels N     default 1\n"
               "      --playback-channels N    default 1\n"
               "      --rig PATH               startup rig; a bare name resolves under --rigs-root\n"
               "      --rigs-root DIR          root for rig.load names (default /etc/namchain/rigs)\n"
               "      --models-root DIR        the device's .nam model library (default /etc/namchain/models)\n"
               "      --irs-root DIR           the device's impulse responses (default /etc/namchain/irs)\n"
               "      --socket PATH            control socket (default /run/namchain/namchaind.sock)\n"
               "      --socket-mode OCTAL      socket permissions, default 0660\n"
               "      --tcp-listen             also listen for control connections on TCP (off by default)\n"
               "      --tcp-address ADDR       TCP address to bind; numeric only (default 127.0.0.1)\n"
               "      --tcp-port N             TCP port, or 0 to pick one (default 47810)\n"
               "      --tcp-token TOKEN        pre-shared token a TCP client must present in engine.hello\n"
               "      --tcp-token-file FILE    read the token from FILE (first line); preferred on a unit,\n"
               "                               because a token on the command line is visible in `ps`\n"
               "      --tcp-max-connections N  TCP connections served at once, default 4\n"
               "      --tcp-max-message-bytes N  largest framed message accepted, default 1048576\n"
               "      --tcp-auth-timeout-ms N  how long a connection may stay unauthenticated, default 10000\n"
               "      --rt-priority N          render-thread SCHED_FIFO priority, default 80; 0 leaves it\n"
               "      --cpu N                  pin the render thread to one CPU; -1 leaves it (default)\n"
               "      --lock-memory            mlockall(MCL_CURRENT) (default)\n"
               "      --no-lock-memory         do not lock memory\n"
               "      --output-queue-periods N output ring cap in periods; 0 = none (default)\n"
               "      --output-queue-floor-periods N  queue floor in periods; 0 = none (default)\n"
               "      --fade-ms X              structural crossfade length, default 5\n"
               "      --device-retry-ms N      retry backoff after a lost device, default 250\n"
               "      --reclamation-timeout-ms N  control poll timeout, default 20\n"
               "  -v, --verbose                log lifecycle events to stderr\n"
               "  -h, --help                   this text\n"
               "      --version                print the version and exit\n"
               "\n"
               "The daemon survives the audio interface being unplugged: the render thread closes the\n"
               "device, reports state \"lost\", and reopens it as soon as it is back. Use\n"
               "`systemctl reload namchaind` (SIGHUP) to re-read the configured rig.\n"
               "\n"
               "TCP control is off unless --tcp-listen is given, and refuses to start without a token:\n"
               "an unauthenticated port that can rewrite the rig and mute the output is worse than a\n"
               "daemon that will not start. See docs/daemon.md for the transport and its threat model.\n",
               NAMCHAIN_VERSION_STRING);
}

} // namespace

int main(int argc, char** argv)
{
  namchain::daemon::Config config;
  std::string config_path;

  // First pass: only what changes how the rest of the line is read.
  for (int i = 1; i < argc; ++i)
  {
    const std::string argument = argv[i];
    if (argument == "--help" || argument == "-h")
    {
      print_usage(stdout);
      return 0;
    }
    if (argument == "--version")
    {
      std::printf("namchaind %s\n", NAMCHAIN_VERSION_STRING);
      return 0;
    }
    if (argument == "--config" || argument == "-c")
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "namchaind: --config needs a path\n");
        return 2;
      }
      config_path = argv[++i];
    }
  }

  if (!config_path.empty())
  {
    std::string error;
    if (!namchain::daemon::load_config_file(config_path, config, error))
    {
      std::fprintf(stderr, "namchaind: %s\n", error.c_str());
      return 2;
    }
  }

  // Second pass: everything else overrides what the file set, in the order given.
  for (int i = 1; i < argc; ++i)
  {
    const std::string argument = argv[i];
    if (argument == "--config" || argument == "-c")
    {
      ++i; // already read above
      continue;
    }
    if (argument.rfind("--", 0) != 0 && argument != "-v")
    {
      std::fprintf(stderr, "namchaind: unexpected argument '%s' (try --help)\n", argument.c_str());
      return 2;
    }

    std::string name = argument == "-v" ? "verbose" : argument.substr(2);
    std::string value;
    const bool takes_no_value = name == "verbose" || name == "lock-memory" || name == "no-lock-memory" ||
                                name == "tcp-listen";
    if (!takes_no_value)
    {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "namchaind: --%s needs a value\n", name.c_str());
        return 2;
      }
      value = argv[++i];
    }

    std::string error;
    if (!namchain::daemon::apply_option(config, name, value, error))
    {
      std::fprintf(stderr, "namchaind: %s\n", error.c_str());
      return 2;
    }
  }

  namchain::daemon::Daemon daemon(config);
  std::string error;
  if (!daemon.start(error))
  {
    std::fprintf(stderr, "namchaind: %s\n", error.c_str());
    return 1;
  }
  return daemon.run();
}
