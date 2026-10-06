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
               "      --backend NAME           alsa_raw | coreaudio | null | file | pipe | pipewire\n"
               "                               (default alsa_raw)\n"
               "      --device SPEC            the device to open: an ALSA spec (hw:4,0), or a CoreAudio\n"
               "                               device UID for coreaudio. Empty = the system default\n"
               "      --capture-path FILE      `file`, `pipe` and `coreaudio`: the WAV to read as input.\n"
               "                               `pipe` and `coreaudio` loop it; `file` stops at its end\n"
               "      --input-device UID       coreaudio: capture from this CoreAudio input device\n"
               "                               instead of a file. Empty = --capture-path is the source,\n"
               "                               and giving both is refused\n"
               "      --device-buffer-frames N coreaudio: ask the device to use an N-frame IO buffer\n"
               "                               (32 is a good guitar-monitoring figure, 512 is common);\n"
               "                               the previous value is restored on stop\n"
               "      --capture-device-path FILE  coreaudio, when built with capture: write what the\n"
               "                               device is sent, as a WAV, for a null test of that path\n"
               "      --playback-path FILE     `file`: the output WAV, written when the input runs\n"
               "                               out. `pipe`: a FIFO the output is streamed to as raw\n"
               "                               interleaved float32, for a player to read\n"
               "      --sample-rate HZ         default 48000\n"
               "      --period-size FRAMES     default 64\n"
               "      --periods N              device buffer length in periods, default 4\n"
               "      --capture-channels N     default 1\n"
               "      --playback-channels N    default 1\n"
               "      --rig PATH               startup rig; a bare name resolves under --rigs-root\n"
               "      --rigs-root DIR          root for rig.load names (default /etc/namchain/rigs)\n"
               "      --models-root DIR        the device's .nam model library (default /etc/namchain/models)\n"
               "      --irs-root DIR           the device's impulse responses (default /etc/namchain/irs)\n"
               "      --record-dir DIR         where engine.record writes; empty disables it\n"
               "                               (default /var/lib/namchain/recordings)\n"
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
               "      --render-threads N       cores the render path uses, from --cpu upwards; 1 = one\n"
               "                               thread (default), N > 1 shares the chains out over N\n"
               "      --lock-memory            mlockall(MCL_CURRENT) (default)\n"
               "      --no-lock-memory         do not lock memory\n"
               "      --output-queue-periods N output ring cap in periods; 0 = none (default)\n"
               "      --output-queue-floor-periods N  queue floor in periods; 0 = none (default)\n"
               "      --fade-ms X              structural crossfade length, default 5\n"
               "      --model-budget-kb N      what a document's models may read per block, together;\n"
               "                               over it, slimmable models are slimmed. 0 = off (default)\n"
               "      --levels                 measure audio levels for engine.levels and the meters\n"
               "                               (default). --no-levels builds no probes at all: the\n"
               "                               render path then folds nothing per sample, engine.levels\n"
               "                               is refused, and `features` omits \"levels\"\n"
               "      --no-levels              do not measure audio levels\n"
               "      --levels-rate-hz X       drain the level probes at most X times a second and answer\n"
               "                               a read in between from the held value; 0 = every read is a\n"
               "                               real measurement. Default 8, which is half what a 20 Hz\n"
               "                               meter can show and keeps the conversions off the control\n"
               "                               thread's critical path; lower it for freshness, raise it\n"
               "                               for a client that wants every window\n"
               "      --no-levels-rate-hz      no limit: every read is a real measurement\n"
               "      --levels-first-frame     measure one frame per period instead of every frame. An\n"
               "                               experiment switch: it cuts the per-sample cost by the\n"
               "                               period length and makes a meter miss transients\n"
               "      --no-levels-first-frame  measure every frame (default)\n"
               "      --levels-publish-period  fold samples into private memory and add the window to\n"
               "                               the probe once per period, instead of writing the probe's\n"
               "                               atomics per sample (default). The meter reads the same\n"
               "                               thing; the render thread stops sharing cache lines with it\n"
               "      --no-levels-publish-period  write the probe per sample: what every build before\n"
               "                               2026-10-06 did\n"
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
               "The `pipe` backend is for auditioning a rig on a machine with no interface: it loops\n"
               "--capture-path and writes to --playback-path, which should be a FIFO. It is paced by\n"
               "whatever is reading that FIFO, so a listener leaving reads as a lost device and the\n"
               "next one to attach picks it up again -- and block times measured against it are the\n"
               "listener's scheduling, not the engine's.\n"
               "\n"
               "The `coreaudio` backend is the same idea in one process, on macOS: it plays the\n"
               "engine's output through --device (a CoreAudio UID, or the default output device when\n"
               "empty) and takes its capture from --capture-path, looped, or from --input-device, a live\n"
               "CoreAudio input. A live input is the one thing here that makes macOS ask for microphone\n"
               "permission, which is the spawner's decision and not a side effect of running a daemon, so\n"
               "it happens only when the flag is given. When the input and the output are different\n"
               "devices the backend makes a private aggregate of the two, because two devices have two\n"
               "clocks and this engine has no resampler. The device's own clock paces the engine, so\n"
               "block times here are the engine's work plus the device's buffer. --period-size is a\n"
               "*ceiling* rather than a request: the backend measures the number of frames the device\n"
               "actually hands its callback -- the smallest it sees -- compiles the plan for that, and\n"
               "refuses a device that wants more than the ceiling. It writes no device property: the\n"
               "rate and the buffer size are read and never changed, so there is nothing to put back\n"
               "when it exits.\n"
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
                                name == "tcp-listen" || name == "levels" || name == "no-levels" ||
                                name == "levels-first-frame" || name == "no-levels-first-frame" ||
                                name == "levels-publish-period" || name == "no-levels-publish-period" ||
                                name == "no-levels-rate-hz";
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
