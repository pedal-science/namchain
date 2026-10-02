#pragma once

// namchaind's configuration surface (plan.md §8.2): a JSON file for the appliance, overridden by
// command-line flags for a one-off run. Both spell the same fields, so what the unit passes and what
// an operator types are the same vocabulary rather than two dialects.
#include <string>

#include "namchain/io/backend.h"

namespace namchain
{
namespace daemon
{

/// \brief Everything namchaind needs before it opens a device or a socket.
///
/// Defaults describe the M1 reference host: raw ALSA, 48 kHz, a 64-frame period, one channel each
/// way, RT priority 80, memory locked. A field that is deliberately empty (`device`, `rig`) means
/// "whatever the platform chooses" or "start silent and wait for a rig.load", not a missing value.
struct Config
{
  BackendKind backend = BackendKind::AlsaRaw;
  /// Device spec for the device backends: "hw:CARD=<id>", "hw:<id>,<d>", "hw:4,0", "4,0", or a /dev/snd path. Empty = platform
  /// default. Ignored by `null` and `file`.
  std::string device;
  unsigned sample_rate = 48000;
  unsigned period_size = 64;
  unsigned periods = 4;
  unsigned capture_channels = 1;
  unsigned playback_channels = 1;
  /// The `file` backend's input and output WAVs. Empty for the device backends, which use \ref device.
  ///
  /// This is what makes `namchaind --backend file` an offline render of a whole document -- every
  /// chain, its channel binding and its summing included -- rather than something only the soak tool
  /// could do, and it is how the multi-chain channel tests assert where each chain's audio landed.
  std::string capture_path;
  std::string playback_path;
  /// The startup rig, read synchronously before the clock starts (rig-pipeline.md §5.1 item 2). An
  /// absolute path is taken as written; a bare name resolves under \ref rigs_root. Empty means the
  /// daemon starts silent and waits for a `rig.load`.
  std::string rig;
  /// The directory a `rig.load` name is resolved against. A protocol client can name a rig but can
  /// never name a path outside this directory (rig-pipeline.md §5.1 item 1).
  std::string rigs_root = "/etc/namchain/rigs";
  /// \brief The device's model library and impulse-response library (docs/ui-integration-plan.md
  /// slice D, docs/assets.md).
  ///
  /// Every `.nam` under the models root becomes a model a document can name by id; every `.nam` or
  /// `.wav` under the IR root becomes an impulse response. Empty disables that library, and a root
  /// that is missing is an empty library rather than a startup failure -- the registry reports it and
  /// `engine.status` says so.
  std::string models_root = "/etc/namchain/models";
  std::string irs_root = "/etc/namchain/irs";
  /// Unix SOCK_SEQPACKET listening socket.
  std::string socket_path = "/run/namchain/namchaind.sock";
  /// Permission bits for the socket node, as an octal string in the config file.
  unsigned socket_mode = 0660;

