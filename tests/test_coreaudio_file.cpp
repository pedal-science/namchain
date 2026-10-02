// Tests for the CoreAudio backend that do not need it to play anything.
//
// Darwin-only: the backend's source is inside `#if defined(__APPLE__)`, so this file is only added to
// the test target on macOS (CMakeLists.txt). Everything here is a refusal path, because a refusal is
// the half of a device backend that can be asserted without a device -- and it is the half whose
// message a person actually reads. The refusals are exercised in the order `create` checks them, so
// each case first satisfies everything before it.
//
// The one case that opens a real device is opt-in (`NAMCHAIN_COREAUDIO_TEST=1`) and plays a short
// blip on whatever the machine's default output is. CI has no CoreAudio at all, and a muted or
// headless developer machine should not fail a unit run because it cannot beep.
#include <doctest.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "namchain/io/backend.h"
#include "namchain/wav.h"

namespace
{

/// The UID of the machine's default input device, or an empty string. Read rather than hardcoded:
/// the point of the live-input case is that it works with whatever this machine has.
std::string default_input_uid()
{
  AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultInputDevice,
                                     kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  AudioObjectID device = kAudioObjectUnknown;
  UInt32 size = sizeof(device);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device) != noErr ||
      device == kAudioObjectUnknown)
    return {};

  AudioObjectPropertyAddress uid_address{kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
  CFStringRef uid = nullptr;
  UInt32 uid_size = sizeof(uid);
  if (AudioObjectGetPropertyData(device, &uid_address, 0, nullptr, &uid_size, &uid) != noErr || uid == nullptr)
    return {};
  char buffer[512] = {0};
  const bool converted = CFStringGetCString(uid, buffer, sizeof(buffer), kCFStringEncodingUTF8);
  CFRelease(uid);
  return converted ? std::string(buffer) : std::string();
}

std::string coreaudio_temp_wav(const char* name)
{
  const std::string path = std::string("/tmp/namchain-coreaudio-") + name + ".wav";
  std::remove(path.c_str());
  return path;
}

/// A 48 kHz mono fixture, which every case past the first needs: without a loadable file the backend
/// refuses before it looks at a device at all.
std::string write_fixture(const char* name, double rate)
{
  const std::string path = coreaudio_temp_wav(name);
  std::vector<float> samples(960, 0.0f);
  for (std::size_t i = 0; i < samples.size(); ++i)
    samples[i] = (i % 2 == 0) ? 0.1f : -0.1f;
  std::string error;
  REQUIRE(namchain::save_wav_float32(path, samples.data(), samples.size(), 1, rate, error));
  return path;
}

namchain::BackendConfig config_for(const std::string& path)
{
  namchain::BackendConfig config;
  config.sample_rate = 48000;
  config.period_size = 64;
  config.periods = 4;
  config.capture_channels = 1;
  config.playback_channels = 1;
  config.input_path = path;
  return config;
}

} // namespace

TEST_CASE("a coreaudio backend with no capture path is refused before it looks at a device")
{
  namchain::BackendConfig config;
  config.sample_rate = 48000;
  config.period_size = 64;
  std::string error;

  CHECK(namchain::open_backend(config, namchain::BackendKind::CoreAudio, error) == nullptr);
  CHECK(error.find("--capture-path") != std::string::npos);
}

TEST_CASE("a coreaudio capture path at the wrong rate is refused, naming both rates")
{
  const std::string path = write_fixture("rate", 44100.0);
  std::string error;

  // The file is refused by the shared loop loader, before any device is opened: the engine has no
  // resampler, so this is the same rule `pipe` enforces and the same message.
  CHECK(namchain::open_backend(config_for(path), namchain::BackendKind::CoreAudio, error) == nullptr);
  CHECK(error.find("44100") != std::string::npos);
  CHECK(error.find("48000") != std::string::npos);

  std::remove(path.c_str());
}

TEST_CASE("a coreaudio capture path that does not exist is refused by name")
{
  namchain::BackendConfig config = config_for("/tmp/namchain-coreaudio-absent.wav");
  std::remove(config.input_path.c_str());
  std::string error;

  CHECK(namchain::open_backend(config, namchain::BackendKind::CoreAudio, error) == nullptr);
  CHECK(error.find(config.input_path) != std::string::npos);
}

