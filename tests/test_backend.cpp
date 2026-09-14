// Tests for the I/O backend interface and the two backends that need no hardware.
//
// These are the backends that let the timing machinery be exercised in CI, so they have to be
// right about the things a device cannot be asked about on a machine with no device: exactly which
// samples arrive, exactly when, and what happens at end of input.
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "namchain/io/backend.h"
#include "namchain/pcm.h"
#include "namchain/wav.h"

namespace
{

std::string temp_wav(const char* name)
{
  return std::string("/tmp/namchain-backend-") + name + ".wav";
}

std::vector<float> decode_f32(const std::vector<unsigned char>& bytes, unsigned channels)
{
  std::vector<float> out(bytes.size() / (4u * channels));
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = namchain::pcm_decode(bytes.data() + i * 4u * channels, namchain::PcmFormat::F32_LE);
  return out;
}

} // namespace

TEST_CASE("the file backend delivers the file's samples, a period at a time")
{
  // An impulse and a step, so a reader that drops or shifts a block cannot pass by producing
  // silence of the right length.
  const std::string path = temp_wav("impulse");
  std::vector<float> source(600, 0.0f);
  source[100] = 0.75f;
  for (std::size_t i = 300; i < source.size(); ++i)
    source[i] = -0.25f;

  std::string error;
  REQUIRE(namchain::save_wav_float32(path, source.data(), source.size(), 1, 48000.0, error));

  // First, that the file on disk is what we think it is. Otherwise a failure below is being blamed
  // on the backend when the loader is the one that is wrong.
  {
    std::vector<float> direct;
    unsigned direct_channels = 0;
    double direct_rate = 0.0;
    REQUIRE(namchain::load_wav(path, direct, direct_channels, direct_rate, error));
    REQUIRE(direct.size() == source.size());
    CHECK(direct[100] == 0.75f);
  }

  namchain::BackendConfig config;
  config.input_path = path;
  config.output_path = temp_wav("unused");
  config.sample_rate = 48000;
  config.period_size = 64;
  config.capture_channels = 1;
  config.playback_channels = 1;

  std::unique_ptr<namchain::AudioBackend> backend =
    namchain::open_backend(config, namchain::BackendKind::File, error);
  REQUIRE(backend != nullptr);
  CHECK(backend->info().needs_bringup_cycle == false);
  CHECK(backend->info().format == namchain::PcmFormat::F32_LE);
  REQUIRE(backend->start(error));

  std::vector<unsigned char> buffer(static_cast<std::size_t>(config.period_size) * 4u);
  std::vector<float> received;
  for (int block = 0; block < 10; ++block)
  {
    REQUIRE(backend->wait_capture(1000, error));
    const int got = backend->read_capture(buffer.data(), config.period_size, 1000, error);
    REQUIRE(got >= 0);
    const std::vector<float> decoded = decode_f32(buffer, 1);
    received.insert(received.end(), decoded.begin(), decoded.begin() + got);
    if (got < static_cast<int>(config.period_size))
      break;
  }

  REQUIRE(received.size() == source.size());

  // Report every frame that differs rather than the first, because a backend that repeats one sample
  // across a period still looks like a waveform: the shape of the mismatch is the diagnosis.
  std::size_t mismatches = 0;
  std::string detail;
  for (std::size_t i = 0; i < source.size(); ++i)
    if (received[i] != source[i])
    {
      if (mismatches < 8)
        detail += " " + std::to_string(i) + ": got " + std::to_string(received[i]) + ", want " +
                  std::to_string(source[i]);
      ++mismatches;
    }
  INFO(mismatches << " of " << source.size() << " frames differ;" << detail);
  CHECK(mismatches == 0);

  backend->stop();
  std::remove(path.c_str());
  std::remove(config.output_path.c_str());
}

TEST_CASE("the file backend stops at end of input instead of repeating it")
{
  const std::string path = temp_wav("short");
  std::vector<float> source(100, 0.5f);

  std::string error;
  REQUIRE(namchain::save_wav_float32(path, source.data(), source.size(), 1, 48000.0, error));

  namchain::BackendConfig config;
  config.input_path = path;
  config.sample_rate = 48000;
  config.period_size = 64;
  config.capture_channels = 1;
  config.playback_channels = 1;

  std::unique_ptr<namchain::AudioBackend> backend =
    namchain::open_backend(config, namchain::BackendKind::File, error);
  REQUIRE(backend != nullptr);
  REQUIRE(backend->start(error));

  std::vector<unsigned char> buffer(64u * 4u);
  REQUIRE(backend->wait_capture(1000, error));
  CHECK(backend->read_capture(buffer.data(), 64, 1000, error) == 64); // a full first period
  CHECK_FALSE(backend->info().capture_exhausted);

  // The second period is short by 36 frames, and that is what "end of input" looks like: a short
  // read, then a flag. Reading again must not wrap around to the start of the file, which would
  // turn a 100-frame file into an infinite one and make every soak "pass".
  CHECK(backend->read_capture(buffer.data(), 64, 1000, error) == 36);
  CHECK(backend->info().capture_exhausted);
  CHECK(backend->read_capture(buffer.data(), 64, 1000, error) == 0);

  backend->stop();
  std::remove(path.c_str());
}

