#pragma once

// The CoreAudio backend: a WAV looped in, the engine's output to a CoreAudio device, both in one
// process with one clock.
//
// It exists for the machine this project is developed on. `pipe` can already get audio out of a Mac
// (a FIFO and `ffplay`), but it needs a second process and an external player, and the player's
// buffering is both the monitoring latency and the only clock -- which is why its block times are
// explicitly not a measurement of the engine. Here the device's IO thread is the clock, the file is
// read in that same callback, and the engine is a producer/consumer against two lock-free rings.
//
// It is deliberately **capture-from-file only**. The system's default *input* device is the other
// half of the same idea and is not in this slice: reading it would make macOS ask for microphone
// permission, which is a decision for whoever spawns this process, not a side effect of asking for a
// monitoring backend.
//
// The CoreAudio framework is not mentioned in this header: it is behind a pimpl so that nothing that
// includes it has to link the framework, and so the file compiles to nothing on Linux. The body of
// coreaudio.cpp is inside `#if defined(__APPLE__)`, like alsa_raw.cpp's is inside `__linux__`.
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "namchain/io/backend.h"

#if defined(__APPLE__)

namespace namchain
{

/// \brief The `coreaudio` backend's configuration: a file to loop and a device to play it on.
struct CoreAudioConfig
{
  /// The WAV read as capture, looped for the life of the stream. Required: there is no input device
  /// path yet, so an empty path is a refusal rather than a silence.
  std::string input_path;
  /// A CoreAudio device UID, or empty for the system's default output device. A UID rather than an
  /// index: indices are not stable across a replug, and this is the string `Audio MIDI Setup` and
  /// `system_profiler` both show.
  std::string device_uid;
  unsigned sample_rate = 48000;
  /// The engine quantum. The device is asked for exactly this and the backend refuses to open if it
  /// cannot have it: the render plan is compiled for one quantum (§9.2), so a device that rounds it
  /// silently would run frames past every node's buffers.
  unsigned period_size = 64;
  unsigned periods = 4;
  unsigned capture_channels = 1;
  unsigned playback_channels = 1;
  /// Periods of processed audio the output ring may hold before a period is dropped. 0 is "no cap",
  /// in which case the ring's own capacity (the next power of two at or above 8) is the cap.
  unsigned output_queue_periods = 0;
};

/// \brief A looped WAV into a CoreAudio output device, in one process.
class CoreAudioBackend final : public AudioBackend
{
public:
  /// The state behind the pimpl. Public only so the file-local device listener can be handed a
  /// pointer to it as the HAL's client data; its definition is in the .cpp, and nothing above the
  /// backend may name a member of it.
  struct Implementation;

  /// \return nullptr with \p error set when the file cannot be looped, the device does not exist or
  /// is not alive, the device will not take the requested rate or quantum, or the device is not
  /// float32 linear PCM.
  static std::unique_ptr<AudioBackend> create(const BackendConfig& config, std::string& error);

  ~CoreAudioBackend() override;

  const BackendInfo& info() const override;
  bool start(std::string& error) override;
  void stop() override;
  bool wait_capture(int timeout_ms, std::string& error) override;
  int read_capture(void* dst, unsigned frames, int timeout_ms, std::string& error) override;
  int write_playback(const void* src, unsigned frames, int timeout_ms, std::string& error) override;
  int try_write_playback(const void* src, unsigned frames, std::string& error) override;
  StreamQueueStats queue_stats() const override;
  uint64_t driver_xruns() const override;
  bool ring_occupancy(long long& capture_frames, long long& playback_frames) const override;
  std::vector<std::pair<std::string, long long>> diagnostics() const override;
  /// Wakes a `wait_capture` that is blocked on the device's next period, so a daemon can shut down
  /// without waiting one out.
  void interrupt() override;

private:
  CoreAudioBackend();
  std::unique_ptr<Implementation> self_;
};

} // namespace namchain

#endif // __APPLE__
