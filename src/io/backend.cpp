// The backends behind namchain/io/backend.h. See that header for why the interface exists.
#include "namchain/io/backend.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "namchain/wav.h"

#if defined(__linux__)
  #include "namchain/io/alsa_params.h"
  #include "namchain/io/alsa_raw.h"
#endif

namespace namchain
{
namespace
{

using Clock = std::chrono::steady_clock;

/// The paced clock with no device.
///
/// This is the backend that lets the timing machinery be exercised anywhere: it hands back silence
/// and throws away what is written, but it keeps real time exactly as a device would, so a soak run
/// against it measures the same block-time percentiles, the same deadline overruns and the same
/// oracle agreement. What it cannot report is xruns, because there is no driver to report them --
/// which is why driver_xruns() is zero here and the harness's own deadline accounting carries the
/// weight in CI.
class NullBackend final : public AudioBackend
{
public:
  explicit NullBackend(const BackendConfig& config) : paced_(config.realtime_paced)
  {
    const unsigned periods = config.periods == 0 ? 4 : config.periods;
    info_.period_size = config.period_size;
    info_.buffer_size = config.period_size * periods;
    info_.sample_rate = config.sample_rate;
    info_.capture_channels = config.capture_channels;
    info_.playback_channels = config.playback_channels;
    info_.format = PcmFormat::F32_LE;
    info_.bytes_per_sample = 4;
    info_.capture_bytes_per_frame = info_.capture_channels * info_.bytes_per_sample;
    info_.playback_bytes_per_frame = info_.playback_channels * info_.bytes_per_sample;
    info_.period_exact = true;
    info_.capture_name = "null";
    info_.playback_name = "null";
    info_.buffer_floor_frames = info_.buffer_size;
  }

  const BackendInfo& info() const override { return info_; }

  bool start(std::string& error) override
  {
    if (info_.sample_rate == 0 || info_.period_size == 0)
    {
      error = "null backend needs a non-zero period and sample rate";
      return false;
    }
    period_ns_ = std::chrono::nanoseconds(static_cast<long long>(
      (1000000000.0 * static_cast<double>(info_.period_size)) / static_cast<double>(info_.sample_rate)));
    origin_ = Clock::now();
    period_ = 0;
    info_.capture_exhausted = false;
    running_ = true;
    return true;
  }

  void stop() override { running_ = false; }

  bool wait_capture(int /*timeout_ms*/, std::string& error) override
  {
    if (!running_)
    {
      error = "null backend: wait_capture before start";
      return false;
    }
    if (paced_)
    {
      // An absolute deadline rather than a relative sleep. Sleeping for one period each time would
      // accumulate however late each sleep woke up, so a run would measure the sleeper's drift; this
      // way a late period is late on its own and the next one is still on time.
      std::this_thread::sleep_until(origin_ + period_ * period_ns_);
    }
    ++period_;
    return true;
  }

  int read_capture(void* dst, unsigned frames, int /*timeout_ms*/, std::string& /*error*/) override
  {
    // Silence, and silence is the honest thing for a device with no input: the engine cannot tell
    // this apart from an interface with nothing plugged into it.
    std::memset(dst, 0, static_cast<size_t>(frames) * info_.capture_bytes_per_frame);
    return static_cast<int>(frames);
  }

  int write_playback(const void* /*src*/, unsigned frames, int /*timeout_ms*/, std::string& /*error*/) override
  {
    return static_cast<int>(frames);
  }

  uint64_t driver_xruns() const override { return 0; }

private:
  BackendInfo info_;
  bool paced_ = true;
  bool running_ = false;
  uint64_t period_ = 0;
  std::chrono::nanoseconds period_ns_{0};
  Clock::time_point origin_{};
};

/// WAV in, WAV out.
///
/// Unpaced on purpose: the offline render is the reference the real-time run is compared against,
/// and a reference that is itself paced is not one. This backend is also the first consumer of the
/// multichannel WAV reader -- the core's loader refuses anything but mono, and a device capture
/// that is going to be replayed through the engine is usually not mono.
///
/// write_playback appends to a vector, so it allocates. That is fine here and nowhere else: the
/// real-time discipline in plan.md §3.3 is about the audio thread of a live stream, and this
/// backend has no live stream. It is said out loud because "the file backend allocates" would
/// otherwise read as a violation.
class FileBackend final : public AudioBackend
{
public:
  static std::unique_ptr<AudioBackend> create(const BackendConfig& config, std::string& error)
  {
    std::unique_ptr<FileBackend> self(new FileBackend());

    std::vector<float> interleaved;
    unsigned channels = 0;
    double rate = 0.0;
    if (!load_wav(config.input_path, interleaved, channels, rate, error))
      return nullptr;
    if (static_cast<unsigned>(rate + 0.5) != config.sample_rate)
    {
      // The engine is 48 kHz and has no resampler (§9.2, G11): a file at another rate is an error
      // rather than a silent resample, in keeping with every other rate check in the project.
      error = "input '" + config.input_path + "' is " + std::to_string(static_cast<long long>(rate)) +
              " Hz but the engine runs at " + std::to_string(config.sample_rate) + " Hz";
      return nullptr;
    }

    self->input_ = std::move(interleaved);
    self->input_frames_ = channels == 0 ? 0 : self->input_.size() / channels;

    self->info_.period_size = config.period_size;
    self->info_.buffer_size = config.period_size * (config.periods == 0 ? 4 : config.periods);
    self->info_.sample_rate = config.sample_rate;
    self->info_.capture_channels = channels;
    self->info_.playback_channels = std::max(1u, config.playback_channels);
    self->info_.format = PcmFormat::F32_LE;
    self->info_.bytes_per_sample = 4;
    self->info_.capture_bytes_per_frame = self->info_.capture_channels * self->info_.bytes_per_sample;
    self->info_.playback_bytes_per_frame = self->info_.playback_channels * self->info_.bytes_per_sample;
    self->info_.period_exact = true;
    self->info_.capture_name = config.input_path;
    self->info_.playback_name = config.output_path;
    // No rings and no converters: nothing in this backend adds latency of its own.
    self->info_.buffer_floor_frames = 0;
    self->output_path_ = config.output_path;
    return self;
  }

