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
  /// Device spec for the device backends. On Linux an ALSA spec: "hw:CARD=<id>", "hw:<id>,<d>",
  /// "hw:4,0", "4,0", or a /dev/snd path. On macOS a CoreAudio device UID, empty asking for the
  /// system's default output device. Ignored by `null`, `file` and `pipe`.
  std::string device;
  unsigned sample_rate = 48000;
  unsigned period_size = 64;
  unsigned periods = 4;
  unsigned capture_channels = 1;
  unsigned playback_channels = 1;
  /// The WAV read as input by `file`, `pipe` and `coreaudio`. Empty for the device backends, which
  /// use \ref device. `file` stops at the end of it; `pipe` and `coreaudio` loop it, because their
  /// whole purpose is to feed a rig with no instrument plugged in.
  ///
  /// This is what makes `namchaind --backend file` an offline render of a whole document -- every
  /// chain, its channel binding and its summing included -- rather than something only the soak tool
  /// could do, and it is how the multi-chain channel tests assert where each chain's audio landed.
  std::string capture_path;
  /// \brief The device to capture from, where the backend takes one: a CoreAudio input device UID.
  ///
  /// The alternative to \ref capture_path rather than a companion to it, and the two together are
  /// refused by name rather than one silently winning. Empty means "take the capture from
  /// capture_path", which is what `file` and `pipe` always do. Only `coreaudio` reads it.
  ///
  /// This is what makes a rig audible through a real interface on a Mac: the input is the interface's
  /// own, not a file. It is also the one thing here that makes macOS ask for microphone permission,
  /// which is a decision for whatever spawns this process and therefore belongs to a flag rather than
  /// to a default.
  std::string input_device;
  /// \brief The device's own buffer, in frames, to ask for where the backend can set it; 0 leaves it.
  ///
  /// Not the engine's quantum: `period_size` is the plan's ceiling and the backend measures what the
  /// device actually hands over. This is the *device's* buffer, which is most of the monitoring
  /// latency, and a backend that sets it must put back what it found when it stops. Only `coreaudio`
  /// reads it today.
  unsigned device_buffer_frames = 0;
  std::string playback_path;
  /// \brief Where `coreaudio` writes what it is sending the device, or empty for nowhere.
  ///
  /// A measurement instrument, not a feature: it exists so the live device path can be null-tested
  /// the way `--backend file` already can. Only compiled in with `NAMCHAIN_COREAUDIO_CAPTURE`, and
  /// refused by name when this build does not have it — the same rule every other unavailable thing
  /// in here follows.
  std::string capture_device_path;
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
  ///
  /// With \ref render_threads above 1 this is the *first* core of the render set: the coordinator
  /// takes this one and the workers take the ones above it.
  int cpu = -1;
  /// \brief How many cores the render path uses: one coordinator plus this many minus one workers.
  ///
  /// 1 is the single-threaded engine that has always run here, and it stays bit-identical to it
  /// (docs/multi-core.md). Above 1, the document's chains are shared out across threads pinned to
  /// `cpu` and the cores above it: the coordinator keeps the device, the input decode, the
  /// crossfades, the output bus and every piece of transition bookkeeping, and each worker runs its
  /// own chains' plans. A chain is never split, so every chain still executes on one thread in its
  /// original node order, which is what keeps the output identical to the single-threaded run.
  ///
  /// It only pays where there is more than one chain's worth of work to run at once; a rig whose
  /// weight is one long chain gets nothing from it (docs/multi-core.md, "What it does not help").
  /// A value larger than the cores this process may run on is refused at startup, because a worker
  /// that cannot be pinned is a worker whose timing cannot be reported.
  unsigned render_threads = 1;
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
  /// \brief Whether the engine measures audio levels at all (`level.h`, docs/daemon.md "Levels").
  ///
  /// On by default, because a meter and a diagnostic both need it and a feature that has to be asked
  /// for is one nobody finds. Off is for the two cases where the per-sample cost is not wanted: an
  /// appliance with no client polling levels at all, and a measurement that has to separate the
  /// engine's own cost from the cost of watching it.
  ///
  /// Off means the probes are never built, the render path skips every accumulation, `engine.levels`
  /// is refused with `levels_disabled`, and `features` does not advertise "levels" -- so a client
  /// degrades to having no meters rather than polling a method that cannot answer. It is read once,
  /// when the daemon is constructed: a plan's probes are made as the plan is compiled, and a local
  /// transition compiles a plan on the render path, so a value that could change under it would be a
  /// probe that some plans write and others do not.
  bool levels = true;
  /// \brief Measure **one frame per period** rather than every frame (experiment switch; off).
  ///
  /// The per-sample cost of a level probe is what an appliance pays for having meters at all, and it
  /// is paid on the render core. This is the crude way to find out what that cost is worth: the
  /// render path folds the period's first frame into the probes and skips the rest, so the atomic
  /// work falls by whatever the period length is -- 64x at the pedal's 64-frame period.
  ///
  /// It is **not** a free win and it is off by default, because it changes what a meter reads: one
  /// sample per period misses nearly every transient the peak exists to catch, and the reported `rms`
  /// is the magnitude of that one sample rather than a mean over the window. `frames` reports 1 per
  /// period, which is the honest answer to "how many samples is this a level of" and what lets a
  /// client tell a sparse window from a full one.
  ///
  /// Measured on vip (bench-results/2026-10-06-levels-switch.md) rather than assumed.
  bool levels_first_frame = false;
  /// \brief The most often a client's read may cost a real measurement, in Hz; 0 is unlimited.
  ///
  /// Every read *takes* the probes (level.h), so a client polling at 20 Hz builds the levels JSON --
  /// a square root and two logarithms per entry, plus the JSON -- twenty times a second. That work is
  /// on the control thread, not the render thread, so this is not about render headroom: it is about
  /// what a poll costs the daemon at all.
  ///
  /// Above 0, the probes are drained at most this often and a read in between is answered from the
  /// held value. The control loop also drains on this cadence, so the cache stays warm whether or not
  /// anybody is reading. A rate at or above the client's own that changes nothing is the point: a
  /// 20 Hz meter against a 20 Hz rate costs exactly what it costs today.
  ///
  /// The trade is freshness: at 1 Hz a meter shows a value up to a second old, which is legible for a
  /// level and not for an edit. Off by default for that reason. `engine.levels` reports the cache's
  /// hit and miss counts, so a client can tell which it got.
  double levels_rate_hz = 0.0;
  /// \brief When the render thread writes a probe's atomic fields: per sample, or once per period.
  ///
  /// `Period` folds samples into plain private memory and adds the window to the atomics once a
  /// period, which is the same thing the meter reads -- a publish adds rather than sets, so the
  /// read-and-reset contract is unchanged -- at a fraction of the cache-coherency traffic between the
  /// render thread and the control thread that reads it. `Sample` is the behaviour every build before
  /// this one had, and stays the default until the measurement says otherwise:
  /// bench-results/2026-10-06-levels-switch.md, "Part 4".
  ///
  /// One period of latency is the price, and only when the reader arrives after a period was rendered
  /// and before the next one is: 1.33 ms at 64 frames and 48 kHz.
  bool levels_publish_period = false;
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