TEST_CASE("the file backend writes back what it was given")
{
  const std::string input = temp_wav("in");
  const std::string output = temp_wav("out");
  std::vector<float> source(200);
  for (std::size_t i = 0; i < source.size(); ++i)
    source[i] = static_cast<float>(std::sin(0.1 * static_cast<double>(i)));

  std::string error;
  REQUIRE(namchain::save_wav_float32(input, source.data(), source.size(), 1, 48000.0, error));

  namchain::BackendConfig config;
  config.input_path = input;
  config.output_path = output;
  config.sample_rate = 48000;
  config.period_size = 64;

  std::unique_ptr<namchain::AudioBackend> backend =
    namchain::open_backend(config, namchain::BackendKind::File, error);
  REQUIRE(backend != nullptr);
  REQUIRE(backend->start(error));

  std::vector<unsigned char> buffer(64u * 4u);
  for (;;)
  {
    const int got = backend->read_capture(buffer.data(), 64, 1000, error);
    REQUIRE(got >= 0);
    if (got == 0)
      break;
    REQUIRE(backend->write_playback(buffer.data(), static_cast<unsigned>(got), 1000, error) == got);
    if (backend->info().capture_exhausted)
      break;
  }
  backend->stop();
  REQUIRE(backend->finish(error));

  std::vector<float> written;
  unsigned channels = 0;
  double rate = 0.0;
  REQUIRE(namchain::load_wav(output, written, channels, rate, error));
  CHECK(channels == 1u);
  REQUIRE(written.size() == source.size());
  for (std::size_t i = 0; i < source.size(); ++i)
    CHECK(written[i] == source[i]);

  std::remove(input.c_str());
  std::remove(output.c_str());
}

TEST_CASE("the null backend keeps real time without a device")
{
  namchain::BackendConfig config;
  config.sample_rate = 48000;
  config.period_size = 64;
  config.periods = 4;
  config.realtime_paced = true;

  std::string error;
  std::unique_ptr<namchain::AudioBackend> backend =
    namchain::open_backend(config, namchain::BackendKind::Null, error);
  REQUIRE(backend != nullptr);
  CHECK(backend->info().format == namchain::PcmFormat::F32_LE);
  CHECK(backend->info().buffer_size == 256u);
  // No device, so nothing may claim to have underrun: a zero here is a fact, not a gap in reporting.
  CHECK(backend->driver_xruns() == 0);

  REQUIRE(backend->start(error));
  std::vector<unsigned char> buffer(64u * 4u);
  const auto start = std::chrono::steady_clock::now();
  for (int block = 0; block < 30; ++block)
  {
    REQUIRE(backend->wait_capture(1000, error));
    CHECK(backend->read_capture(buffer.data(), 64, 1000, error) == 64);
    CHECK(backend->write_playback(buffer.data(), 64, 1000, error) == 64);
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  backend->stop();

  // 30 periods of 64 frames at 48 kHz is 40 ms. Sleeping is an absolute deadline, so the run should
  // land within a small margin rather than drifting: if this test needs a wide tolerance the pacing
  // is relative sleeps accumulating their own lateness.
  CHECK(seconds > 0.030);
  CHECK(seconds < 0.080);
}

TEST_CASE("the null backend hands back silence and discards what is written")
{
  namchain::BackendConfig config;
  config.sample_rate = 48000;
  config.period_size = 64;
  config.realtime_paced = false; // no reason to wait in a unit test

  std::string error;
  std::unique_ptr<namchain::AudioBackend> backend =
    namchain::open_backend(config, namchain::BackendKind::Null, error);
  REQUIRE(backend != nullptr);
  REQUIRE(backend->start(error));

  std::vector<unsigned char> buffer(64u * 4u, 0x7F); // pre-filled, so untouched memory is visible
  REQUIRE(backend->read_capture(buffer.data(), 64, 1000, error) == 64);
  for (const unsigned char byte : buffer)
    CHECK(byte == 0);

  backend->stop();
}

TEST_CASE("a backend with no ring never drops a period")
{
  // try_write_playback is how the loop bounds its own latency, and its whole safety argument rests on
  // the default being "always write": a backend with no ring cannot be full, and a dropped period
  // from `file` would silently remove audio from the output WAV instead of failing. So the default
  // has to be checked rather than assumed, and it has to report no drops.
  namchain::BackendConfig config;
  config.sample_rate = 48000;
  config.period_size = 64;
  config.realtime_paced = false; // no reason to wait in a unit test

  std::string error;
  std::unique_ptr<namchain::AudioBackend> backend =
    namchain::open_backend(config, namchain::BackendKind::Null, error);
  REQUIRE(backend != nullptr);
  REQUIRE(backend->start(error));

  std::vector<unsigned char> silence(64u * 4u, 0);
  CHECK(backend->try_write_playback(silence.data(), 64, error) == 64);
  CHECK(backend->try_write_playback(silence.data(), 64, error) == 64);

  const namchain::StreamQueueStats stats = backend->queue_stats();
  CHECK(stats.output_drops == 0);
  CHECK(stats.capture_bursts == 0);
  CHECK(stats.settle_discarded_frames == 0);
  backend->stop();
}

TEST_CASE("a requested backend that does not exist fails by name")
{
  namchain::BackendConfig config;
  std::string error;

  // PipeWire is planned and not implemented. Falling back to another backend would make "measured
  // on PipeWire" unverifiable, so it has to fail here rather than quietly do something else.
  CHECK(namchain::open_backend(config, namchain::BackendKind::PipeWire, error) == nullptr);
  CHECK(error.find("pipewire") != std::string::npos);

  namchain::BackendKind kind = namchain::BackendKind::Null;
  CHECK(namchain::parse_backend_kind("alsa", kind));
  CHECK(kind == namchain::BackendKind::AlsaRaw);
  CHECK(namchain::parse_backend_kind("file", kind));
  CHECK(kind == namchain::BackendKind::File);
  CHECK_FALSE(namchain::parse_backend_kind("coreaudio", kind));
}

TEST_CASE("an out-of-range period is refused rather than rounded")
{
  namchain::BackendConfig config;
  config.period_size = 8192;
  std::string error;
  CHECK(namchain::open_backend(config, namchain::BackendKind::Null, error) == nullptr);
  CHECK_FALSE(error.empty());
}