TEST_CASE("a coreaudio device UID that does not exist is refused, naming the UID")
{
  const std::string path = write_fixture("uid", 48000.0);
  namchain::BackendConfig config = config_for(path);
  // A UID is a CoreAudio device UID, not a name or an index: this string cannot resolve, which is
  // the point -- the refusal has to say which UID it could not find.
  config.device = "namchain-no-such-device";
  std::string error;

  CHECK(namchain::open_backend(config, namchain::BackendKind::CoreAudio, error) == nullptr);
  CHECK(error.find("namchain-no-such-device") != std::string::npos);

  std::remove(path.c_str());
}

TEST_CASE("a coreaudio backend given both capture sources is refused, naming both")
{
  const std::string path = write_fixture("both", 48000.0);
  namchain::BackendConfig config = config_for(path);
  // A device to capture from and a file to loop are alternatives, not layers: one of them would have
  // to win silently, and "which one" is exactly what the operator cannot tell from the sound.
  config.input_device = "namchain-any-device";
  std::string error;

  CHECK(namchain::open_backend(config, namchain::BackendKind::CoreAudio, error) == nullptr);
  CHECK(error.find("--input-device") != std::string::npos);
  CHECK(error.find("--capture-path") != std::string::npos);
  CHECK(error.find("not both") != std::string::npos);

  std::remove(path.c_str());
}

TEST_CASE("a coreaudio input device UID that does not exist is refused, naming the UID")
{
  // No capture path at all: this is the live-input shape, and the UID is the only thing wrong with it.
  namchain::BackendConfig config;
  config.sample_rate = 48000;
  config.period_size = 1024;
  config.capture_channels = 1;
  config.playback_channels = 1;
  config.input_device = "namchain-no-such-input";
  std::string error;

  CHECK(namchain::open_backend(config, namchain::BackendKind::CoreAudio, error) == nullptr);
  CHECK(error.find("namchain-no-such-input") != std::string::npos);
}

TEST_CASE("a live coreaudio input cannot be asked for more channels than the device has")
{
  // No hardware to open and no permission needed: the channel count comes from the device's stream
  // configuration, which is a property read. 32 is more than any input device this will meet, so the
  // refusal is reached whichever machine this runs on.
  const std::string uid = default_input_uid();
  if (uid.empty())
  {
    MESSAGE("no default input device; the live-input channel case is skipped");
    return;
  }

  namchain::BackendConfig config;
  config.sample_rate = 48000;
  config.period_size = 1024;
  config.capture_channels = 32;
  config.playback_channels = 1;
  config.input_device = uid;
  std::string error;

  CHECK(namchain::open_backend(config, namchain::BackendKind::CoreAudio, error) == nullptr);
  CHECK(error.find("input channels") != std::string::npos);
}