  const BackendInfo& info() const override { return info_; }

  bool start(std::string& /*error*/) override
  {
    read_frames_ = 0;
    info_.capture_exhausted = false;
    return true;
  }

  void stop() override {}

  bool wait_capture(int /*timeout_ms*/, std::string& /*error*/) override { return true; }

  int read_capture(void* dst, unsigned frames, int /*timeout_ms*/, std::string& /*error*/) override
  {
    const size_t remaining = input_frames_ - read_frames_;
    const unsigned count = static_cast<unsigned>(std::min<size_t>(frames, remaining));
    const size_t stride = info_.capture_bytes_per_frame;

    for (unsigned i = 0; i < count; ++i)
      for (unsigned c = 0; c < info_.capture_channels; ++c)
        pcm_encode(static_cast<unsigned char*>(dst) + static_cast<size_t>(i) * stride +
                     static_cast<size_t>(c) * info_.bytes_per_sample,
                   PcmFormat::F32_LE, input_[(read_frames_ + i) * info_.capture_channels + c]);

    // The tail of a short final period is silence rather than stale bytes, so a caller that ignores
    // the short return still gets something defined.
    if (count < frames)
      std::memset(static_cast<unsigned char*>(dst) + static_cast<size_t>(count) * stride, 0,
                  static_cast<size_t>(frames - count) * stride);

    read_frames_ += count;
    if (count < frames)
      info_.capture_exhausted = true;
    return static_cast<int>(count);
  }

  int write_playback(const void* src, unsigned frames, int /*timeout_ms*/, std::string& /*error*/) override
  {
    const size_t stride = info_.playback_bytes_per_frame;
    output_.resize(output_.size() + static_cast<size_t>(frames) * info_.playback_channels);
    float* destination = output_.data() + output_.size() - static_cast<size_t>(frames) * info_.playback_channels;
    for (unsigned i = 0; i < frames; ++i)
      for (unsigned c = 0; c < info_.playback_channels; ++c)
        destination[static_cast<size_t>(i) * info_.playback_channels + c] =
          pcm_decode(static_cast<const unsigned char*>(src) + static_cast<size_t>(i) * stride +
                       static_cast<size_t>(c) * info_.bytes_per_sample,
                     PcmFormat::F32_LE);
    return static_cast<int>(frames);
  }

  uint64_t driver_xruns() const override { return 0; }

  bool finish(std::string& error) override
  {
    if (output_path_.empty())
      return true; // nothing was asked for, nothing to write
    const size_t frames = output_.size() / info_.playback_channels;
    return save_wav_float32(output_path_, output_.data(), frames, static_cast<int>(info_.playback_channels),
                            static_cast<double>(info_.sample_rate), error);
  }

private:
  FileBackend() = default;

