// The CoreAudio backend. See namchain/io/coreaudio.h for what it is for and why it captures from a
// file rather than from an input device.
//
// The shape is forced by one fact: the interface is a pull (wait_capture -> read_capture -> DSP ->
// write_playback) and CoreAudio is a push (an IOProc is called with a buffer to fill). So the IOProc
// produces capture periods into a ring and drains processed periods from another, and the engine
// thread keeps its existing loop. Both rings are single-producer/single-consumer, which is the case
// rt::SpscRing is built for, so neither side of the handoff allocates or locks.
#include "namchain/io/coreaudio.h"

#if defined(__APPLE__)

  #include <CoreAudio/CoreAudio.h>
  #include <CoreFoundation/CoreFoundation.h>
  #include <dispatch/dispatch.h>

  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <cstdio>
  #include <cstring>
  #include <string>
  #include <thread>
  #include <unistd.h>
  #include <vector>

  #include "namchain/io/loop_source.h"
  #include "namchain/pcm.h"
  #include "namchain/rt/spsc_ring.h"

namespace namchain
{
namespace
{

using rt::SpscRing;

/// How long start() waits for the device's first callback before calling it a device that will not
/// run. The precedent is kPipeOpenTimeoutMs: long enough for a device that is coming up, short
/// enough that a daemon started with nothing to play to reports a lost device rather than hanging.
constexpr int kCoreAudioStartTimeoutMs = 2000;

constexpr AudioObjectPropertyAddress global_address(AudioObjectPropertySelector selector)
{
  return AudioObjectPropertyAddress{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}

/// Every HAL call returns an OSStatus whose interesting value is zero. Unchecked, a failure here
/// shows up later as a device that will not start, which is a much worse diagnostic.
bool ca_ok(OSStatus status) { return status == noErr; }

std::string os_status_text(OSStatus status)
{
  const unsigned value = static_cast<unsigned>(status);
  const char text[5] = {static_cast<char>((value >> 24) & 0xff), static_cast<char>((value >> 16) & 0xff),
                        static_cast<char>((value >> 8) & 0xff), static_cast<char>(value & 0xff), 0};
  bool printable = true;
  for (int i = 0; i < 4; ++i)
    if (text[i] < 32 || text[i] > 126)
      printable = false;
  char buffer[64];
  if (printable)
    std::snprintf(buffer, sizeof(buffer), "%.4s (%d)", text, static_cast<int>(status));
  else
    std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(status));
  return buffer;
}

template <typename T>
bool get_property(AudioObjectID object, AudioObjectPropertyAddress address, T& value)
{
  UInt32 size = sizeof(T);
  return ca_ok(AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value));
}

template <typename T>
bool set_property(AudioObjectID object, AudioObjectPropertyAddress address, const T& value)
{
  return ca_ok(AudioObjectSetPropertyData(object, &address, 0, nullptr, sizeof(T), &value));
}

/// The user-visible name of a device, for `engine.status`'s `playback_name`. Best effort: a device
/// with no readable name is "device N", which is still better than an empty string.
std::string device_name(AudioObjectID device)
{
  CFStringRef name = nullptr;
  const AudioObjectPropertyAddress address = global_address(kAudioObjectPropertyName);
  UInt32 size = sizeof(name);
  if (!ca_ok(AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &name)) || name == nullptr)
    return "device " + std::to_string(device);
  char buffer[256] = {0};
  const bool converted = CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
  CFRelease(name);
  return converted ? std::string(buffer) : ("device " + std::to_string(device));
}

/// \brief Silence the whole device buffer.
///
/// The layout does not matter here: every byte of every buffer is zeroed, and zero float bytes are
/// zero floats in both an interleaved and a planar buffer. This is what a callback this backend
/// cannot serve must hand the device -- *not* the buffer as it found it, which the HAL hands back to
/// the next callback and which therefore plays the previous callback's audio again.
void zero_output(AudioBufferList* output)
{
  if (output == nullptr)
    return;
  for (UInt32 b = 0; b < output->mNumberBuffers; ++b)
  {
    if (output->mBuffers[b].mData != nullptr)
      std::memset(output->mBuffers[b].mData, 0, output->mBuffers[b].mDataByteSize);
  }
}

/// The same address with an explicit scope. Only the capture side needs one: a device with both
/// directions has an input format and an input latency that are not the output's, and asking at
/// global scope gives whichever of the two the HAL decides to answer with.
constexpr AudioObjectPropertyAddress scoped_address(AudioObjectPropertySelector selector,
                                                    AudioObjectPropertyScope scope)
{
  return {selector, scope, kAudioObjectPropertyElementMain};
}

/// \brief A device's UID, or an empty string.
///
/// Needed because an aggregate device is built out of **UIDs** and the operator may have named
/// neither device -- an empty UID means "the system's default", and the aggregate cannot be told
/// "whatever the default is": it wants the string.
std::string device_uid(AudioObjectID device)
{
  CFStringRef uid = nullptr;
  const AudioObjectPropertyAddress address = global_address(kAudioDevicePropertyDeviceUID);
  UInt32 size = sizeof(uid);
  if (!ca_ok(AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &uid)) || uid == nullptr)
    return {};
  char buffer[256] = {0};
  const bool converted = CFStringGetCString(uid, buffer, sizeof(buffer), kCFStringEncodingUTF8);
  CFRelease(uid);
  return converted ? std::string(buffer) : std::string();
}

/// \brief The device a UID names, or kAudioObjectUnknown with \p error set.
///
/// Matched by reading every device's `kAudioDevicePropertyDeviceUID`, **not** through
/// `kAudioHardwarePropertyTranslateUIDToDevice`. That property is the documented way to do this and
/// it does not work here: on macOS 27 it refuses every size this code can offer it with
/// `kAudioHardwareBadPropertySizeError` (`!siz`), including the size of `AudioValueTranslation`
/// itself, which is 32 bytes in this SDK. Nothing in the parked work noticed, because the parked
/// work only ever opened the *default* device -- an empty UID -- and the one refusal test passes a
/// deliberately bogus UID, which fails the same way a working call would refuse a bad string. It was
/// found the first time a real UID was given, which is what live input requires.
///
/// Enumerating costs a handful of property reads once, at open time, and it reads the very string it
/// compares against, so the two cannot disagree about what a device's UID is.
AudioObjectID device_for_uid(const std::string& uid, std::string& error)
{
  AudioObjectPropertyAddress address = global_address(kAudioHardwarePropertyDevices);
  UInt32 size = 0;
  if (!ca_ok(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size)) ||
      size < sizeof(AudioObjectID))
  {
    error = "the CoreAudio device list could not be read";
    return kAudioObjectUnknown;
  }
  std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
  if (!ca_ok(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data())))
  {
    error = "the CoreAudio device list could not be read";
    return kAudioObjectUnknown;
  }
  for (AudioObjectID device : devices)
  {
    if (device_uid(device) == uid)
      return device;
  }
  error = "no CoreAudio device with UID '" + uid + "'";
  return kAudioObjectUnknown;
}

/// A system default device, named in the error so a caller can say which direction is missing.
AudioObjectID default_device(AudioObjectPropertySelector selector, const char* direction, std::string& error)
{
  AudioObjectID device = kAudioObjectUnknown;
  if (!get_property(kAudioObjectSystemObject, global_address(selector), device) ||
      device == kAudioObjectUnknown)
  {
    error = std::string("there is no default CoreAudio ") + direction + " device";
    return kAudioObjectUnknown;
  }
  return device;
}

/// \brief How many channels a device's input scope presents, or 0 when it has none.
///
/// `kAudioDevicePropertyStreamConfiguration` rather than the stream format: the format describes one
/// stream, and a device is free to present several (an interface with two stereo pairs is four
/// channels in two two-channel buffers). The sum over the buffers is the number a chain's
/// `input_channel` is an index into.
unsigned input_channel_count(AudioObjectID device)
{
  const AudioObjectPropertyAddress address =
    scoped_address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput);
  UInt32 size = 0;
  if (!ca_ok(AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size)) ||
      size < sizeof(AudioBufferList))
    return 0;
  std::vector<unsigned char> storage(size);
  AudioBufferList* list = reinterpret_cast<AudioBufferList*>(storage.data());
  if (!ca_ok(AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list)))
    return 0;
  unsigned channels = 0;
  for (UInt32 b = 0; b < list->mNumberBuffers; ++b)
    channels += list->mBuffers[b].mNumberChannels;
  return channels;
}

/// \brief The CFString keys an aggregate device's description is built from.
///
/// The SDK's `kAudioAggregateDevice*Key` names are **C string literals**
/// (`#define kAudioAggregateDeviceNameKey "name"`), not CFStrings, so each one has to be turned into a
/// CFString before it can key a CFDictionary. Passing the constants straight in is what crashed the
/// first aggregate this backend made: `CFDictionarySetValue` retained a `char *` and sent an
/// Objective-C message to a string. Owning them here is also what makes the releases at the end
/// impossible to forget.
struct AggregateKeys
{
  CFStringRef name = nullptr;
  CFStringRef uid = nullptr;
  CFStringRef is_private = nullptr;
  CFStringRef is_stacked = nullptr;
  CFStringRef main_sub_device = nullptr;
  CFStringRef sub_device_list = nullptr;
  CFStringRef sub_device_uid = nullptr;
  CFStringRef drift_compensation = nullptr;

  static CFStringRef make(const char* text)
  {
    return CFStringCreateWithCString(kCFAllocatorDefault, text, kCFStringEncodingUTF8);
  }