TEST_CASE("a coreaudio backend captures from a live input device")
{
  // Opt-in, and its own variable rather than the file one: opening an input device is what makes
  // macOS ask for microphone permission, and a run that only wanted to hear a file must not be asked.
  if (std::getenv("NAMCHAIN_COREAUDIO_INPUT_TEST") == nullptr)
    return;

  const std::string uid = default_input_uid();
  if (uid.empty())
  {
    MESSAGE("no default input device; the live-input case is skipped");
    return;
  }

  namchain::BackendConfig config;
  config.sample_rate = 48000;
  config.period_size = 1024;  // a ceiling, as everywhere: the device's own period is measured
  config.periods = 4;
  config.capture_channels = 1;
  config.playback_channels = 1;
  config.input_device = uid;
  std::string error;

  std::unique_ptr<namchain::AudioBackend> backend =
    namchain::open_backend(config, namchain::BackendKind::CoreAudio, error);
  REQUIRE_MESSAGE(backend != nullptr, error);

  const namchain::BackendInfo& info = backend->info();
  CHECK(info.period_size >= 1);
  CHECK(info.period_size <= config.period_size);
  CHECK(info.sample_rate == 48000);
  CHECK(info.capture_channels == 1);
  CHECK(info.format == namchain::PcmFormat::F32_LE);
  // Named after the device rather than a file, which is how a status page says where the capture is
  // coming from without knowing about this flag.
  CHECK(info.capture_name.find("(input)") != std::string::npos);
  // A device's first period is an artefact of its own ring filling, unlike a file's, so the daemon is
  // told to discard it rather than to render it.
  CHECK(info.needs_bringup_cycle == true);

  REQUIRE_MESSAGE(backend->start(error), error);
  const std::size_t capture_bytes = static_cast<std::size_t>(info.period_size) * info.capture_bytes_per_frame;
  const std::size_t playback_bytes = static_cast<std::size_t>(info.period_size) * info.playback_bytes_per_frame;
  std::vector<unsigned char> capture(capture_bytes, 0);
  std::vector<unsigned char> playback(playback_bytes, 0);

  for (int period = 0; period < 15; ++period)
  {
    REQUIRE(backend->wait_capture(2000, error));
    CHECK(backend->read_capture(capture.data(), info.period_size, 1000, error) ==
          static_cast<int>(info.period_size));
    CHECK(backend->try_write_playback(playback.data(), info.period_size, error) >= 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  // A live input runs for as long as the device does, so it never reports itself exhausted.
  CHECK(info.capture_exhausted == false);

  backend->stop();
}

TEST_CASE("a coreaudio backend opens and runs a looped file on the default device")
{
  // Opt-in: this plays 200 ms of a square wave through the machine's default output device.
  if (std::getenv("NAMCHAIN_COREAUDIO_TEST") == nullptr)
    return;

  const std::string path = write_fixture("run", 48000.0);
  namchain::BackendConfig config = config_for(path);
  // A ceiling for the plan, not a requested device quantum: the backend measures the device's own
  // period and reports it, and 1024 is above anything a Mac's default output device delivers.
  config.period_size = 1024;
  std::string error;

  std::unique_ptr<namchain::AudioBackend> backend =
    namchain::open_backend(config, namchain::BackendKind::CoreAudio, error);
  REQUIRE_MESSAGE(backend != nullptr, error);

  const namchain::BackendInfo& info = backend->info();
  // The device's number, measured rather than echoed from the configuration.
  CHECK(info.period_size >= 1);
  CHECK(info.period_size <= config.period_size);
  CHECK(info.sample_rate == 48000);
  CHECK(info.capture_channels == 1);
  CHECK(info.playback_channels >= 1);
  CHECK(info.format == namchain::PcmFormat::F32_LE);
  CHECK(info.capture_name.find("looped") != std::string::npos);
  // The callback produces input from its first call, so there is no bring-up artefact to run off.
  CHECK(info.needs_bringup_cycle == false);

  const bool started = backend->start(error);
  MESSAGE("start: ", error);
  REQUIRE_MESSAGE(started, error);
  // One period in, one period out, then out again -- enough to know the stream actually runs rather
  // than merely starting.
  const std::size_t capture_bytes = static_cast<std::size_t>(info.period_size) * info.capture_bytes_per_frame;
  const std::size_t playback_bytes = static_cast<std::size_t>(info.period_size) * info.playback_bytes_per_frame;
  std::vector<unsigned char> capture(capture_bytes, 0);
  std::vector<unsigned char> playback(playback_bytes, 0);

  for (int period = 0; period < 15; ++period)
  {
    REQUIRE(backend->wait_capture(2000, error));
    CHECK(backend->read_capture(capture.data(), info.period_size, 1000, error) ==
          static_cast<int>(info.period_size));
    CHECK(backend->try_write_playback(playback.data(), info.period_size, error) >= 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  // The loop is one file long and reads for ever, so it is never exhausted -- the opposite of `file`.
  CHECK(info.capture_exhausted == false);

  long long capture_frames = 0;
  long long playback_frames = 0;
  CHECK(backend->ring_occupancy(capture_frames, playback_frames));
  CHECK(capture_frames >= 0);
  CHECK(playback_frames >= 0);

  backend->stop();

  std::remove(path.c_str());
}
