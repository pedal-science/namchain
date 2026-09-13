#pragma once

// The I/O layer's one interface (plan.md §9.1).
//
// Four very different things have to look the same to the engine: a WAV file, a paced clock with no
// device, raw ALSA ioctls, and PipeWire. Before this existed, `alsa_raw` was a concrete class and
// `offline` was an unrelated sibling, so the soak harness had exactly two modes -- a real device, or
// flat out -- and neither CI nor a laptop could exercise the timing machinery. The point of the
// interface is that everything above it (the render plan, the block timers, the oracle, the xrun
// accounting) is written once and runs against all of them.
#include <cstdint>
#include <memory>
#include <string>

#include "namchain/pcm.h"

namespace namchain
{

enum class BackendKind
{
  File,    ///< WAV in, WAV out, as fast as the disk allows. The offline instance of the interface.
  Null,    ///< Real-time paced against the wall clock, with no device: silence in, discarded out.
  AlsaRaw, ///< /dev/snd PCM ioctls, no libasound (Linux only, §4.3).
  PipeWire ///< Not implemented; refused by name rather than silently falling back.
};

const char* backend_kind_name(BackendKind kind);

/// Parse "file", "null", "alsa", "alsa_raw", "pipewire". \return false on an unknown name, so a
/// typo is an error rather than a silent fallback to a different backend.
bool parse_backend_kind(const std::string& text, BackendKind& kind);

struct BackendConfig
{
  unsigned sample_rate = 48000;
  /// Requested period size. Every backend reports what it actually got in \ref BackendInfo.
  unsigned period_size = 64;
  /// Requested buffer length in periods.
  unsigned periods = 4;
  unsigned capture_channels = 1;
  unsigned playback_channels = 1;
  /// Device spec for the device backends: "hw:4,0", "4,0", or a /dev/snd path.
  std::string device;
  /// `file` only.
  std::string input_path;
  std::string output_path;
  /// `null` only: pace against the wall clock the way a device would. False runs flat out, which is
  /// what a throughput measurement wants and a lateness measurement cannot use.
  bool realtime_paced = true;
  /// Refuse a period size other than the one requested. On a device this is the guard against the
  /// silent rounding measured in §9.5.
  bool require_exact_period = true;
};

struct BackendInfo
{
  unsigned period_size = 0;
  unsigned buffer_size = 0;
  unsigned sample_rate = 0;
  unsigned capture_channels = 0;
  unsigned playback_channels = 0;
  /// The format on the wire. The backends with no wire use F32_LE.
  PcmFormat format = PcmFormat::Unknown;
  unsigned bytes_per_sample = 0;
  /// Bytes per frame, per direction. Two fields rather than one because the counts differ on an
  /// asymmetric device -- the SSL 2+ Mk II is 6 out / 8 in -- and sizing both directions from one
  /// number is how a capture buffer ends up 25 % short.
  unsigned capture_bytes_per_frame = 0;
  unsigned playback_bytes_per_frame = 0;
  /// False when the backend could not give the requested period exactly.
  bool period_exact = false;
  std::string capture_name;
  std::string playback_name;
  /// The minimum a round trip through this backend can take, in frames: what its own buffers add
  /// before any converter is involved. 0 for a backend with no rings.
  uint64_t buffer_floor_frames = 0;
  /// Set once capture has no more frames to give. A caller that loops until this is set will stop
  /// at end of input rather than reading silence forever.
  bool capture_exhausted = false;
  /// True when the first period after start is an artefact of starting and the caller should run one
  /// untimed cycle before beginning to measure -- a device's ring is empty at START, and its first
  /// period costs a fill rather than a period's work.
  ///
  /// False for the backends with no rings, and it has to be: for `file` that "bring-up" cycle is a
  /// period of silence unconditionally written to the output WAV, which showed up as an output one
  /// period longer than the renderer's and would have been blamed on the DSP.
  bool needs_bringup_cycle = false;
};

/// \brief One full-duplex audio stream, whatever is behind it.
///
/// Deliberately byte-oriented and direction-symmetric, matching what the ioctls actually do: the
/// caller converts with namchain/pcm.h. Making the interface float-typed would push a conversion
/// into every backend and hide the one place it belongs.
///
/// Not thread-safe. `read_capture`/`write_playback`/`wait_capture` are called from the audio thread
/// and must not allocate, lock or (for the device backends) do anything the caller has not put
/// outside its real-time scope.
class AudioBackend
{
public:
  virtual ~AudioBackend() = default;

  AudioBackend(const AudioBackend&) = delete;
  AudioBackend& operator=(const AudioBackend&) = delete;
  virtual const BackendInfo& info() const = 0;

  virtual bool start(std::string& error) = 0;
  virtual void stop() = 0;

  /// Wait until a period of capture is available. The engine's loop is wait (untimed) -> read, DSP,
  /// write (timed), so this is where a real-time backend blocks.
  virtual bool wait_capture(int timeout_ms, std::string& error) = 0;

  /// Read exactly \p frames, or fewer only at end of input. \return frames transferred, or -1 on
  /// error. \p dst must hold `frames * bytes_per_sample * capture_channels` BYTES.
  virtual int read_capture(void* dst, unsigned frames, int timeout_ms, std::string& error) = 0;

  /// Write exactly \p frames from \p src. Same sizing rule, playback direction.
  virtual int write_playback(const void* src, unsigned frames, int timeout_ms, std::string& error) = 0;

  /// Unrecoverable breaks reported by whatever is underneath: ALSA `-EPIPE`, and nothing else.
  /// Always zero for the backends with no device, which is a fact about the measurement worth
  /// reporting rather than a reason to omit the field (§10.5.1).
  virtual uint64_t driver_xruns() const = 0;

  /// Flush anything buffered. A no-op for every backend that is not `file`, where it writes the
  /// output WAV.
  virtual bool finish(std::string& error)
  {
    (void)error;
    return true;
  }

protected:
  /// Protected and defaulted: declaring the copy operations deleted above would otherwise suppress
  /// it, and every concrete backend needs it.
  AudioBackend() = default;
};

/// \brief Open a backend, or return nullptr with \p error set.
///
/// A backend that does not exist (PipeWire today) fails by name. Falling back to another one would
/// make "I measured it on PipeWire" a claim nobody could check.
std::unique_ptr<AudioBackend> open_backend(const BackendConfig& config, BackendKind kind, std::string& error);

} // namespace namchain