  bool make_all()
  {
    name = make(kAudioAggregateDeviceNameKey);
    uid = make(kAudioAggregateDeviceUIDKey);
    is_private = make(kAudioAggregateDeviceIsPrivateKey);
    is_stacked = make(kAudioAggregateDeviceIsStackedKey);
    main_sub_device = make(kAudioAggregateDeviceMainSubDeviceKey);
    sub_device_list = make(kAudioAggregateDeviceSubDeviceListKey);
    sub_device_uid = make(kAudioSubDeviceUIDKey);
    drift_compensation = make(kAudioSubDeviceDriftCompensationKey);
    return name != nullptr && uid != nullptr && is_private != nullptr && is_stacked != nullptr &&
           main_sub_device != nullptr && sub_device_list != nullptr && sub_device_uid != nullptr &&
           drift_compensation != nullptr;
  }

  void release_all()
  {
    for (CFStringRef* key : {&name, &uid, &is_private, &is_stacked, &main_sub_device, &sub_device_list,
                             &sub_device_uid, &drift_compensation})
    {
      if (*key != nullptr)
        CFRelease(*key);
      *key = nullptr;
    }
  }
};

/// \brief A private aggregate device whose clock master is \p master and whose other member is
///        \p follower, or kAudioObjectUnknown with \p error set.
///
/// This is the answer to "the input and the output are different devices". Two devices have two
/// clocks, this engine has no resampler, and running one IOProc on each would therefore drift: at a
/// hundred parts per million -- ordinary for two nominally-48 kHz devices -- that is a period dropped
/// or inserted every couple of minutes, counted as an xrun and indistinguishable from the engine
/// being late. An aggregate device is the HAL's own answer: one clock, the master's, with the other
/// member drift-compensated, which means resampled by the HAL to follow it. That is exactly the code
/// this project does not have and does not want.
///
/// The **output** is the master, for the same reason the output is the clock everywhere else in this
/// backend: the engine's plan is compiled for the output device's period and the output's callback is
/// what `wait_capture` is paced by. The input is the member that gets resampled, which is the cheaper
/// mistake -- a few parts per million of pitch on a guitar rather than on what the player hears.
///
/// The aggregate is **private**: the HAL keeps it out of Audio MIDI Setup, and it belongs to this
/// process, so it is destroyed in `stop()` rather than left behind on somebody's machine.
AudioObjectID make_aggregate(AudioObjectID master, AudioObjectID follower, std::string& error)
{
  const std::string master_uid = device_uid(master);
  const std::string follower_uid = device_uid(follower);
  if (master_uid.empty() || follower_uid.empty())
  {
    error = "one of the two CoreAudio devices has no UID, so an aggregate device cannot name it";
    return kAudioObjectUnknown;
  }

  // The aggregate needs a UID of its own, unique among the live ones. The process id and a counter
  // are enough: it is private to this process and goes away with it.
  static std::atomic<unsigned> counter{0};
  const std::string aggregate_uid = "cc.hemsley.namchain.aggregate." +
                                    std::to_string(static_cast<long long>(::getpid())) + "." +
                                    std::to_string(counter.fetch_add(1, std::memory_order_relaxed));

  AggregateKeys keys;
  if (!keys.make_all())
  {
    keys.release_all();
    error = "could not build the aggregate device's keys";
    return kAudioObjectUnknown;
  }

  CFStringRef master_string =
    CFStringCreateWithCString(kCFAllocatorDefault, master_uid.c_str(), kCFStringEncodingUTF8);
  CFStringRef follower_string =
    CFStringCreateWithCString(kCFAllocatorDefault, follower_uid.c_str(), kCFStringEncodingUTF8);
  CFStringRef aggregate_string =
    CFStringCreateWithCString(kCFAllocatorDefault, aggregate_uid.c_str(), kCFStringEncodingUTF8);
  CFMutableDictionaryRef description =
    CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFMutableArrayRef subdevices = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
  CFMutableDictionaryRef master_entry =
    CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFMutableDictionaryRef follower_entry =
    CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (master_string == nullptr || follower_string == nullptr || aggregate_string == nullptr ||
      description == nullptr || subdevices == nullptr || master_entry == nullptr || follower_entry == nullptr)
  {
    if (master_string != nullptr)
      CFRelease(master_string);
    if (follower_string != nullptr)
      CFRelease(follower_string);
    if (aggregate_string != nullptr)
      CFRelease(aggregate_string);
    if (description != nullptr)
      CFRelease(description);
    if (subdevices != nullptr)
      CFRelease(subdevices);
    if (master_entry != nullptr)
      CFRelease(master_entry);
    if (follower_entry != nullptr)
      CFRelease(follower_entry);
    keys.release_all();
    error = "could not build the aggregate device's description";
    return kAudioObjectUnknown;
  }

  CFDictionarySetValue(description, keys.name, CFSTR("namchain (private)"));
  CFDictionarySetValue(description, keys.uid, aggregate_string);
  CFDictionarySetValue(description, keys.is_private, kCFBooleanTrue);
  CFDictionarySetValue(description, keys.is_stacked, kCFBooleanFalse);
  // Named as well as ordered first: "the master is the first sub-device" is the documented rule, and
  // saying which one it is costs nothing and survives a reordering.
  CFDictionarySetValue(description, keys.main_sub_device, master_string);

  CFDictionarySetValue(master_entry, keys.sub_device_uid, master_string);
  CFDictionarySetValue(master_entry, keys.drift_compensation, kCFBooleanFalse);
  CFDictionarySetValue(follower_entry, keys.sub_device_uid, follower_string);
  CFDictionarySetValue(follower_entry, keys.drift_compensation, kCFBooleanTrue);
  CFArrayAppendValue(subdevices, master_entry);
  CFArrayAppendValue(subdevices, follower_entry);
  CFDictionarySetValue(description, keys.sub_device_list, subdevices);

  AudioObjectID aggregate = kAudioObjectUnknown;
  const OSStatus status = AudioHardwareCreateAggregateDevice(description, &aggregate);

  // Released here: the HAL has copied what it needs, and from now on the aggregate is addressed by
  // its object id (and destroyed by it).
  CFRelease(master_entry);
  CFRelease(follower_entry);
  CFRelease(subdevices);
  CFRelease(description);
  CFRelease(master_string);
  CFRelease(follower_string);
  CFRelease(aggregate_string);
  keys.release_all();

  if (!ca_ok(status) || aggregate == kAudioObjectUnknown)
  {
    error = "could not make a private aggregate device of the output and input devices: " +
            os_status_text(status);
    return kAudioObjectUnknown;
  }
  return aggregate;
}

/// \brief How many frames this device hands the IOProc, measured rather than asked for.
///
/// `kAudioDevicePropertyBufferFrameSize` is a *request*: on this project's own machine it reads back
/// as whatever was written while the callback is handed something else entirely, and what it is
/// handed is not constant -- a device coming up was seen at 128 frames and later at 1024. The engine
/// compiles its plan for one quantum (§9.2), so the number that matters is the one the callback
/// actually gets, and the safe reading of "not constant" is the *smallest* one seen: a plan must
/// carry every frame of every callback, and a period shorter than the usual one is still a period.
///
/// **Frames, not samples.** `mDataByteSize` is bytes for every channel of the buffer, so 4096 bytes
/// at two channels of float is 1024 samples and 512 frames. Dividing by the sample size alone once
/// doubled every figure that came out of this function -- see `local_engine_probe` in the app's own
/// history for how long that took to notice. The channel count is not optional.
bool measure_device(AudioObjectID device, unsigned& frames, std::string& error)
{
  // Thirty-two callbacks: long enough that a rate derived from host time is steady (a short run
  // over-reads it), and short enough not to be noticeable at any period a device will hand over.
  constexpr int kSamples = 32;
  dispatch_semaphore_t arrived = dispatch_semaphore_create(0);
  dispatch_queue_t queue = dispatch_queue_create("cc.hemsley.namchain.coreaudio.probe", DISPATCH_QUEUE_SERIAL);
  if (queue == nullptr)
  {
    dispatch_release(arrived);
    error = "could not create the CoreAudio probe queue";
    return false;
  }

  // A block captures by copy, and an atomic is not copyable, so what the callback writes lives
  // behind a shared_ptr: only the callback touches it, and only this thread reads it after the wait.
  struct Probe
  {
    std::atomic<unsigned> smallest{0};
    std::atomic<int> seen{0};
  };
  std::shared_ptr<Probe> probe_state = std::make_shared<Probe>();

  AudioDeviceIOProcID probe = nullptr;
  const OSStatus created = AudioDeviceCreateIOProcIDWithBlock(
    &probe, device, queue,
    ^(const AudioTimeStamp* /*now*/, const AudioBufferList* /*input*/, const AudioTimeStamp* /*input_time*/,
      AudioBufferList* output, const AudioTimeStamp* /*output_time*/) {
      if (output == nullptr || output->mNumberBuffers == 0)
        return;
      // Bytes for every channel of this buffer: 4096 bytes at 2 channels of float is 1024 samples
      // and **512 frames**, not 1024. Dividing by the sample size alone was a real mistake made here,
      // and it doubled every figure derived from it -- including a rate check that then refused a
      // perfectly good device.
      const unsigned channels = output->mBuffers[0].mNumberChannels;
      const unsigned got = static_cast<unsigned>(output->mBuffers[0].mDataByteSize /
                                                 (sizeof(float) * (channels == 0 ? 1 : channels)));
      if (got == 0)
        return;
      unsigned current = probe_state->smallest.load(std::memory_order_relaxed);
      while ((current == 0 || got < current) &&
             !probe_state->smallest.compare_exchange_weak(current, got, std::memory_order_relaxed))
      {
      }
      if (probe_state->seen.fetch_add(1, std::memory_order_relaxed) + 1 >= kSamples)
        dispatch_semaphore_signal(arrived);
    });
  if (!ca_ok(created))
  {
    dispatch_release(queue);
    dispatch_release(arrived);
    error = "AudioDeviceCreateIOProcIDWithBlock (probe): " + os_status_text(created);
    return false;
  }

  const OSStatus started = AudioDeviceStart(device, probe);
  bool got = false;
  if (ca_ok(started))
    got = dispatch_semaphore_wait(arrived, dispatch_time(DISPATCH_TIME_NOW, 1000 * NSEC_PER_MSEC)) == 0;
  else
    error = "AudioDeviceStart (probe): " + os_status_text(started);

  if (ca_ok(started))
    AudioDeviceStop(device, probe);
  AudioDeviceDestroyIOProcID(device, probe);
  dispatch_release(queue);
  dispatch_release(arrived);

  if (!got && probe_state->seen.load() == 0)
  {
    if (error.empty())
      error = "the CoreAudio device produced no callback in 1000 ms, so its frame count could not be measured";
    return false;
  }
  // A partial run is still a measurement: the smallest callback seen is what the plan has to carry.
  frames = probe_state->smallest.load();
  if (frames == 0)
  {
    error = "the CoreAudio device reported a zero-frame period";
    return false;
  }

  return true;
}

} // namespace

