// Tests for LoopSource: the WAV loop the `pipe` and `coreaudio` backends both read from.
//
// The wrap rule is the whole reason this is its own class rather than forty lines inside each
// backend, so it is what these tests are about. A loop that wraps once per *call* still sounds
// plausible -- it repeats the file, roughly -- and quietly loses the tail of any file whose length is
// not a whole number of periods, which is every fixture anybody writes by hand.
#include <doctest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "namchain/io/loop_source.h"
#include "namchain/wav.h"

namespace
{

std::string loop_temp_wav(const char* name)
{
  const std::string path = std::string("/tmp/namchain-loop-") + name + ".wav";
  std::remove(path.c_str());
  return path;
}

/// A ramp: every sample distinct, so a reader that drops or repeats one cannot pass by producing the
/// right number of samples.
std::vector<float> ramp(std::size_t count)
{
  std::vector<float> samples(count);
  for (std::size_t i = 0; i < count; ++i)
    samples[i] = static_cast<float>(i) + 1.0f;
  return samples;
}

} // namespace

TEST_CASE("a loop wraps per frame, not per call, so a partial tail survives")
{
  // 5 frames, read 2 at a time: the third read straddles the end and must return 5 then 1, not 5
  // followed by a silent tail or a restart at the top.
  const std::string path = loop_temp_wav("straddle");
  const std::vector<float> source = ramp(5);
  std::string error;
  REQUIRE(namchain::save_wav_float32(path, source.data(), source.size(), 1, 48000.0, error));

  namchain::LoopSource loop;
  REQUIRE(namchain::LoopSource::load(path, 48000, loop, error));
  CHECK(loop.frames() == 5);
  CHECK(loop.channels() == 1);

  float period[2] = {0.0f, 0.0f};
  loop.read(period, 2);
  CHECK(period[0] == doctest::Approx(1.0));
  CHECK(period[1] == doctest::Approx(2.0));
  loop.read(period, 2);
  CHECK(period[0] == doctest::Approx(3.0));
  CHECK(period[1] == doctest::Approx(4.0));
  loop.read(period, 2);
  CHECK(period[0] == doctest::Approx(5.0));
  CHECK(period[1] == doctest::Approx(1.0));
  // One frame into the second pass, so the next frame out is the file's second.
  CHECK(loop.position() == 1);

  std::remove(path.c_str());
}

TEST_CASE("a loop reads past a whole file without losing a sample")
{
  // The same property from the other side: two passes of a 3-frame file read 2 at a time must be
  // exactly 1 2 3 1 2 3.
  const std::string path = loop_temp_wav("two-passes");
  const std::vector<float> source = ramp(3);
  std::string error;
  REQUIRE(namchain::save_wav_float32(path, source.data(), source.size(), 1, 48000.0, error));

  namchain::LoopSource loop;
  REQUIRE(namchain::LoopSource::load(path, 48000, loop, error));

  std::vector<float> got(6, 0.0f);
  for (int i = 0; i < 3; ++i)
    loop.read(got.data() + i * 2, 2);
  for (std::size_t i = 0; i < got.size(); ++i)
    CHECK(got[i] == doctest::Approx(static_cast<float>(i % 3) + 1.0f));

  std::remove(path.c_str());
}

TEST_CASE("reset rewinds, so a reopened device starts in the same place")
{
  const std::string path = loop_temp_wav("reset");
  const std::vector<float> source = ramp(4);
  std::string error;
  REQUIRE(namchain::save_wav_float32(path, source.data(), source.size(), 1, 48000.0, error));

  namchain::LoopSource loop;
  REQUIRE(namchain::LoopSource::load(path, 48000, loop, error));

  float one = 0.0f;
  loop.read(&one, 1);
  loop.read(&one, 1);
  CHECK(one == doctest::Approx(2.0));

  loop.reset();
  CHECK(loop.position() == 0);
  loop.read(&one, 1);
  CHECK(one == doctest::Approx(1.0));

  std::remove(path.c_str());
}

TEST_CASE("a loop keeps the file's channel count and interleaving")
{
  const std::string path = loop_temp_wav("stereo");
  // Frame 0 is (1, -1), frame 1 is (2, -2), and the reader asks for 3 frames so it must wrap mid-read.
  const std::vector<float> source{1.0f, -1.0f, 2.0f, -2.0f};
  std::string error;
  REQUIRE(namchain::save_wav_float32(path, source.data(), 2, 2, 48000.0, error));

  namchain::LoopSource loop;
  REQUIRE(namchain::LoopSource::load(path, 48000, loop, error));
  CHECK(loop.channels() == 2);
  CHECK(loop.frames() == 2);

  std::vector<float> got(6, 0.0f);
  loop.read(got.data(), 3);
  CHECK(got[0] == doctest::Approx(1.0));
  CHECK(got[1] == doctest::Approx(-1.0));
  CHECK(got[2] == doctest::Approx(2.0));
  CHECK(got[3] == doctest::Approx(-2.0));
  CHECK(got[4] == doctest::Approx(1.0)); // wrapped
  CHECK(got[5] == doctest::Approx(-1.0));

  std::remove(path.c_str());
}

TEST_CASE("a loop at another rate is refused, naming both rates")
{
  const std::string path = loop_temp_wav("rate");
  const std::vector<float> source = ramp(4);
  std::string error;
  REQUIRE(namchain::save_wav_float32(path, source.data(), source.size(), 1, 44100.0, error));

  namchain::LoopSource loop;
  CHECK_FALSE(namchain::LoopSource::load(path, 48000, loop, error));
  CHECK(error.find("44100") != std::string::npos);
  CHECK(error.find("48000") != std::string::npos);
  CHECK(error.find(path) != std::string::npos);

  std::remove(path.c_str());
}

TEST_CASE("a loop over nothing is refused rather than playing silence for ever")
{
  const std::string path = loop_temp_wav("empty");
  std::vector<float> empty;
  std::string error;
  // A zero-frame WAV is what a truncated export looks like, and an infinite silence is exactly the
  // failure that would look like a working device.
  REQUIRE(namchain::save_wav_float32(path, empty.data(), 0, 1, 48000.0, error));

  namchain::LoopSource loop;
  CHECK_FALSE(namchain::LoopSource::load(path, 48000, loop, error));
  CHECK(error.find("no frames to loop") != std::string::npos);

  std::remove(path.c_str());
}

TEST_CASE("a filename that does not exist is refused with the loader's own reason")
{
  const std::string path = "/tmp/namchain-loop-does-not-exist.wav";
  std::remove(path.c_str());

  namchain::LoopSource loop;
  std::string error;
  CHECK_FALSE(namchain::LoopSource::load(path, 48000, loop, error));
  CHECK_FALSE(error.empty());
}
