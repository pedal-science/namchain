#pragma once

// The CoreAudio backend: the engine's output to a CoreAudio device, and its capture from a live
// CoreAudio input device or a WAV on a loop, in one process with one clock.
//
// It exists for the machine this project is developed on. `pipe` can already get audio out of a Mac
// (a FIFO and `ffplay`), but it needs a second process and an external player, and the player's
// buffering is both the monitoring latency and the only clock -- which is why its block times are
// explicitly not a measurement of the engine. Here the device's IO thread is the clock, the capture
// is read in that same callback, and the engine is a producer/consumer against two lock-free rings.
//
// Capture has two sources and they are alternatives, not layers:
//
//  * **A live input device** (`CoreAudioConfig::input_device_uid`), which is what makes a guitar
//    through an interface audible on a Mac with no board attached. This is the one thing in this
//    backend that makes macOS ask for microphone permission -- deliberately, and only when the flag
//    is given: permission is a decision for whoever spawns this process, and a monitoring backend
//    that asked for the microphone because someone wanted to hear a WAV would be asking on the wrong
//    authority. See docs/daemon.md for what the spawner has to arrange.
//  * **A WAV on a loop** (`input_path`), for a machine with no interface attached at all.
//
// When the chosen input and output are *different* HAL devices the backend makes a private aggregate
// device from the two, because two devices have two clocks and this engine has no resampler: the
// output is the aggregate's master (the plan is compiled for its period, and its callback is what
// paces the engine) and the HAL resamples the input to follow it. Private, so it never appears in
// Audio MIDI Setup and goes away with the process.
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

/// \brief The `coreaudio` backend's configuration: where the capture comes from and where the
///        engine's output goes.
struct CoreAudioConfig
{
  /// The WAV read as capture, looped for the life of the stream. Required when \ref input_device_uid
  /// is empty; giving both is refused, because a device to capture from and a file to loop are
  /// alternatives rather than layers.
  std::string input_path;
  /// \brief A live CoreAudio input device UID to capture from, or empty to loop \ref input_path.
  ///
  /// A UID rather than an index, for the same reason \ref device_uid is one. Reading a device is what
  /// makes macOS ask for microphone permission, so this is only ever set because somebody asked for
  /// it by name.
  std::string input_device_uid;
  /// A CoreAudio device UID, or empty for the system's default output device. A UID rather than an
  /// index: indices are not stable across a replug, and this is the string `Audio MIDI Setup` and
  /// `system_profiler` both show.
  std::string device_uid;
  unsigned sample_rate = 48000;
  /// \brief The engine's quantum, as a *ceiling* rather than a request.
  ///
  /// A device's own period is what its callback is actually handed, and on this project's machine
  /// that is not the number `kAudioDevicePropertyBufferFrameSize` reads back as. So the backend
  /// measures the smallest callback the device gives it, reports that as `info.period_size`, and
  /// refuses a device that wants *more* than this: a plan compiled for the daemon's `period_size`
  /// carries any smaller period, and a larger one has nowhere to go.
  unsigned period_size = 64;
  unsigned periods = 4;
  unsigned capture_channels = 1;
  unsigned playback_channels = 1;
  /// Periods of processed audio the output ring may hold before a period is dropped. 0 is "no cap",
  /// in which case the ring's own capacity (the next power of two at or above 8) is the cap.
  unsigned output_queue_periods = 0;
  /// \brief The device's own buffer, in frames, to ask for; 0 leaves it as it is found.
  ///
  /// This is the device's IO buffer, not the engine's quantum, and it is most of the monitoring
  /// latency: 512 frames is 10.67 ms at 48 kHz, and a USB interface will usually take a great deal
  /// less. Applied with `kAudioDevicePropertyBufferFrameSize` after the stream device is chosen and
  /// *before* the period is measured, because the measurement is of what the callback is handed and
  /// that is exactly what this changes. What the device was set to before is put back in `stop()`.
  unsigned device_buffer_frames = 0;
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