  // --- the TCP control transport (slice C; docs/ui-integration-plan.md §1.8) ------------------
  //
  // Off unless an operator turns it on, and useless without a token: the Unix socket is authorised by
  // filesystem permissions, and a TCP connection has none to offer. Everything here is about making
  // a network control port a deliberate act with a bounded blast radius.
  /// Listen for control connections on TCP as well.
  bool tcp_listen = false;
  /// The address to bind. Loopback by default: reaching the daemon from another machine is a decision
  /// an operator should have to write down, not something that happens because a flag was set.
  /// Resolved as a numeric address only -- no DNS at startup, so what binds is what was written.
  std::string tcp_address = "127.0.0.1";
  /// The port to bind, or 0 to let the kernel choose one (which `engine.status` then reports).
  unsigned tcp_port = 47810;
  /// How many TCP connections may be open at once, authenticated or not.
  unsigned tcp_max_connections = 4;
  /// The largest framed message accepted, in bytes. Checked against the length prefix *before* the
  /// body is read, so an oversized message costs a rejection rather than a megabyte of buffer.
  unsigned tcp_max_message_bytes = 1u << 20;
  /// The pre-shared token a TCP client must present in `engine.hello`.
  ///
  /// Passed on the command line this is visible to every user on the machine (`/proc/<pid>/cmdline`),
  /// so on a real appliance use \ref tcp_token_file.
  std::string tcp_token;
  /// Read the token from the first line of this file instead. The file should be mode 0600; the
  /// daemon warns if it is readable more widely, because a token every local user can read is not a
  /// secret. Setting both this and \ref tcp_token is an error rather than a silent precedence.
  std::string tcp_token_file;
  /// How long a connection may stay unauthenticated before it is closed, in milliseconds.
  ///
  /// Without this, four idle TCP connections would hold every slot the transport has. 0 disables the
  /// deadline, which is only reasonable when the listener is not reachable from anywhere untrusted.
  unsigned tcp_auth_timeout_ms = 10000;
  /// SCHED_FIFO priority for the render thread; 0 leaves the policy alone. Requires LimitRTPRIO in
  /// the unit (a systemd service does not get pam_limits).
  int rt_priority = 80;
  /// CPU the render thread is pinned to, or -1 to leave the affinity alone.
  int cpu = -1;
  /// mlockall(MCL_CURRENT) once the plan and the stream buffers exist (never MCL_FUTURE, §14.10).
  bool lock_memory = true;
  /// Output-queue cap and floor, in periods; see BackendConfig. 0 disables each.
  unsigned output_queue_periods = 0;
  unsigned output_queue_floor_periods = 0;
  /// \brief Equal-power crossfade length for a structural plan transition, in milliseconds (§5.3).
  ///
  /// The old and new plans are both rendered for this long and their chain outputs are crossfaded
  /// before the old is retired. 5 ms is §5.3's default; 0 would be a hard switch, which the glitch
  /// detector catches, so it is accepted but not recommended.
  double fade_ms = 5.0;
  /// \brief The model budget, in KiB: what the `nc.nam` nodes of a document may read every block,
  /// together, as estimated from each model's config (working_set.h). 0 turns the budget off.
  ///
  /// All of a document renders on one thread, so this is one core's: on a Cortex-A76 the 512 KiB L2
  /// holds one A2-Full model and a Lite one, not two Full ones (docs/daemon.md, "Model budget"). A
  /// document over it has its slimmable models slimmed, last first, until it fits; one that still
  /// doesn't is refused from a client and admitted with a warning at startup.
  unsigned model_budget_kb = 0;
  /// \brief Where `engine.record` writes its recordings, one directory each. Empty disables it.
  ///
  /// A client names a recording, never a path: the name is one plain directory component under this,
  /// so a control connection can create files here and nowhere else.
  std::string record_dir = "/var/lib/namchain/recordings";
  /// How long the render thread waits before retrying a device that failed or is not present.
  unsigned device_retry_ms = 250;
  /// How long the control loop's poll sleeps when nothing is pending. This is the reclamation
  /// window of rig-pipeline.md §5.3, not a poll interval: the loop wakes on the socket, a signal, or
  /// this timeout, and only the timeout is for `collect()`.
  int reclamation_timeout_ms = 20;
  bool verbose = false;
};

/// \brief Load a JSON config file over the defaults in \p config.
///
/// Keys are the long flag names without the dashes. An unknown key is an error rather than ignored,
/// because a mistyped appliance setting that silently does nothing is exactly the kind of thing a
/// headless daemon cannot afford.
bool load_config_file(const std::string& path, Config& config, std::string& error);

/// \brief Apply one command-line option. \p name is without the leading dashes.
///
/// \p value is the flag's argument, already taken from argv; boolean flags pass an empty string.
/// \return false with \p error set for an unknown flag or an unparsable value.
bool apply_option(Config& config, const std::string& name, const std::string& value, std::string& error);

/// \brief Resolve a rig spec named by the operator (CLI or config) to an absolute path.
///
/// An absolute path is canonicalised as written. A bare name resolves under \ref Config::rigs_root.
/// A path that exists relative to the working directory is also accepted, so `--rig rigs/x.json`
/// works from a checkout. \return false with a reason when nothing resolves to a regular file.
bool resolve_startup_rig(const Config& config, const std::string& spec, std::string& path, std::string& error);

/// \brief Resolve a rig name from the protocol against \ref Config::rigs_root.
///
/// The untrusted counterpart of \ref resolve_startup_rig: absolute paths and anything that escapes
/// the root are refused, and the result must be a regular file. Separate functions because the trust
/// levels are different -- an operator may point the daemon anywhere, a socket client may not.
bool resolve_rig_name(const Config& config, const std::string& name, std::string& path, std::string& error);

/// \brief The shortest TCP token the daemon will accept, in characters.
///
/// A credential a person can remember is a credential a script can guess: the transport allows three
/// attempts per connection, and connections are free. Sixteen characters is the point below which a
/// token is refused outright rather than quietly accepted as decoration.
inline constexpr std::size_t kMinTokenLength = 16;

/// \brief Read \ref Config::tcp_token_file if it is set, and check the token can be a credential.
///
/// Called once at startup, before anything is bound. Refuses a token that is too short, an unreadable
/// or empty token file, both spellings of the token at once, and -- when \ref Config::tcp_listen is
/// set -- a listener with no token at all, because an unauthenticated port that can rewrite the rig
/// and mute the output is worse than a daemon that will not start. Warns (does not refuse) on a token
/// file any other user can read; the message goes to stderr, ungated by \ref Config::verbose.
bool resolve_tcp_token(Config& config, std::string& error);

} // namespace daemon
} // namespace namchain