  BackendInfo info_;
  std::vector<float> input_;   ///< interleaved, capture_channels wide
  std::vector<float> output_;  ///< interleaved, playback_channels wide
  size_t input_frames_ = 0;
  size_t read_frames_ = 0;
  std::string output_path_;
};

#if defined(__linux__)

/// The adapter that makes the raw ALSA stream one of the backends.
///
/// It converts only at the boundary and holds nothing: every call goes straight to the ioctl path,
/// so the device backend's behaviour -- including its xrun counting and its re-priming recovery --
/// is the code that was already measured on hardware, not a reimplementation of it.
class AlsaBackend final : public AudioBackend
{
public:
  static std::unique_ptr<AudioBackend> create(const BackendConfig& config, std::string& error)
  {
    AlsaRawConfig raw;
    raw.capture = config.device;
    raw.playback = config.device;
    raw.sample_rate = config.sample_rate;
    raw.channels = config.capture_channels;
    raw.capture_channels = config.capture_channels;
    raw.playback_channels = config.playback_channels;
    raw.quantum = config.period_size;
    raw.periods = config.periods;
    raw.require_exact_period = config.require_exact_period;

    std::unique_ptr<AlsaRawDuplex> device = AlsaRawDuplex::open(raw, error);
    if (!device)
      return nullptr;

    std::unique_ptr<AlsaBackend> self(new AlsaBackend());
    const AlsaRawInfo& source = device->info();
    self->info_.period_size = source.period_size;
    self->info_.buffer_size = source.buffer_size;
    self->info_.sample_rate = source.sample_rate;
    self->info_.capture_channels = source.capture_channels;
    self->info_.playback_channels = source.playback_channels;
    self->info_.format = alsa::pcm_format(source.sample_format);
    self->info_.bytes_per_sample = source.bytes_per_sample;
    self->info_.capture_bytes_per_frame = source.capture_bytes_per_frame;
    self->info_.playback_bytes_per_frame = source.playback_bytes_per_frame;
    self->info_.period_exact = source.period_exact;
    self->info_.capture_name = source.capture_node;
    self->info_.playback_name = source.playback_node;
    // One buffer for playback plus one for capture is the floor the rings impose on any round trip.
    self->info_.buffer_floor_frames = 2ull * source.buffer_size;
    self->info_.needs_bringup_cycle = true;
    if (self->info_.format == PcmFormat::Unknown)
    {
      error = "the device's sample format (" + std::to_string(source.sample_format) +
              ") is not one this project converts";
      return nullptr;
    }
    self->device_ = std::move(device);
    return self;
  }

  const BackendInfo& info() const override { return info_; }

  bool start(std::string& error) override { return device_->start(error); }
  void stop() override { device_->stop(); }

  bool wait_capture(int timeout_ms, std::string& error) override
  {
    return device_->wait_capture(timeout_ms, error);
  }

  int read_capture(void* dst, unsigned frames, int timeout_ms, std::string& error) override
  {
    return device_->read_capture(dst, frames, timeout_ms, error);
  }

  int write_playback(const void* src, unsigned frames, int timeout_ms, std::string& error) override
  {
    return device_->write_playback(src, frames, timeout_ms, error);
  }

  uint64_t driver_xruns() const override { return device_->driver_xruns(); }

  bool ring_occupancy(long long& capture_frames, long long& playback_frames) const override
  {
    AlsaRawOccupancy capture;
    AlsaRawOccupancy playback;
    std::string ignored;
    if (!device_->query_occupancy(true, capture, ignored))
      return false;
    if (!device_->query_occupancy(false, playback, ignored))
      return false;
    capture_frames = capture.delay_frames;
    playback_frames = playback.delay_frames;
    return true;
  }

private:
  AlsaBackend() = default;

  BackendInfo info_;
  std::unique_ptr<AlsaRawDuplex> device_;
};

#endif // __linux__

} // namespace

const char* backend_kind_name(BackendKind kind)
{
  switch (kind)
  {
  case BackendKind::File: return "file";
  case BackendKind::Null: return "null";
  case BackendKind::AlsaRaw: return "alsa_raw";
  case BackendKind::PipeWire: return "pipewire";
  }
  return "unknown";
}

bool parse_backend_kind(const std::string& text, BackendKind& kind)
{
  if (text == "file")
  {
    kind = BackendKind::File;
    return true;
  }
  if (text == "null")
  {
    kind = BackendKind::Null;
    return true;
  }
  if (text == "alsa" || text == "alsa_raw")
  {
    kind = BackendKind::AlsaRaw;
    return true;
  }
  if (text == "pipewire")
  {
    kind = BackendKind::PipeWire;
    return true;
  }
  return false;
}

std::unique_ptr<AudioBackend> open_backend(const BackendConfig& config, BackendKind kind, std::string& error)
{
  if (config.period_size < 1 || config.period_size > 4096)
  {
    error = "period size " + std::to_string(config.period_size) + " is out of range [1, 4096]";
    return nullptr;
  }
  if (config.sample_rate < 8000 || config.sample_rate > 384000)
  {
    error = "sample rate " + std::to_string(config.sample_rate) + " is out of range";
    return nullptr;
  }
  if (config.capture_channels < 1 || config.playback_channels < 1)
  {
    error = "channel counts must be at least 1";
    return nullptr;
  }

  switch (kind)
  {
  case BackendKind::Null: return std::make_unique<NullBackend>(config);
  case BackendKind::File: return FileBackend::create(config, error);
  case BackendKind::AlsaRaw:
#if defined(__linux__)
    if (config.device.empty())
    {
      error = "the alsa_raw backend needs a device, e.g. hw:4,0";
      return nullptr;
    }
    return AlsaBackend::create(config, error);
#else
    error = "the alsa_raw backend is Linux-only; this build has no /dev/snd";
    return nullptr;
#endif
  case BackendKind::PipeWire:
    // Named rather than silently redirected to another backend. A measurement that says "PipeWire"
    // has to have been taken on PipeWire.
    error = "the pipewire backend is not implemented yet; use --backend alsa or --backend null";
    return nullptr;
  }
  error = "unknown backend";
  return nullptr;
}

} // namespace namchain