#if defined(NAMCHAIN_COREAUDIO_CAPTURE)
/// \brief Records what the callback hands the device, for a null test of the live path.
///
/// Deliberately a `FILE*` and not `save_wav_float32`: the write happens on the device's thread, and
/// the point of the exercise is to record the stream *as it is delivered* rather than to buffer a
/// session in memory. The WAV header is written at close, when the frame count is known. It is not
/// real-time safe — it can block on a slow disk — which is the whole reason it is behind a build
/// option and not a config key in an ordinary build.
struct DeviceRecorder
{
  FILE* file = nullptr;
  std::string path;
  uint64_t frames = 0;
  unsigned channels = 0;
  unsigned written = 0;       ///< frames since the last flush
  bool header_written = false;

  bool open(const std::string& where, unsigned channel_count)
  {
    if (where.empty())
      return true;             // nothing asked for, nothing to do
    file = std::fopen(where.c_str(), "wb+");
    if (file == nullptr)
      return false;
    path = where;
    channels = channel_count == 0 ? 1 : channel_count;
    // 44 bytes of header now, so the audio lands at a known offset and can be rewritten at close.
    std::fwrite(std::vector<unsigned char>(44, 0).data(), 1, 44, file);
    return true;
  }

  /// The callback's planar period, written interleaved so the result is a WAV any tool can read.
  void write(const float* const* planar, unsigned channel_count, unsigned period, unsigned sample_rate)
  {
    if (file == nullptr)
      return;
    std::vector<float> interleaved(static_cast<std::size_t>(period) * channels, 0.0f);
    for (unsigned c = 0; c < channels; ++c)
    {
      const float* source = c < channel_count ? planar[c] : planar[channel_count - 1];
      if (source == nullptr)
        continue;
      for (unsigned i = 0; i < period; ++i)
        interleaved[static_cast<std::size_t>(i) * channels + c] = source[i];
    }
    std::fwrite(interleaved.data(), sizeof(float), interleaved.size(), file);
    frames += period;
    // Flushed often enough that a kill -9 still leaves most of the recording, rarely enough not to
    // turn every callback into a disk write.
    if (++written >= 32)
    {
      std::fflush(file);
      written = 0;
    }
    sample_rate_ = sample_rate;
  }

  void close()
  {
    if (file == nullptr)
      return;
    std::fflush(file);
    write_header();
    std::fclose(file);
    file = nullptr;
  }

private:
  unsigned sample_rate_ = 48000;

  void put32(unsigned value)
  {
    const unsigned char bytes[4] = {static_cast<unsigned char>(value & 0xff),
                                    static_cast<unsigned char>((value >> 8) & 0xff),
                                    static_cast<unsigned char>((value >> 16) & 0xff),
                                    static_cast<unsigned char>((value >> 24) & 0xff)};
    std::fwrite(bytes, 1, 4, file);
  }
  void put16(unsigned value)
  {
    const unsigned char bytes[2] = {static_cast<unsigned char>(value & 0xff),
                                    static_cast<unsigned char>((value >> 8) & 0xff)};
    std::fwrite(bytes, 1, 2, file);
  }

  void write_header()
  {
    if (file == nullptr || header_written)
      return;
    header_written = true;
    const unsigned data_bytes = static_cast<unsigned>(frames * channels * sizeof(float));
    std::fseek(file, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, file);
    put32(36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, file);
    put32(16);
    put16(3);                         // IEEE float
    put16(channels);
    put32(sample_rate_);
    put32(sample_rate_ * channels * sizeof(float));
    put16(channels * sizeof(float));
    put16(32);
    std::fwrite("data", 1, 4, file);
    put32(data_bytes);
  }
};
#endif

struct CoreAudioBackend::Implementation
{
  // --- configuration, fixed at open time ------------------------------------------------------
  CoreAudioConfig config;
  BackendInfo info;
  LoopSource loop;

  /// The stream's device: the one the output goes to, or the private aggregate when the input and
  /// the output are different devices. The IOProc and the unplug listener belong to it.
  AudioObjectID device = kAudioObjectUnknown;
  /// The private aggregate this backend made, and must destroy. kAudioObjectUnknown when the input
  /// and output are one device -- the common case, and the one that needs no HAL object at all.
  AudioObjectID aggregate = kAudioObjectUnknown;
  /// \brief Whether the capture comes from a device rather than from `loop`.
  ///
  /// The one branch in the callback that matters: it decides whether the capture side reads the
  /// device's own buffers or the file. It is fixed at open time and never changes for the life of the
  /// backend.
  bool live_input = false;
  AudioDeviceIOProcID io_proc = nullptr;
  dispatch_queue_t queue = nullptr;
  bool started = false;

  /// The two devices as the operator named them, for `info` and for the error messages: the
  /// aggregate's own name ("namchain (private)") names neither of them.
  std::string input_device_name;
  std::string output_device_name;

  /// The device's buffer as it was found, and whether this open changed it. `stop()` puts it back, so
  /// borrowing somebody's interface at 32 frames does not leave their DAW configured that way.
  unsigned previous_buffer_frames = 0;
  bool buffer_frames_set = false;
  /// What the device's buffer actually is after the request, for `diagnostics`.
  unsigned device_buffer_frames = 0;

  /// One captured period, preallocated. Filled by the IOProc and pushed to the ring as a whole.
  std::vector<float> capture_scratch;
  /// One period of silence, written when the output ring is empty.
  std::vector<float> silence;
  /// try_write_playback's decode target, preallocated: the engine's period laid out channel-major,
  /// which is the ring slot's layout and what the IOProc reads. It must not be allocated per period,
  /// because try_write_playback runs on the engine's audio thread.
  std::vector<float> playback_planar;
  /// Periods of processed audio waiting for the device, in device channel order.
  std::unique_ptr<SpscRing<std::vector<float>>> playback;
  /// Periods of captured audio waiting for the engine.
  std::unique_ptr<SpscRing<std::vector<float>>> capture;
  unsigned playback_channels = 0;

  /// The most periods one callback can carry. A device asking for more is refused rather than
  /// overrunning this array -- see the guard in `io`. Declared before the arrays it sizes.
  static constexpr unsigned kMaxPeriodsPerCallback = 16;
  /// The most capture channels this backend carries. A device with more is asked for the first this
  /// many; the engine's own bus is far narrower (a document is at most eight chains).
  static constexpr unsigned kMaxCaptureChannels = 32;

  /// \brief Where one of the device's input channels is in the callback's buffer list.
  ///
  /// A device may present one interleaved buffer of N channels, N buffers of one channel, or -- an
  /// interface with two stereo pairs -- two buffers of two. Flattening that here, once per callback,
  /// is what lets the copy below index "capture channel c" without caring which of the three the
  /// device chose.
  struct ChannelSource
  {
    const float* data = nullptr;
    unsigned stride = 1;  ///< Floats between consecutive frames of this channel.
    unsigned offset = 0;  ///< Floats from the start of a frame to this channel.
  };
  ChannelSource capture_source[kMaxCaptureChannels] = {};
  /// The IOProc's own copy targets, one per period a callback can carry, so the callback allocates
  /// nothing. **One per period rather than one shared**, because a callback carrying two periods pops
  /// two and the fill below writes the whole device buffer from them: a single vector left the first
  /// half of every device buffer holding the second period's audio. That was a real defect here --
  /// this machine's probe sees 512 frames while the stream's own IOProc is handed 1024, so every
  /// other half-buffer was a repeat of the one before it.
  std::vector<float> period_scratch[kMaxPeriodsPerCallback];
  /// Whether each period of the current callback came from the ring. Sized by the constant above.
  bool playback_fresh[kMaxPeriodsPerCallback] = {};
#if defined(NAMCHAIN_COREAUDIO_CAPTURE)
  /// Scratch for de-interleaving into the recorder, so the recorder needs no allocation on this
  /// thread. Most devices are stereo; the array is generous and the loop is bounded by it.
  std::vector<float> recorder_slice[32];
  const float* recorder_planar[32] = {nullptr};
#endif

  /// Signalled by the IOProc after a capture period is pushed, waited on by `wait_capture`. This is
  /// what makes the engine's wait a block rather than a poll: the alternative measured 70 % CPU for
  /// a passthrough rig, because a poll loop spins for most of a device period.
  dispatch_semaphore_t capture_ready = nullptr;
  /// The `kAudioDevicePropertyDeviceIsAlive` listener registration, kept so it can be removed before
  /// the device is forgotten. The C proc form rather than the block form: the proc takes the object
  /// as client data, so there is no block for the HAL to copy, retain and release, and nothing to
  /// free twice.
  AudioObjectPropertyAddress alive_address = global_address(kAudioDevicePropertyDeviceIsAlive);
  bool alive_listener_registered = false;
  /// Set by `stop()` to cut a `wait_capture` short. Without it the daemon's shutdown waits out the
  /// render loop's device timeout, because the loop only re-checks whether it is running *after*
  /// wait_capture returns -- which for a device with a 21 ms period is most of a second of delay on
  /// every Ctrl-C.
  std::atomic<bool> stopping{false};

