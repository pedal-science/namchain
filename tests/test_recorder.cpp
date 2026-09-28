// Tests for the daemon's diagnostic recorder: what it writes is what it was given, period for period,
// and the events land at the right positions.
#include <doctest.h>

#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "namchain/daemon/recorder.h"
#include "namchain/wav.h"

using namchain::PcmFormat;
using namchain::daemon::Recorder;
using namchain::daemon::RecorderEvents;
using namchain::daemon::RecorderFormat;

namespace
{

std::string fresh_dir(const char* name)
{
  const std::string path = std::string("/tmp/namchain-test-recorder-") + name + "-" + std::to_string(::getpid());
  std::filesystem::remove_all(path);
  return path;
}

RecorderFormat small_format()
{
  RecorderFormat format;
  format.sample_rate = 48000;
  format.period_size = 16;
  format.capture_channels = 3;
  format.playback_channels = 2;
  format.format = PcmFormat::S32_LE;
  return format;
}

/// A period whose every sample says where it is: period, frame and channel.
std::vector<int32_t> period_of(unsigned period, unsigned frames, unsigned channels, int32_t salt)
{
  std::vector<int32_t> out(static_cast<std::size_t>(frames) * channels);
  for (unsigned f = 0; f < frames; ++f)
    for (unsigned c = 0; c < channels; ++c)
      out[static_cast<std::size_t>(f) * channels + c] =
        static_cast<int32_t>((period * frames + f) * 256 + c * 16 + salt) << 8;
  return out;
}

std::string read_text(const std::string& path)
{
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

} // namespace

TEST_CASE("recorder: both files hold exactly the periods pushed, with events at their positions")
{
  const std::string dir = fresh_dir("roundtrip");
  const RecorderFormat format = small_format();
  std::string error;
  std::unique_ptr<Recorder> recorder = Recorder::start(dir, format, 10.0, "{\"test\": true}", error);
  REQUIRE_MESSAGE(recorder != nullptr, error);

  constexpr unsigned kPeriods = 50;
  for (unsigned p = 0; p < kPeriods; ++p)
  {
    const std::vector<int32_t> capture = period_of(p, format.period_size, format.capture_channels, 1);
    const std::vector<int32_t> playback = period_of(p, format.period_size, format.playback_channels, 2);
    RecorderEvents events;
    if (p == 7)
      events.dropped = 1;
    if (p == 20)
      events.inserted = 2;
    CHECK(recorder->push(capture.data(), capture.size() * 4, playback.data(), playback.size() * 4, events));
  }
  recorder->stop();
  CHECK(recorder->periods() == kPeriods);
  CHECK(recorder->dropped() == 1);
  CHECK(recorder->inserted() == 2);
  CHECK(recorder->overflows() == 0);
  CHECK(recorder->write_error().empty());

  std::vector<float> samples;
  unsigned channels = 0;
  double rate = 0;
  REQUIRE_MESSAGE(namchain::load_wav(dir + "/capture.wav", samples, channels, rate, error), error);
  CHECK(channels == format.capture_channels);
  CHECK(rate == 48000.0);
  REQUIRE(samples.size() == static_cast<std::size_t>(kPeriods) * format.period_size * format.capture_channels);

  // The raw bytes, not just the decode: a recording is for comparing bit for bit.
  std::ifstream raw(dir + "/playback.wav", std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(raw)), std::istreambuf_iterator<char>());
  REQUIRE(bytes.size() == 44 + static_cast<std::size_t>(kPeriods) * format.period_size * format.playback_channels * 4);
  const std::vector<int32_t> expected = period_of(33, format.period_size, format.playback_channels, 2);
  const std::size_t offset = 44 + static_cast<std::size_t>(33) * format.period_size * format.playback_channels * 4;
  CHECK(std::memcmp(bytes.data() + offset, expected.data(), expected.size() * 4) == 0);

  const std::string events = read_text(dir + "/events.tsv");
  CHECK(events.find("7\t112\tdropped\t1\n") != std::string::npos);
  CHECK(events.find("20\t320\tinserted\t2\n") != std::string::npos);
  CHECK(read_text(dir + "/meta.json") == "{\"test\": true}\n");
  std::filesystem::remove_all(dir);
}

TEST_CASE("recorder: ends itself at its length, and on a change of shape")
{
  const RecorderFormat format = small_format();
  const std::vector<int32_t> capture = period_of(0, format.period_size, format.capture_channels, 1);
  const std::vector<int32_t> playback = period_of(0, format.period_size, format.playback_channels, 2);
  std::string error;

  const std::string dir = fresh_dir("length");
  // Ten periods' worth of seconds.
  std::unique_ptr<Recorder> recorder =
    Recorder::start(dir, format, 10.0 * format.period_size / format.sample_rate, "{}", error);
  REQUIRE_MESSAGE(recorder != nullptr, error);
  unsigned accepted = 0;
  for (unsigned p = 0; p < 25; ++p)
    accepted += recorder->push(capture.data(), capture.size() * 4, playback.data(), playback.size() * 4, {}) ? 1 : 0;
  CHECK(accepted == 10);
  CHECK(recorder->complete());
  recorder->stop();
  std::filesystem::remove_all(dir);

  const std::string other = fresh_dir("shape");
  recorder = Recorder::start(other, format, 10.0, "{}", error);
  REQUIRE_MESSAGE(recorder != nullptr, error);
  CHECK(recorder->push(capture.data(), capture.size() * 4, playback.data(), playback.size() * 4, {}));
  CHECK_FALSE(recorder->push(capture.data(), capture.size() * 4 - 4, playback.data(), playback.size() * 4, {}));
  CHECK(recorder->complete());
  recorder->stop();
  std::filesystem::remove_all(other);
}

TEST_CASE("recorder: refuses a directory that already exists, and a stream that is not running")
{
  const std::string dir = fresh_dir("exists");
  std::filesystem::create_directories(dir);
  std::string error;
  CHECK(Recorder::start(dir, small_format(), 1.0, "{}", error) == nullptr);
  CHECK(error.find("already exists") != std::string::npos);
  std::filesystem::remove_all(dir);

  RecorderFormat idle;
  CHECK(Recorder::start(fresh_dir("idle"), idle, 1.0, "{}", error) == nullptr);
}
