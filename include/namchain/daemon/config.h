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
  /// Device spec for the device backends: "hw:4,0", "4,0", or a /dev/snd path. Empty = platform
  /// default. Ignored by `null` and `file`.
  std::string device;
  unsigned sample_rate = 48000;
  unsigned period_size = 64;
  unsigned periods = 4;
  unsigned capture_channels = 1;
  unsigned playback_channels = 1;
  /// The startup rig, read synchronously before the clock starts (rig-pipeline.md §5.1 item 2). An
  /// absolute path is taken as written; a bare name resolves under \ref rigs_root. Empty means the
  /// daemon starts silent and waits for a `rig.load`.
  std::string rig;
  /// The directory a `rig.load` name is resolved against. A protocol client can name a rig but can
  /// never name a path outside this directory (rig-pipeline.md §5.1 item 1).
  std::string rigs_root = "/etc/namchain/rigs";
  /// Unix SOCK_SEQPACKET listening socket.
  std::string socket_path = "/run/namchain/namchaind.sock";
  /// Permission bits for the socket node, as an octal string in the config file.
  unsigned socket_mode = 0660;
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

} // namespace daemon
} // namespace namchain