  ~Implementation()
  {
    if (capture_ready != nullptr)
      dispatch_release(capture_ready);
  }

  /// The cap actually enforced, in periods; 0 when the ring's capacity is the only cap. Copied into
  /// `info.output_queue_periods` so the reported number and the enforced one cannot disagree.
  unsigned queue_periods = 0;

  /// The highest channel the engine has actually written, on the render thread. The callback reads it
  /// to decide which channel to duplicate into a device that wants more channels than the document
  /// uses -- see `try_write_playback`.
  std::atomic<unsigned> engine_last_channel{0};


  // --- cross-thread state ---------------------------------------------------------------------
  std::atomic<bool> lost{false};
  char lost_reason[256] = {0};
  std::atomic<uint64_t> playback_underruns{0};
  std::atomic<uint64_t> capture_overruns{0};
  /// The most periods one callback has carried, and the most a period has been late by. Both are for
  /// `diagnostics`: "the engine is being starved" and "the device is handing over more than one
  /// quantum at a time" are the same symptom from outside and different faults inside, and a
  /// recording of the sound cannot tell them apart.
  std::atomic<unsigned> max_periods_per_callback{0};

  void mark_lost(const std::string& reason)
  {
    // Written by whichever thread found the device gone, read by the engine thread after the acquire
    // load of `lost`. The buffer is never rewritten, so a reader either sees the whole message or
    // (before the release store) not the flag at all.
    std::snprintf(lost_reason, sizeof(lost_reason), "%s", reason.c_str());
    lost.store(true, std::memory_order_release);
  }

  /// The same, for a caller on the device's own thread, which may not allocate: the message is built
  /// straight into the buffer. Kept separate from the overload above rather than merged, because the
  /// difference is the whole reason it exists -- building a `std::string` here would be an allocation
  /// on a real-time thread, and the one path that calls it is the one nobody exercises by hand.
  void mark_lost_rt(const char* fate, unsigned got, unsigned expected)
  {
    std::snprintf(lost_reason, sizeof(lost_reason), "the CoreAudio device %s %u frames, not the compiled period of %u",
                  fate, got, expected);
    lost.store(true, std::memory_order_release);
  }

  /// The one-callback-too-big refusal, which is not the same failure as a callback that is not a
  /// whole number of periods and must not be reported as if it were.
  void mark_lost_periods(unsigned frames, unsigned period)
  {
    std::snprintf(lost_reason, sizeof(lost_reason),
                  "the CoreAudio device handed over %u frames at once, which is more than the %u periods of %u "
                  "this backend carries",
                  frames, kMaxPeriodsPerCallback, period);
    lost.store(true, std::memory_order_release);
  }

  /// The input direction's version of the same two failures. Kept apart from the output's because the
  /// fix for each is different and a message that does not say which side failed sends the reader to
  /// the wrong device.
  void mark_lost_input_frames(unsigned got, unsigned expected)
  {
    std::snprintf(lost_reason, sizeof(lost_reason),
                  "the CoreAudio input device handed over %u frames, and the engine's period is %u",
                  got, expected);
    lost.store(true, std::memory_order_release);
  }

  /// No input buffers at all: the IOProc was created without the input streams, or the device has
  /// stopped presenting them. **A refused microphone is not this** -- macOS hands silence rather than
  /// nothing, so a Mac that has not granted permission records silence and this message would be a
  /// lie; docs/daemon.md says what the spawner has to arrange.
  void mark_lost_no_input()
  {
    std::snprintf(lost_reason, sizeof(lost_reason),
                  "the CoreAudio device handed the callback no input buffers, so there is nothing to capture");
    lost.store(true, std::memory_order_release);
  }

  /// The device going away is the one failure that cannot be noticed by looking at a buffer: the
  /// HAL simply stops calling, and the engine would sit in `wait_capture` until its timeout and then
  /// call the device lost anyway. This listener makes the state visible immediately, and it is what
  /// `engine.status`'s `device_lost` counter is fed by.
  void mark_unplugged() { mark_lost("the CoreAudio device was removed"); }

  bool lost_error(std::string& error) const
  {
    if (!lost.load(std::memory_order_acquire))
      return false;
    error = lost_reason;
    return true;
  }

