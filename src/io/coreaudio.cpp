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
    /// The device's own clock, and the host clock beside it: the pair the rate is derived from.

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

  AudioObjectID device = kAudioObjectUnknown;
  AudioDeviceIOProcID io_proc = nullptr;
  dispatch_queue_t queue = nullptr;
  bool started = false;

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

  /// The IOProc's own copy target for the period it pops, so the callback allocates nothing.
  std::vector<float> period_scratch;
  /// The most periods one callback can carry. A device asking for more is refused rather than
  /// overrunning this array -- see the guard in `io`.
  static constexpr unsigned kMaxPeriodsPerCallback = 16;
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
  void io(const AudioTimeStamp* /*now*/, const AudioBufferList* /*input*/, const AudioTimeStamp* /*input_time*/,
          AudioBufferList* output, const AudioTimeStamp* /*output_time*/)
  {
    const unsigned channels = output->mNumberBuffers > 0 ? output->mBuffers[0].mNumberChannels : 0;
    const unsigned bytes = output->mNumberBuffers > 0 ? output->mBuffers[0].mDataByteSize : 0;
    const unsigned frames = bytes / (sizeof(float) * (channels == 0 ? 1 : channels));
    const unsigned period = info.period_size;

    if (frames == 0 || period == 0 || (frames % period) != 0)
    {
      // A callback that is not a whole number of periods cannot be divided without inventing an
      // offset. Zero the output, record why once, and let the engine's next call report it.
      mark_lost_rt("asked for", frames, period);
      return;
    }

    // How many engine periods this one callback carries. A device is free to present a larger IO
    // buffer than the engine's quantum: the callback hands the engine one period per period's worth
    // of frames, and asks for the same number back.
    const unsigned periods = frames / period;
    if (periods > kMaxPeriodsPerCallback)
    {
      mark_lost_rt("asked for", frames, period);
      return;
    }
    // One pop per period, before the fill: the fill above must not branch on the ring, and a period
    // that did not arrive is a period of silence and one underrun.
    for (unsigned p = 0; p < periods; ++p)
    {
      playback_fresh[p] = playback->pop(period_scratch);
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
      if (!playback_fresh[period_index])
        continue;                                        // left zeroed: this period did not arrive

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
          period_scratch[static_cast<std::size_t>(source_channel) * period + frame_in_period];

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
    // And the same number of periods back in. Each one is read from the loop and pushed, so the
    // engine's `read_capture` finds exactly what it was credited for.
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
    // One wake per callback, not one per period: the engine spends credits itself, and a second
    // signal would leave a token in the semaphore with nothing behind it.
    dispatch_semaphore_signal(capture_ready);
  }

#if defined(NAMCHAIN_COREAUDIO_CAPTURE)
  /// What the device was sent, when something asked for it. Empty (and unused) otherwise.
  DeviceRecorder recorder;
  /// Whether `--capture-device-path` was given but this build cannot honour it: refused by name in
  /// `create`, the same as any other thing a build does not have.
  bool capture_requested = false;
  bool capture_supported = true;
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
  if (config.input_path.empty())
  {
    error = "the coreaudio backend needs --capture-path: the WAV to loop as its input";
    return nullptr;
  }

  std::unique_ptr<CoreAudioBackend> self(new CoreAudioBackend());
  Implementation& impl = *self->self_;

  impl.config.input_path = config.input_path;
  impl.config.device_uid = config.device;
  impl.config.sample_rate = config.sample_rate;
  impl.config.period_size = config.period_size;
  impl.config.periods = config.periods == 0 ? 4 : config.periods;
  impl.config.capture_channels = config.capture_channels;
  impl.config.playback_channels = config.playback_channels;
  impl.config.output_queue_periods = config.output_queue_periods;

  // 1. The signal. Loaded whole, before anything is opened: the IOProc reads it on the device's
  //    thread, and a file read there would be a dropout.
  if (!LoopSource::load(impl.config.input_path, impl.config.sample_rate, impl.loop, error))
    return nullptr;

  // 2. The device: the operator's UID, or the system's default. A UID rather than an index, because
  //    indices move when something is unplugged.
  if (impl.config.device_uid.empty())
  {
    AudioObjectID default_device = kAudioObjectUnknown;
    if (!get_property(kAudioObjectSystemObject, global_address(kAudioHardwarePropertyDefaultOutputDevice),
                      default_device) ||
        default_device == kAudioObjectUnknown)
    {
      error = "there is no default CoreAudio output device";
      return nullptr;
    }
    impl.device = default_device;
  }
  else
  {
    CFStringRef uid = CFStringCreateWithCString(kCFAllocatorDefault, impl.config.device_uid.c_str(),
                                                kCFStringEncodingUTF8);
    if (uid == nullptr)
    {
      error = "the CoreAudio device UID '" + impl.config.device_uid + "' is not valid UTF-8";
      return nullptr;
    }
    AudioValueTranslation translation;
    translation.mInputData = &uid;
    translation.mInputDataSize = sizeof(uid);
    AudioObjectID found = kAudioObjectUnknown;
    translation.mOutputData = &found;
    translation.mOutputDataSize = sizeof(found);
    UInt32 size = sizeof(translation);
    // A copy: the HAL takes a mutable address even for a read.
    AudioObjectPropertyAddress address = global_address(kAudioHardwarePropertyTranslateUIDToDevice);
    const OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &translation);
    CFRelease(uid);
    if (!ca_ok(status) || found == kAudioObjectUnknown)
    {
      error = "no CoreAudio device with UID '" + impl.config.device_uid + "'";
      return nullptr;
    }
    impl.device = found;
  }

  // 3. It has to be there now. A device that is alive at open and dies later is the daemon's lost
  //    device path; a device that was never there is a configuration error.
  UInt32 alive = 0;
  if (!get_property(impl.device, global_address(kAudioDevicePropertyDeviceIsAlive), alive))
  {
    error = "the CoreAudio device '" + impl.config.device_uid + "' has no IsAlive property";
    return nullptr;
  }
  if (alive == 0)
  {
    error = "the CoreAudio device '" + impl.config.device_uid + "' is not alive";
    return nullptr;
  }

  // 4. The rate. The engine has no resampler, so a device at another rate is refused rather than
  //    asked to change: changing it would be the app editing a system-wide setting.
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

  // 5. The format: float32, native endian, the device's own channel count. A device that presents
  //    something else is refused rather than converted -- this backend has no format conversion, and
  //    a silent one would be worse than a refusal.
  AudioStreamBasicDescription format{};
  if (!get_property(impl.device, global_address(kAudioDevicePropertyStreamFormat), format))
  {
    error = "the CoreAudio device's stream format could not be read";
    return nullptr;
  }
  const bool is_float = (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0 &&
                        (format.mFormatFlags & kAudioFormatFlagIsBigEndian) == 0;
  if (format.mFormatID != kAudioFormatLinearPCM || !is_float || format.mBitsPerChannel != 32)
  {
    error = "the CoreAudio device does not present 32-bit float linear PCM, so this backend cannot drive it";
    return nullptr;
  }
  if (format.mChannelsPerFrame < impl.config.playback_channels)
  {
    error = "the CoreAudio device has " + std::to_string(format.mChannelsPerFrame) +
            " playback channels, configured for " + std::to_string(impl.config.playback_channels);
    return nullptr;
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

  impl.playback_channels = format.mChannelsPerFrame;

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


  // 7. The info the engine reads. `capture_channels` is the file's, as it is for `file` and `pipe`:
  //    the daemon refuses a device that gives fewer than the configuration promised.
  impl.info.period_size = device_frames;
  impl.info.buffer_size = device_frames * impl.config.periods;
  impl.info.sample_rate = impl.config.sample_rate;
  impl.info.capture_channels = impl.loop.channels();
  impl.info.playback_channels = impl.playback_channels;
  impl.info.format = PcmFormat::F32_LE;
  impl.info.bytes_per_sample = 4;
  impl.info.capture_bytes_per_frame = impl.info.capture_channels * impl.info.bytes_per_sample;
  impl.info.playback_bytes_per_frame = impl.info.playback_channels * impl.info.bytes_per_sample;
  impl.info.period_exact = true;
  impl.info.capture_name = impl.config.input_path + " (looped)";
  impl.info.playback_name = device_name(impl.device);
  // The rings plus whatever the device adds in front of them. Read, not guessed, because this is the
  // number a latency report is built on.
  uint64_t device_latency_frames = 0;
  UInt32 safety = 0;
  UInt32 device_latency = 0;
  if (get_property(impl.device, global_address(kAudioDevicePropertySafetyOffset), safety))
    device_latency_frames += safety;
  if (get_property(impl.device, global_address(kAudioDevicePropertyLatency), device_latency))
    device_latency_frames += device_latency;
  impl.info.buffer_floor_frames = static_cast<uint64_t>(impl.info.buffer_size) + device_latency_frames;
  // The callback produces input from its first invocation, so the first period is not an artefact of
  // a ring filling -- unlike a capture device, where it is.
  impl.info.needs_bringup_cycle = false;

  // 8. The rings. Sized here, so neither the IOProc nor the engine's read allocates.
  const unsigned capacity = impl.config.output_queue_periods == 0 ? 8 : impl.config.output_queue_periods;
  impl.playback.reset(new SpscRing<std::vector<float>>(capacity));
  impl.capture.reset(new SpscRing<std::vector<float>>(2));
  // Zero means "no cap beyond the ring", matching what the field means everywhere else. Otherwise the
  // requested number is the cap, rounded up by the ring's power-of-two capacity.
  impl.queue_periods = impl.config.output_queue_periods;
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
  impl.period_scratch.assign(static_cast<std::size_t>(impl.info.period_size) * impl.playback_channels, 0.0f);
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
  // `loop_wraps` is the one that matters: each wrap is exactly one pass of the file, so a client can
  // divide the file's frame count by the wrap rate and see the rate the engine is really consuming
  // capture at. It is how "the loop plays at the right speed" became checkable from outside.
  return {{"loop_wraps", static_cast<long long>(impl.loop.wraps())},
          {"device_period_frames", static_cast<long long>(impl.info.period_size)}};
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