  /// \brief The one callback. Real-time: no allocation, no lock, no logging, no file I/O -- the loop
  /// is in memory.
  ///
  /// It handles **whatever frame count the device asks for**, which is not always the period the
  /// engine is running at. On the machine this was written on the probe sees 512 frames per callback
  /// and the stream's own IOProc is then handed 1024: a device is free to present a larger IO buffer
  /// than the engine's quantum. The callback therefore converts frames into *periods*: one callback
  /// carrying two periods' worth of frames hands the engine two periods, and asks it for two back.
  /// Both directions are quantised to the engine's period, and over a second the frame count the
  /// device consumes and the frame count the engine produces are the same number -- which is the only
  /// property that matters, and the one a frame-count equality check here was getting wrong.
  void io(const AudioTimeStamp* /*now*/, const AudioBufferList* input, const AudioTimeStamp* /*input_time*/,
          AudioBufferList* output, const AudioTimeStamp* /*output_time*/)
  {
    const unsigned channels = output->mNumberBuffers > 0 ? output->mBuffers[0].mNumberChannels : 0;
    const unsigned bytes = output->mNumberBuffers > 0 ? output->mBuffers[0].mDataByteSize : 0;
    const unsigned frames = bytes / (sizeof(float) * (channels == 0 ? 1 : channels));
    const unsigned period = info.period_size;

    if (frames == 0 || period == 0 || (frames % period) != 0)
    {
      // A callback that is not a whole number of periods cannot be divided without inventing an
      // offset. Silence it, record why once, and let the engine's next call report it.
      zero_output(output);
      mark_lost_rt("asked for", frames, period);
      return;
    }

    // How many engine periods this one callback carries. A device is free to present a larger IO
    // buffer than the engine's quantum: the callback hands the engine one period per period's worth
    // of frames, and asks for the same number back.
    const unsigned periods = frames / period;
    if (periods > kMaxPeriodsPerCallback)
    {
      zero_output(output);
      mark_lost_periods(frames, period);
      return;
    }
    // What the device is actually asking for, in engine periods. Recorded rather than assumed: the
    // probe's smallest callback is what the engine runs at, and a *stream* callback may be larger --
    // on this machine the probe sees 512 frames and the stream is handed 1024, which is two periods.
    if (periods > max_periods_per_callback.load(std::memory_order_relaxed))
      max_periods_per_callback.store(periods, std::memory_order_relaxed);
    // One pop per period, before the fill: the fill above must not branch on the ring, and a period
    // that did not arrive is a period of silence and one underrun. Each period is popped into its own
    // scratch, because the fill below needs all of them, not just the last.
    for (unsigned p = 0; p < periods; ++p)
    {
      playback_fresh[p] = playback->pop(period_scratch[p]);
      if (!playback_fresh[p])
        playback_underruns.fetch_add(1, std::memory_order_relaxed);
    }

    // **Frames and channels are tracked separately, and every index is a frame index.** This loop
    // used to advance by `period` floats per period, which is right only when a period is one
    // channel wide. On an interleaved stereo buffer it writes half a period and leaves the rest of
    // the buffer holding whatever was there before -- and the device plays that. It was reported as
    // an octave-up tone gated at the device period, which is exactly what half-a-buffer of fresh
    // audio followed by half a buffer of stale audio sounds like.
    // **The layout the device presents decides the indexing**, and this device presents one
    // interleaved buffer holding both channels: `mNumberBuffers == 1`, `mNumberChannels == 2`, so a
    // frame occupies two consecutive floats and channel `c` of frame `f` is `mData[f * channels +
    // c]`. Writing it as though each channel had its own buffer -- `mData[f]` per channel -- is the
    // bug that made this sound broken: every sample went to a position one channel-stride away from
    // where the device reads it, so the left got even samples, the right got odd ones, the pitch
    // doubled, and every other period was never written at all.
    //
    // Both layouts are handled explicitly rather than assumed, because "which one is it" is a
    // property of the device and not of this code.
    // The device's channel count, and the engine's, are different numbers and both are needed:
    // the ring holds `playback_channels` slices of `period` frames, and the device wants
    // `device_channels` interleaved. Conflating them is what made the right channel silent.
    const unsigned device_channels = channels == 0 ? 1 : channels;
    const unsigned engine_channels = std::max(1u, playback_channels);
    const bool interleaved = output->mNumberBuffers == 1 && device_channels > 1;

    for (unsigned f = 0; f < frames; ++f)
    {
      const unsigned period_index = f / period;          // which period of this callback
      const unsigned frame_in_period = f % period;
      // A period that did not arrive is **silence, written rather than skipped**: the HAL hands the
      // same buffer back to the next callback, so leaving it alone plays whatever was in it last time
      // round. The comment here used to say the frame was "left zeroed" while nothing zeroed it.
      const bool fresh = playback_fresh[period_index];
      const std::vector<float>& source = period_scratch[period_index];

      // The channel loop must run once per **device channel**, not once per buffer: an interleaved
      // device has one buffer holding both channels, so `b < mNumberBuffers` visits one channel and
      // writes every sample at a stride of one -- which is how the source's sample k ended up at
      // index 2k, silent right channel, doubled pitch, and one-sample L/R offset.
      const unsigned written = interleaved ? device_channels : output->mNumberBuffers;
      for (unsigned c = 0; c < written; ++c)
      {
        // The engine writes `playback_channels` channels; a device with more gets the last one
        // repeated, which is how a mono engine feeds a stereo device.
        // Device channel `c` reads engine channel `c` when the document has one; past the last
        // channel the document writes, it repeats that channel rather than emitting silence.
        const unsigned last = std::min(engine_last_channel.load(std::memory_order_relaxed), engine_channels - 1);
        const unsigned source_channel = std::min(c, last);
        const float value =
          fresh ? source[static_cast<std::size_t>(source_channel) * period + frame_in_period] : 0.0f;

        const UInt32 buffer = interleaved ? 0u : c;
        if (buffer >= output->mNumberBuffers)
          break;
        float* destination = static_cast<float*>(output->mBuffers[buffer].mData);
        if (destination == nullptr)
          continue;
        const std::size_t index = interleaved ? static_cast<std::size_t>(f) * device_channels + c : f;
        destination[index] = value;
      }
    }

#if defined(NAMCHAIN_COREAUDIO_CAPTURE)
    // What the device is being sent, for a null test of this path -- taken from the buffer that is
    // handed to it rather than from the ring, so what is recorded is what the device receives.
    if (recorder.file != nullptr)
    {
      // **The recorder must read the buffer with the same layout the callback wrote it in.** It takes
      // planar slices, and this device is interleaved: handing it `mBuffers[0]` as though it were one
      // channel makes it read both channels as consecutive frames, which duplicates every sample --
      // a 1 kHz tone recorded as 500 Hz with every value repeated, and a measurement that accuses the
      // engine of a bug that is entirely in the instrument. De-interleave here.
      if (interleaved)
      {
        for (unsigned c = 0; c < device_channels && c < 32; ++c)
        {
          float* slice = recorder_slice[c].data();
          const float* source = static_cast<const float*>(output->mBuffers[0].mData);
          for (unsigned f = 0; f < frames; ++f)
            slice[f] = source[static_cast<std::size_t>(f) * device_channels + c];
          recorder_planar[c] = slice;
        }
        recorder.write(recorder_planar, device_channels, frames, info.sample_rate);
      }
      else
      {
        const float* planar[32] = {nullptr};
        const unsigned count = std::min<unsigned>(output->mNumberBuffers, 32);
        for (unsigned b = 0; b < count; ++b)
          planar[b] = static_cast<const float*>(output->mBuffers[b].mData);
        recorder.write(planar, count, frames, info.sample_rate);
      }
    }
#endif
    // And the same number of periods back in. Each one comes from the device when the capture is
    // live and from the loop when it is not, and each is pushed as a whole, so the engine's
    // `read_capture` finds exactly what it was credited for.
    if (live_input)
    {
      capture_from_input(input, frames, periods);
    }
    else
    {
      for (unsigned p = 0; p < periods; ++p)
      {
        loop.read(capture_scratch.data(), period);
        if (capture->push(capture_scratch))
        {
          capture_credits.fetch_add(1, std::memory_order_release);
        }
        else
        {
          // The engine had not read the last two periods yet. Dropping is the only real-time-safe
          // answer, and `capture_overruns` is the honest name for it -- it is the engine being late,
          // not the device.
          capture_overruns.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    // One wake per callback, not one per period: the engine spends credits itself, and a second
    // signal would leave a token in the semaphore with nothing behind it. Signalled even when the
    // capture side has just marked the device lost, so the engine sees that now rather than after
    // its own timeout.
    dispatch_semaphore_signal(capture_ready);
  }

  /// \brief One callback's worth of a live input device's capture, into the ring.
  ///
  /// The device's *input* layout is decided the same way its output layout is, and for the same
  /// reason: a device may present one interleaved buffer holding every channel, or one buffer per
  /// channel, and assuming the wrong one reads every sample twice. That mistake was made here once --
  /// a 1 kHz tone came out as 500 Hz with every value repeated -- and it was in the measurement, not
  /// in the engine.
  void capture_from_input(const AudioBufferList* input, unsigned frames, unsigned periods)
  {
    if (input == nullptr || input->mNumberBuffers == 0)
    {
      mark_lost_no_input();
      return;
    }

    // The engine's period and the device's callback are the same length here -- one IOProc is handed
    // one list covering both directions, on one device or on an aggregate -- but a short input buffer
    // would be read past its end below, so it is checked rather than assumed.
    const UInt32 first_channels = input->mBuffers[0].mNumberChannels;
    const unsigned input_frames = static_cast<unsigned>(
      input->mBuffers[0].mDataByteSize / (sizeof(float) * (first_channels == 0 ? 1 : first_channels)));
    if (input_frames < frames)
    {
      mark_lost_input_frames(input_frames, frames);
      return;
    }

    // Flatten the list once: engine channel `c` is `data[frame * stride + offset]`, whatever the
    // device's buffer layout is.
    const unsigned capture_channels = info.capture_channels;
    unsigned flat = 0;
    for (UInt32 b = 0; b < input->mNumberBuffers && flat < capture_channels; ++b)
    {
      const unsigned in_buffer = std::max(1u, input->mBuffers[b].mNumberChannels);
      const float* data = static_cast<const float*>(input->mBuffers[b].mData);
      for (unsigned k = 0; k < in_buffer && flat < capture_channels; ++k, ++flat)
      {
        capture_source[flat].data = data;
        capture_source[flat].stride = in_buffer;
        capture_source[flat].offset = k;
      }
    }
    // Fewer channels in the list than the engine's bus. `create` checked the device's *stream
    // configuration* against the bus width, so this is the callback disagreeing with the property:
    // silence for the rest rather than reading past the list.
    for (unsigned c = flat; c < capture_channels; ++c)
      capture_source[c] = ChannelSource{};

    for (unsigned p = 0; p < periods; ++p)
    {
      for (unsigned i = 0; i < info.period_size; ++i)
      {
        const unsigned frame = p * info.period_size + i;
        for (unsigned c = 0; c < capture_channels; ++c)
        {
          const ChannelSource& source = capture_source[c];
          const float value =
            source.data == nullptr ? 0.0f : source.data[static_cast<std::size_t>(frame) * source.stride + source.offset];
          capture_scratch[static_cast<std::size_t>(i) * capture_channels + c] = value;
        }
      }
      if (capture->push(capture_scratch))
      {
        capture_credits.fetch_add(1, std::memory_order_release);
      }
      else
      {
        // The same answer as the loop case: the engine had not read the last two periods yet.
        capture_overruns.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

#if defined(NAMCHAIN_COREAUDIO_CAPTURE)
  /// What the device was sent, when something asked for it. Empty (and unused) otherwise.
  DeviceRecorder recorder;
#endif

  /// Periods the device has handed over that the engine has not read yet. The engine's loop is one
  /// iteration per period, so this is what makes a callback carrying two periods produce two.
  std::atomic<unsigned> capture_credits{0};
};

/// The `kAudioDevicePropertyDeviceIsAlive` listener. A free function so the HAL can be handed a plain
/// function pointer with the `Implementation` as client data -- the block form has the HAL copy,
/// retain and release the block, and getting that wrong is a crash on the way out rather than on the
/// way in.
OSStatus device_alive_listener(AudioObjectID /*object*/, UInt32 /*count*/,
                               const AudioObjectPropertyAddress* /*addresses*/, void* client)
{
  if (client != nullptr)
    static_cast<CoreAudioBackend::Implementation*>(client)->mark_unplugged();
  return noErr;
}

CoreAudioBackend::CoreAudioBackend() : self_(new Implementation()) {}

CoreAudioBackend::~CoreAudioBackend() { stop(); }

const BackendInfo& CoreAudioBackend::info() const { return self_->info; }

std::unique_ptr<AudioBackend> CoreAudioBackend::create(const BackendConfig& config, std::string& error)
{
  // 0. Where the capture comes from. A device to capture from and a WAV to loop are alternatives
  //    rather than layers, so a configuration that gives both -- or neither -- is refused by name
  //    rather than one of them quietly winning. Neither would be an engine running on silence for
  //    ever, which looks like a working device.
  const bool live_input = !config.input_device.empty();
  if (live_input && !config.input_path.empty())
  {
    error = "the coreaudio backend takes --input-device or --capture-path, not both: a CoreAudio input "
            "device to capture from and a WAV to loop are alternatives";
    return nullptr;
  }
  if (!live_input && config.input_path.empty())
  {
    error = "the coreaudio backend needs --input-device (a CoreAudio input device UID to capture from) "
            "or --capture-path (a WAV to loop as its input)";
    return nullptr;
  }
  if (live_input && config.capture_channels > Implementation::kMaxCaptureChannels)
  {
    error = "the coreaudio backend carries at most " + std::to_string(Implementation::kMaxCaptureChannels) +
            " capture channels, configured for " + std::to_string(config.capture_channels);
    return nullptr;
  }

  std::unique_ptr<CoreAudioBackend> self(new CoreAudioBackend());
  Implementation& impl = *self->self_;

  impl.config.input_path = config.input_path;
  impl.config.input_device_uid = config.input_device;
  impl.config.device_uid = config.device;
  impl.config.sample_rate = config.sample_rate;
  impl.config.period_size = config.period_size;
  impl.config.periods = config.periods == 0 ? 4 : config.periods;
  impl.config.capture_channels = config.capture_channels;
  impl.config.device_buffer_frames = config.device_buffer_frames;
  impl.config.playback_channels = config.playback_channels;
  impl.config.output_queue_periods = config.output_queue_periods;
  impl.live_input = live_input;

  // 1. The signal, when the signal is a file. Loaded whole, before anything is opened: the IOProc
  //    reads it on the device's thread, and a file read there would be a dropout.
  if (!live_input &&
      !LoopSource::load(impl.config.input_path, impl.config.sample_rate, impl.loop, error))
    return nullptr;

  // 2. The output device: the operator's UID, or the system's default. A UID rather than an index,
  //    because indices move when something is unplugged.
  AudioObjectID output = kAudioObjectUnknown;
  if (impl.config.device_uid.empty())
  {
    output = default_device(kAudioHardwarePropertyDefaultOutputDevice, "output", error);
    if (output == kAudioObjectUnknown)
      return nullptr;
  }
  else
  {
    output = device_for_uid(impl.config.device_uid, error);
    if (output == kAudioObjectUnknown)
      return nullptr;
  }
  impl.output_device_name = device_name(output);

  // 2b. The input device, when the capture is live. There is deliberately no "default input": reading
  //     a microphone is a decision, so it is only ever made because a UID was given.
  AudioObjectID input = kAudioObjectUnknown;
  if (live_input)
  {
    input = device_for_uid(impl.config.input_device_uid, error);
    if (input == kAudioObjectUnknown)
      return nullptr;
    impl.input_device_name = device_name(input);

    // Checked here, before any aggregate is made, so that a device with no inputs at all is refused
    // by the name the operator used rather than as an aggregate with a silent input scope.
    const unsigned available = input_channel_count(input);
    if (available == 0)
    {
      error = "the CoreAudio input device '" + impl.config.input_device_uid + "' has no input channels";
      return nullptr;
    }
    if (available < impl.config.capture_channels)
    {
      error = "the CoreAudio input device '" + impl.config.input_device_uid + "' has " +
              std::to_string(available) + " input channels, configured for " +
              std::to_string(impl.config.capture_channels);
      return nullptr;
    }
  }

  // 2c. One stream device. The same device serves both directions when it has both; two *different*
  //     devices need an aggregate, because they have two clocks and this engine has no resampler --
  //     see make_aggregate for why the aggregate is the answer rather than two IOProcs. The aggregate
  //     is the stream device from here on, and this object owns it until stop().
  if (live_input && input != output)
  {
    impl.aggregate = make_aggregate(output, input, error);
    if (impl.aggregate == kAudioObjectUnknown)
      return nullptr;
    impl.device = impl.aggregate;
  }
  else
  {
    impl.device = output;
  }

  // 3. It has to be there now. A device that is alive at open and dies later is the daemon's lost
  //    device path; a device that was never there is a configuration error. The name used is the
  //    operator's -- the output device's, or the aggregate's when there is one -- because that is
  //    what they can act on.
  UInt32 alive = 0;
  if (!get_property(impl.device, global_address(kAudioDevicePropertyDeviceIsAlive), alive))
  {
    error = "the CoreAudio device '" + impl.output_device_name + "' has no IsAlive property";
    return nullptr;
  }
  if (alive == 0)
  {
    error = "the CoreAudio device '" + impl.output_device_name + "' is not alive";
    return nullptr;
  }

  // 3b. The device's own buffer, when the caller asked for one. This is the first thing that should
  //     happen to the device, because everything after it -- the format checks, and above all the
  //     probe -- is about the stream this buffer defines. A request is checked against the device's
  //     own range and read back; the read-back is what `diagnostics` reports, because a device that
  //     silently clamps to 512 must not be described as running at 32.
  if (impl.config.device_buffer_frames > 0)
  {
    AudioValueRange range{};
    if (get_property(impl.device, global_address(kAudioDevicePropertyBufferFrameSizeRange), range) &&
        (impl.config.device_buffer_frames < static_cast<unsigned>(range.mMinimum) ||
         impl.config.device_buffer_frames > static_cast<unsigned>(range.mMaximum)))
    {
      error = "the CoreAudio device takes a buffer of " + std::to_string(static_cast<unsigned>(range.mMinimum)) +
              " to " + std::to_string(static_cast<unsigned>(range.mMaximum)) + " frames, and " +
              std::to_string(impl.config.device_buffer_frames) + " was asked for";
      return nullptr;
    }
    if (get_property(impl.device, global_address(kAudioDevicePropertyBufferFrameSize), impl.previous_buffer_frames))
      impl.buffer_frames_set = true;
    const UInt32 wanted = impl.config.device_buffer_frames;
    if (!set_property(impl.device, global_address(kAudioDevicePropertyBufferFrameSize), wanted))
    {
      error = "the CoreAudio device would not take a buffer of " + std::to_string(wanted) + " frames";
      return nullptr;
    }
    UInt32 actual = 0;
    if (get_property(impl.device, global_address(kAudioDevicePropertyBufferFrameSize), actual))
      impl.device_buffer_frames = actual;
  }

  // 4. The rate. The engine has no resampler, so a device at another rate is refused rather than
  //    asked to change: changing it would be the app editing a system-wide setting. On an aggregate
  //    the master's rate is the aggregate's, and an input at a different rate would need the HAL to
  //    convert it -- which is a thing aggregates do, but only within what the two devices share, so
  //    both are checked before the aggregate is allowed near them.
  Float64 nominal_rate = 0.0;
  if (!get_property(impl.device, global_address(kAudioDevicePropertyNominalSampleRate), nominal_rate))
  {
    error = "the CoreAudio device's nominal sample rate could not be read";
    return nullptr;
  }
  if (static_cast<unsigned>(nominal_rate + 0.5) != impl.config.sample_rate)
  {
    error = "the CoreAudio device runs at " + std::to_string(static_cast<long long>(nominal_rate + 0.5)) +
            " Hz but the engine runs at " + std::to_string(impl.config.sample_rate) +
            " Hz; set it in Audio MIDI Setup";
    return nullptr;
  }

  // 5. The formats: float32, native endian, the device's own channel count, read per *scope*. A
  //    device that presents something else is refused rather than converted -- this backend has no
  //    format conversion, and a silent one would be worse than a refusal.
  AudioStreamBasicDescription format{};
  if (!get_property(impl.device, scoped_address(kAudioDevicePropertyStreamFormat, kAudioObjectPropertyScopeOutput),
                    format))
  {
    error = "the CoreAudio device's playback stream format could not be read";
    return nullptr;
  }
  const bool is_float = (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0 &&
                        (format.mFormatFlags & kAudioFormatFlagIsBigEndian) == 0;
  if (format.mFormatID != kAudioFormatLinearPCM || !is_float || format.mBitsPerChannel != 32)
  {
    error = "the CoreAudio device does not present 32-bit float linear PCM for playback, so this backend "
            "cannot drive it";
    return nullptr;
  }
  if (format.mChannelsPerFrame < impl.config.playback_channels)
  {
    error = "the CoreAudio device has " + std::to_string(format.mChannelsPerFrame) +
            " playback channels, configured for " + std::to_string(impl.config.playback_channels);
    return nullptr;
  }
  impl.playback_channels = format.mChannelsPerFrame;

  // 5b. The same for the capture, when it is live. The input scope's format is the one the callback's
  //     input buffers will be in, and the engine's capture bus is as wide as `--capture-channels`
  //     says. The channel count comes from the stream configuration rather than the format, because a
  //     device may present several streams and the format describes only one of them.
  unsigned capture_channels = impl.loop.channels();
  if (live_input)
  {
    AudioStreamBasicDescription input_format{};
    if (!get_property(impl.device,
                      scoped_address(kAudioDevicePropertyStreamFormat, kAudioObjectPropertyScopeInput),
                      input_format))
    {
      error = "the CoreAudio input device's stream format could not be read";
      return nullptr;
    }
    const bool input_is_float = (input_format.mFormatFlags & kAudioFormatFlagIsFloat) != 0 &&
                                (input_format.mFormatFlags & kAudioFormatFlagIsBigEndian) == 0;
    if (input_format.mFormatID != kAudioFormatLinearPCM || !input_is_float || input_format.mBitsPerChannel != 32)
    {
      error = "the CoreAudio input device does not present 32-bit float linear PCM, so this backend cannot "
              "capture from it";
      return nullptr;
    }
    capture_channels = impl.config.capture_channels;
  }

  // 6. The quantum, which is the device's to choose. The engine compiles its plan for one period
  //    size (§9.2), so what matters is the number the callback is actually handed -- and asking the
  //    device for a different one is a request the HAL may not honour (see measure_device).
  //    The configured `--period-size` is therefore a *ceiling* here: a plan compiled for it carries
  //    any smaller period, while a device that wants more than the plan was built for is refused,
  //    because there is no way to assemble a period the engine has no buffers for.
  unsigned device_frames = 0;
  if (!measure_device(impl.device, device_frames, error))
    return nullptr;
  if (device_frames > impl.config.period_size)
  {
    error = "the CoreAudio device delivers " + std::to_string(device_frames) +
            " frames per callback, and the plan is compiled for " + std::to_string(impl.config.period_size) +
            "; restart with --period-size " + std::to_string(device_frames);
    return nullptr;
  }

#if defined(NAMCHAIN_COREAUDIO_CAPTURE)
  // A measurement instrument, asked for by name. When this build has it, it opens here -- before the
  // device runs, so a run that cannot be recorded is refused rather than quietly unrecorded.
  if (!config.capture_device_path.empty())
    if (!impl.recorder.open(config.capture_device_path, impl.playback_channels))
    {
      error = "could not open '" + config.capture_device_path + "' to record what the device is sent";
      return nullptr;
    }
#else
  if (!config.capture_device_path.empty())
  {
    error = "this build cannot record what the device is sent; rebuild with "
            "-DNAMCHAIN_COREAUDIO_CAPTURE=ON (--capture-device-path)";
    return nullptr;
  }
#endif


  // 7. The info the engine reads. `capture_channels` is the device's when the capture is live and
  //    the file's otherwise -- as it is for `file` and `pipe`, the daemon refuses a backend that
  //    gives fewer than the configuration promised.
  impl.info.period_size = device_frames;
  impl.info.buffer_size = device_frames * impl.config.periods;
  impl.info.sample_rate = impl.config.sample_rate;
  impl.info.capture_channels = capture_channels;
  impl.info.playback_channels = impl.playback_channels;
  impl.info.format = PcmFormat::F32_LE;
  impl.info.bytes_per_sample = 4;
  impl.info.capture_bytes_per_frame = impl.info.capture_channels * impl.info.bytes_per_sample;
  impl.info.playback_bytes_per_frame = impl.info.playback_channels * impl.info.bytes_per_sample;
  impl.info.period_exact = true;
  // Both name the device the operator chose rather than the aggregate: "namchain (private)" is what
  // this backend called it, and it is what a status page would otherwise show.
  impl.info.capture_name = live_input ? (impl.input_device_name + " (input)")
                                      : (impl.config.input_path + " (looped)");
  impl.info.playback_name = impl.output_device_name;
  // The rings plus whatever the device adds in front of them. Read, not guessed, because this is the
  // number a latency report is built on. Both directions are read on their own scope: a device's
  // input latency is not its output latency.
  uint64_t device_latency_frames = 0;
  UInt32 safety = 0;
  UInt32 device_latency = 0;
  if (get_property(impl.device,
                   scoped_address(kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput), safety))
    device_latency_frames += safety;
  if (get_property(impl.device,
                   scoped_address(kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput), device_latency))
    device_latency_frames += device_latency;
  if (live_input)
  {
    if (get_property(impl.device,
                     scoped_address(kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput), safety))
      device_latency_frames += safety;
    if (get_property(impl.device,
                     scoped_address(kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput), device_latency))
      device_latency_frames += device_latency;
  }
  impl.info.buffer_floor_frames = static_cast<uint64_t>(impl.info.buffer_size) + device_latency_frames;
  // A file has something to give from the callback's first invocation; a capture device's first
  // period is an artefact of its own ring filling, which is what this flag exists to say (`alsa_raw`
  // sets it for the same reason). The daemon uses it to discard that period rather than to render it.
  impl.info.needs_bringup_cycle = live_input;

  // 8. The rings. Sized here, so neither the IOProc nor the engine's read allocates.
  const unsigned capacity = impl.config.output_queue_periods == 0 ? 8 : impl.config.output_queue_periods;
  impl.playback.reset(new SpscRing<std::vector<float>>(capacity));
  impl.capture.reset(new SpscRing<std::vector<float>>(2));
  // Zero means "no cap beyond the ring", matching what the field means everywhere else. What is
  // reported is the ring's own capacity rather than the request, because the ring rounds it up to a
  // power of two and `info.output_queue_periods` is documented as the number that refuses: reporting
  // the configuration here would state a bound the ring does not have.
  impl.queue_periods = static_cast<unsigned>(impl.playback->capacity());
  impl.info.output_queue_periods = impl.queue_periods;
  impl.info.output_queue_floor_periods = 0;
  // Sized from the *device's* period, which is the number of frames the IOProc is handed and the
  // number the engine's loop will ask for.
  impl.capture_scratch.assign(static_cast<std::size_t>(impl.info.period_size) * impl.info.capture_channels, 0.0f);
  impl.silence.assign(static_cast<std::size_t>(impl.info.period_size), 0.0f);
#if defined(NAMCHAIN_COREAUDIO_CAPTURE)
  for (auto& slice : impl.recorder_slice)
    slice.assign(static_cast<std::size_t>(impl.info.period_size), 0.0f);
#endif
  for (auto& scratch : impl.period_scratch)
    scratch.assign(static_cast<std::size_t>(impl.info.period_size) * impl.playback_channels, 0.0f);
  impl.playback_planar.assign(static_cast<std::size_t>(impl.info.period_size) * impl.playback_channels, 0.0f);

  // 9. The IOProc. A dispatch queue of our own rather than the HAL's IO thread: the callback does not
  //    block either way, and a named queue makes a stall attributable to us.
  impl.capture_ready = dispatch_semaphore_create(0);
  impl.queue = dispatch_queue_create("cc.hemsley.namchain.coreaudio", DISPATCH_QUEUE_SERIAL);
  if (impl.queue == nullptr)
  {
    error = "could not create the CoreAudio dispatch queue";
    return nullptr;
  }
  const OSStatus created = AudioDeviceCreateIOProcIDWithBlock(
    &impl.io_proc, impl.device, impl.queue,
    ^(const AudioTimeStamp* now, const AudioBufferList* input, const AudioTimeStamp* input_time,
      AudioBufferList* output, const AudioTimeStamp* output_time) {
      impl.io(now, input, input_time, output, output_time);
    });
  if (!ca_ok(created))
  {
    error = "AudioDeviceCreateIOProcIDWithBlock: " + os_status_text(created);
    return nullptr;
  }

  // 10. A device that is unplugged is a device that has stopped calling back, which the engine would
  //     eventually conclude on its own -- this just makes it immediate, and names the reason. The C
  //     proc form with `this` as client data: no block to copy, retain or release.
  impl.alive_address = global_address(kAudioDevicePropertyDeviceIsAlive);
  impl.alive_listener_registered =
    ca_ok(AudioObjectAddPropertyListener(impl.device, &impl.alive_address, &device_alive_listener, &impl));

  return self;
}

bool CoreAudioBackend::start(std::string& error)
{
  Implementation& impl = *self_;
  if (impl.started)
    return true;

  impl.loop.reset();
  impl.lost.store(false, std::memory_order_release);
  impl.lost_reason[0] = 0;
  impl.stopping.store(false, std::memory_order_release);
  impl.capture_credits.store(0, std::memory_order_release);
  impl.playback_underruns.store(0, std::memory_order_relaxed);
  impl.capture_overruns.store(0, std::memory_order_relaxed);
  impl.info.capture_exhausted = false;
  impl.capture_scratch.assign(impl.capture_scratch.size(), 0.0f);

  // Both rings start empty: three periods of stale audio from a previous run would be three periods
  // of the wrong thing at the top of this one.
  {
    std::vector<float> discarded;
    while (impl.playback->pop(discarded))
    {
    }
    while (impl.capture->pop(discarded))
    {
    }
  }

  // The output cushion, before the device is started: the engine does not call
  // prefill_output_queue, and a ring that begins the run empty underruns on the first callback. Two
  // periods, always -- `--output-queue-periods` sizes the ring's capacity, and this is the part of it
  // the run starts with.
  impl.playback->push(impl.silence);
  impl.playback->push(impl.silence);

  // Size every ring slot while the device is still stopped. `push` copies into a slot vector, and an
  // empty slot's first copy is the one that allocates -- which would happen on the device's thread
  // for capture and on the engine's audio thread for playback. Pushing the correctly-sized period
  // and popping it straight back leaves the slot sized and the ring empty, so every later push is
  // pure copy. This is the only place the rings are touched from one thread.
  {
    std::vector<float> sized;
    for (int i = 0; i < 2; ++i)
      if (impl.capture->push(impl.capture_scratch))
        impl.capture->pop(sized);
    if (impl.playback->push(impl.silence))
      impl.playback->pop(sized);
  }

  const OSStatus status = AudioDeviceStart(impl.device, impl.io_proc);
  if (!ca_ok(status))
  {
    error = "AudioDeviceStart: " + os_status_text(status);
    return false;
  }
  impl.started = true;

  // Wait for the device to produce its first period, so a device that accepts a start and never
  // calls back is reported as a lost device rather than hanging the render loop. The capture ring
  // holds two periods, so this also means the callback has run twice and the loop has somewhere to
  // read from the moment start() returns.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCoreAudioStartTimeoutMs);
  while (impl.capture_credits.load(std::memory_order_acquire) == 0)
  {
    if (impl.lost_error(error))
    {
      stop();
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline)
    {
      // The device is running but silent, or the queue is not being serviced. Reported as a device
      // that will not start, which is what it is from here.
      AudioDeviceStop(impl.device, impl.io_proc);
      impl.started = false;
      error = "the CoreAudio device did not produce a period within " +
              std::to_string(kCoreAudioStartTimeoutMs) + " ms of starting";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

void CoreAudioBackend::stop()
{
  if (self_ == nullptr)
    return;
  Implementation& impl = *self_;
  // Told before anything is torn down, so a render thread blocked in wait_capture wakes now rather
  // than at its timeout. The comparison is on the state, and `start()` clears it, so a stop/start
  // pair is still allowed.
  impl.stopping.store(true, std::memory_order_release);
  if (impl.device != kAudioObjectUnknown && impl.alive_listener_registered)
  {
    // Removed first: it points at this object, and the object is about to go.
    AudioObjectRemovePropertyListener(impl.device, &impl.alive_address, &device_alive_listener, &impl);
    impl.alive_listener_registered = false;
  }
  if (impl.device != kAudioObjectUnknown && impl.io_proc != nullptr)
  {
    if (impl.started)
    {
      AudioDeviceStop(impl.device, impl.io_proc);
      impl.started = false;
    }
    AudioDeviceDestroyIOProcID(impl.device, impl.io_proc);
    impl.io_proc = nullptr;
  }
#if defined(NAMCHAIN_COREAUDIO_CAPTURE)
  // Closed before the device is forgotten: this writes the WAV header, which needs the frame count.
  impl.recorder.close();
#endif
  // Nothing of the device's own configuration is touched at open time -- the buffer-frame-size probe
  // writes no property and the format is read-only -- so there is nothing to put back here. That is
  // deliberate: `kAudioDevicePropertyBufferFrameSize` is a system-wide setting, and a development
  // tool has no business changing it, let alone leaving it changed.
  if (impl.queue != nullptr)
  {
    // After the IOProc is destroyed, which is what makes releasing here safe: the queue has a +1
    // reference from dispatch_queue_create and the HAL holds no reference of its own once the proc
    // is gone. This is a .cpp, so no ARC, so the C entry point is the right spelling.
    dispatch_release(impl.queue);
    impl.queue = nullptr;
  }
  // The device's buffer goes back to what it was found as. This is the one property this backend
  // writes, and writing it without putting it back would leave somebody's interface -- and every other
  // application that opens it -- at 32 frames because a monitoring tool was tried once. A crash
  // between the write and this line leaves it set, which is the price of touching it at all; the same
  // value is in Audio MIDI Setup for whoever has to put it right.
  if (impl.buffer_frames_set && impl.device != kAudioObjectUnknown && impl.previous_buffer_frames > 0)
  {
    const UInt32 previous = impl.previous_buffer_frames;
    set_property(impl.device, global_address(kAudioDevicePropertyBufferFrameSize), previous);
    impl.buffer_frames_set = false;
  }
  // Last, and after the stream is stopped: the aggregate is this process's own object, so destroying
  // it takes it off the system as well as out of here. A private aggregate does not survive the
  // process that made it, but leaving it to that would leave it alive for as long as the daemon keeps
  // the device open after a `coreaudio` open that failed.
  if (impl.aggregate != kAudioObjectUnknown)
  {
    AudioHardwareDestroyAggregateDevice(impl.aggregate);
    impl.aggregate = kAudioObjectUnknown;
  }
  impl.device = kAudioObjectUnknown;
}

bool CoreAudioBackend::wait_capture(int timeout_ms, std::string& error)
{
  Implementation& impl = *self_;
  if (impl.lost_error(error))
    return false;

  // One block per callback rather than a poll: polling a period at 48 kHz spends most of it spinning,
  // which is what this backend did before the semaphore existed. The *credit* is what says a period is
  // there — a callback may carry more than one — and it is spent here, once per period the engine is
  // given. `dispatch_semaphore_wait` cannot be interrupted, so the deadline is re-derived from the
  // clock on every wake rather than assumed.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, timeout_ms));
  for (;;)
  {
    // Spend a credit only when the ring can honour it: a credit without a period behind it would
    // make `read_capture` come up empty.
    if (impl.capture_credits.load(std::memory_order_acquire) != 0 && impl.capture->size() != 0)
    {
      impl.capture_credits.fetch_sub(1, std::memory_order_acq_rel);
      return true;
    }
    if (impl.lost_error(error))
      return false;
    if (impl.stopping.load(std::memory_order_acquire))
    {
      // A shutdown is not a device failure, so this reports "no period" and lets the render loop see
      // its own running flag; a false here is read as a lost device, which is why the error string
      // stays empty.
      error.clear();
      return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
    dispatch_semaphore_wait(impl.capture_ready,
                            dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(remaining)));
  }
}

int CoreAudioBackend::read_capture(void* dst, unsigned frames, int /*timeout_ms*/, std::string& error)
{
  Implementation& impl = *self_;
  if (impl.lost_error(error))
    return -1;

  std::vector<float> period;
  if (!impl.capture->pop(period))
  {
    // wait_capture returned true, so this is a period that vanished between the two calls -- which
    // cannot happen with one consumer. Reported rather than filled with silence, because silence
    // here would be indistinguishable from a quiet input.
    error = "the CoreAudio capture ring was emptied between wait_capture and read_capture";
    return -1;
  }

  const size_t stride = impl.info.capture_bytes_per_frame;
  if (frames > impl.info.period_size)
    frames = impl.info.period_size;
  for (unsigned i = 0; i < frames; ++i)
    for (unsigned c = 0; c < impl.info.capture_channels; ++c)
      pcm_encode(static_cast<unsigned char*>(dst) + static_cast<std::size_t>(i) * stride +
                   static_cast<std::size_t>(c) * impl.info.bytes_per_sample,
                 PcmFormat::F32_LE, period[static_cast<std::size_t>(i) * impl.info.capture_channels + c]);
  return static_cast<int>(frames);
}

int CoreAudioBackend::write_playback(const void* /*src*/, unsigned /*frames*/, int /*timeout_ms*/, std::string& error)
{
  // The daemon only ever calls try_write_playback. This is here because the interface declares it:
  // blocking on a callback-driven device would mean waiting for the device to drain the ring, which
  // is the one thing this backend cannot do from the engine thread.
  error = "the coreaudio backend does not implement a blocking write; use try_write_playback";
  return -1;
}

int CoreAudioBackend::try_write_playback(const void* src, unsigned frames, std::string& error)
{
  Implementation& impl = *self_;
  if (impl.lost_error(error))
    return -1;

  const unsigned count = std::min(frames, impl.info.period_size);
  // Decode the interface's interleaved bytes into `playback_planar`, which is channel-major: the
  // layout the IOProc reads and the layout of a ring slot. Both are sized in create(), so nothing
  // here allocates -- this runs on the engine's audio thread.
  const size_t stride = impl.info.playback_bytes_per_frame;
  // A chain writes the one channel it is bound to, and every other channel of the bus stays silent.
  // A document with a single chain therefore fills channel 0 and leaves the rest of the bus at zero
  // -- correctly, because silence is what no chain having that channel means. What the *device* must
  // not then be sent is that silence: it has two channels and one signal, so the last channel that
  // was actually written is the one repeated. Remembered here, where the engine's bytes are read.
  impl.engine_last_channel = 0;
  for (unsigned i = 0; i < count; ++i)
    for (unsigned c = 0; c < impl.info.playback_channels; ++c)
      impl.playback_planar[static_cast<std::size_t>(c) * count + i] =
        pcm_decode(static_cast<const unsigned char*>(src) + static_cast<std::size_t>(i) * stride +
                     static_cast<std::size_t>(c) * impl.info.bytes_per_sample,
                   PcmFormat::F32_LE);

  // Which channels this period actually carried. A channel is "used" when it holds anything but
  // zero; a bus whose second channel is silently zero is a mono document, not a stereo one.
  for (unsigned c = 0; c < impl.info.playback_channels; ++c)
  {
    const float* slice = impl.playback_planar.data() + static_cast<std::size_t>(c) * count;
    for (unsigned i = 0; i < count; ++i)
      if (slice[i] != 0.0f)
      {
        impl.engine_last_channel.store(std::max(impl.engine_last_channel.load(), c));
        break;
      }
  }

  // The slot is sized to `period_size` channels by start(); this period is always that long, because
  // the engine's loop asks for `period_size` and the interface refuses a device that gives another.
  if (!impl.playback->push(impl.playback_planar))
  {
    // No room: the device has not taken a period yet. A dropped period is the contract for this call
    // (0, not -1) -- the engine counts it rather than blocking a loop that has a deadline.
    return 0;
  }
  return static_cast<int>(count);
}

StreamQueueStats CoreAudioBackend::queue_stats() const
{
  const Implementation& impl = *self_;
  StreamQueueStats stats;
  stats.capture_overruns = impl.capture_overruns.load(std::memory_order_relaxed);
  stats.playback_underruns = impl.playback_underruns.load(std::memory_order_relaxed);
  stats.output_queue_periods = impl.queue_periods;
  stats.output_drops = impl.playback->refused();
  return stats;
}

uint64_t CoreAudioBackend::driver_xruns() const
{
  const Implementation& impl = *self_;
  return impl.capture_overruns.load(std::memory_order_relaxed) +
         impl.playback_underruns.load(std::memory_order_relaxed);
}

bool CoreAudioBackend::ring_occupancy(long long& capture_frames, long long& playback_frames) const
{
  const Implementation& impl = *self_;
  capture_frames = static_cast<long long>(impl.capture->size()) * impl.info.period_size;
  playback_frames = static_cast<long long>(impl.playback->size()) * impl.info.period_size;
  return true;
}

std::vector<std::pair<std::string, long long>> CoreAudioBackend::diagnostics() const
{
  const Implementation& impl = *self_;
  // `loop_wraps` is the one that matters when the capture is a file: each wrap is exactly one pass of
  // it, so a client can divide the file's frame count by the wrap rate and see the rate the engine is
  // really consuming capture at. It is how "the loop plays at the right speed" became checkable from
  // outside. It stays zero for a live input, and `live_input` is what says so rather than leaving a
  // reader to wonder.
  return {{"loop_wraps", static_cast<long long>(impl.loop.wraps())},
          {"device_period_frames", static_cast<long long>(impl.info.period_size)},
          {"live_input", impl.live_input ? 1 : 0},
          {"aggregate_device", impl.aggregate != kAudioObjectUnknown ? 1 : 0},
          // The two halves of `xruns`, which the summary number cannot tell apart: a capture overrun
          // is the engine not having read the last one or two periods before the device produced
          // more, and a playback underrun is the device having run out of processed audio.
          {"capture_overruns", static_cast<long long>(impl.capture_overruns.load(std::memory_order_relaxed))},
          {"playback_underruns", static_cast<long long>(impl.playback_underruns.load(std::memory_order_relaxed))},
          {"periods_per_callback", static_cast<long long>(impl.max_periods_per_callback.load(std::memory_order_relaxed))},
          // What the device's buffer actually is, which is the number `--device-buffer-frames` is
          // about and which `period_size` only shows as a frame count per callback. 0 means it was
          // never asked for.
          {"device_buffer_frames", static_cast<long long>(impl.device_buffer_frames)}};
}

void CoreAudioBackend::interrupt()
{
  // Signalling the semaphore rather than setting `stopping` here: this can be called while the
  // engine's render thread is still running normally (the daemon calls it on its way to a stop, and
  // the device is still open at that point), and a flag that made wait_capture lie would end the
  // stream. One extra token is all it takes to make the wait return; the wait's own loop then sees
  // the device state for what it is.
  dispatch_semaphore_signal(self_->capture_ready);
}

} // namespace namchain

#endif // __APPLE__
