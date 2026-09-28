// The daemon's diagnostic recorder. See namchain/daemon/recorder.h for what it is for.
#include "namchain/daemon/recorder.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>

namespace namchain
{
namespace daemon
{
namespace
{

/// A slot's header: the period's sequence number and its events. Kept a multiple of 8 so the audio
/// that follows it is aligned.
struct SlotHeader
{
  uint64_t sequence = 0;
  RecorderEvents events;
};
static_assert(sizeof(SlotHeader) % 8 == 0, "the slot header must keep the audio aligned");

/// Four seconds of periods. The writer drains every few milliseconds, so this is room for a slow
/// disk, not a working depth.
constexpr double kRingSeconds = 4.0;
constexpr int kWriterIdleMs = 10;
constexpr std::size_t kWavHeaderBytes = 44;

void put_u16(unsigned char* at, uint16_t value)
{
  at[0] = static_cast<unsigned char>(value & 0xFFu);
  at[1] = static_cast<unsigned char>(value >> 8);
}

void put_u32(unsigned char* at, uint32_t value)
{
  for (int i = 0; i < 4; ++i)
    at[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFFu);
}

/// The canonical 44-byte header. The sizes are patched when the file is closed; until then they say
/// "empty", which is what a file cut short by a crash then honestly is to a strict reader.
void wav_header(unsigned char* out, PcmFormat format, unsigned channels, unsigned rate, uint32_t data_bytes)
{
  const unsigned width = static_cast<unsigned>(pcm_bytes_per_sample(format));
  std::memcpy(out, "RIFF", 4);
  put_u32(out + 4, 36u + data_bytes);
  std::memcpy(out + 8, "WAVEfmt ", 8);
  put_u32(out + 16, 16);
  put_u16(out + 20, format == PcmFormat::F32_LE ? 3 : 1);
  put_u16(out + 22, static_cast<uint16_t>(channels));
  put_u32(out + 24, rate);
  put_u32(out + 28, rate * channels * width);
  put_u16(out + 32, static_cast<uint16_t>(channels * width));
  put_u16(out + 34, static_cast<uint16_t>(8 * width));
  std::memcpy(out + 36, "data", 4);
  put_u32(out + 40, data_bytes);
}

bool write_all(std::FILE* file, const void* data, std::size_t bytes)
{
  return bytes == 0 || std::fwrite(data, 1, bytes, file) == bytes;
}

std::FILE* open_wav(const std::string& path, const RecorderFormat& format, unsigned channels, std::string& error)
{
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
  {
    error = "cannot create " + path + ": " + std::strerror(errno);
    return nullptr;
  }
  unsigned char header[kWavHeaderBytes];
  wav_header(header, format.format, channels, format.sample_rate, 0);
  if (!write_all(file, header, sizeof(header)))
  {
    error = "cannot write " + path + ": " + std::strerror(errno);
    std::fclose(file);
    return nullptr;
  }
  return file;
}

void close_wav(std::FILE* file, const RecorderFormat& format, unsigned channels, uint64_t data_bytes)
{
  if (file == nullptr)
    return;
  unsigned char header[kWavHeaderBytes];
  wav_header(header, format.format, channels, format.sample_rate, static_cast<uint32_t>(data_bytes));
  std::fflush(file);
  if (std::fseek(file, 0, SEEK_SET) == 0)
    write_all(file, header, sizeof(header));
  std::fclose(file);
}

} // namespace

std::unique_ptr<Recorder> Recorder::start(const std::string& directory, const RecorderFormat& format,
                                          double max_seconds, const std::string& meta, std::string& error)
{
  const std::size_t width = pcm_bytes_per_sample(format.format);
  if (width == 0 || format.sample_rate == 0 || format.period_size == 0 || format.capture_channels == 0 ||
      format.playback_channels == 0)
  {
    error = "the device is not running, so there is no stream to record";
    return nullptr;
  }
  if (!(max_seconds > 0.0))
  {
    error = "a recording needs a length greater than zero";
    return nullptr;
  }

  std::unique_ptr<Recorder> self(new Recorder());
  self->directory_ = directory;
  self->format_ = format;
  self->capture_bytes_ = static_cast<std::size_t>(format.period_size) * format.capture_channels * width;
  self->playback_bytes_ = static_cast<std::size_t>(format.period_size) * format.playback_channels * width;
  self->slot_bytes_ = sizeof(SlotHeader) + ((self->capture_bytes_ + 7) & ~std::size_t{7}) +
                      ((self->playback_bytes_ + 7) & ~std::size_t{7});
  self->slot_count_ =
    static_cast<std::size_t>(kRingSeconds * format.sample_rate / format.period_size) + 1;

  // A RIFF size is 32 bits, so the capture file (the wider of the two) bounds the length.
  const uint64_t by_length =
    static_cast<uint64_t>(max_seconds * format.sample_rate / format.period_size + 0.5);
  const uint64_t by_riff = (std::numeric_limits<uint32_t>::max() - 36u) / self->capture_bytes_;
  self->max_periods_ = std::max<uint64_t>(1, std::min(by_length, by_riff));

  if (::mkdir(directory.c_str(), 0755) != 0)
  {
    error = errno == EEXIST ? "a recording called '" + directory + "' already exists"
                            : "cannot create " + directory + ": " + std::strerror(errno);
    return nullptr;
  }

  std::FILE* meta_file = std::fopen((directory + "/meta.json").c_str(), "w");
  if (meta_file == nullptr || !write_all(meta_file, meta.data(), meta.size()) ||
      !write_all(meta_file, "\n", 1))
  {
    error = "cannot write " + directory + "/meta.json: " + std::strerror(errno);
    if (meta_file != nullptr)
      std::fclose(meta_file);
    return nullptr;
  }
  std::fclose(meta_file);

  self->capture_file_ = open_wav(directory + "/capture.wav", format, format.capture_channels, error);
  if (self->capture_file_ == nullptr)
    return nullptr;
  self->playback_file_ = open_wav(directory + "/playback.wav", format, format.playback_channels, error);
  if (self->playback_file_ == nullptr)
    return nullptr;
  self->events_file_ = std::fopen((directory + "/events.tsv").c_str(), "w");
  if (self->events_file_ == nullptr)
  {
    error = "cannot create " + directory + "/events.tsv: " + std::strerror(errno);
    return nullptr;
  }
  std::fprintf(self->events_file_, "period\tframe\tevent\tcount\n");

  // Last: the slab is the big allocation, and it is touched here so the render thread's first
  // copies do not take the page faults.
  self->slab_.assign(self->slot_bytes_ * self->slot_count_, 0);
  self->writer_ = std::thread([raw = self.get()] { raw->writer_loop(); });
  return self;
}

Recorder::~Recorder()
{
  stop();
}

bool Recorder::push(const void* capture, std::size_t capture_bytes, const void* playback,
                    std::size_t playback_bytes, const RecorderEvents& events) noexcept
{
  if (complete_.load(std::memory_order_relaxed))
    return false;
  if (capture_bytes != capture_bytes_ || playback_bytes != playback_bytes_)
  {
    // The device came back with another shape. The files' headers describe the old one, so the
    // recording ends here rather than carrying on into audio they would misdescribe.
    complete_.store(true, std::memory_order_release);
    return false;
  }

  const uint64_t sequence = offered_++;
  if (offered_ >= max_periods_)
    complete_.store(true, std::memory_order_release);

  const uint64_t head = head_.load(std::memory_order_relaxed);
  const uint64_t tail = tail_.load(std::memory_order_acquire);
  if (head - tail >= slot_count_)
  {
    overflows_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  unsigned char* slot = slab_.data() + (head % slot_count_) * slot_bytes_;
  SlotHeader header;
  header.sequence = sequence;
  header.events = events;
  std::memcpy(slot, &header, sizeof(header));
  std::memcpy(slot + sizeof(SlotHeader), capture, capture_bytes_);
  std::memcpy(slot + sizeof(SlotHeader) + ((capture_bytes_ + 7) & ~std::size_t{7}), playback, playback_bytes_);
  head_.store(head + 1, std::memory_order_release);

  periods_.fetch_add(1, std::memory_order_relaxed);
  dropped_.fetch_add(events.dropped, std::memory_order_relaxed);
  inserted_.fetch_add(events.inserted, std::memory_order_relaxed);
  xruns_.fetch_add(events.xruns, std::memory_order_relaxed);
  return true;
}

bool Recorder::write_slot(const unsigned char* slot)
{
  SlotHeader header;
  std::memcpy(&header, slot, sizeof(header));

  const auto event = [this](uint64_t period, const char* name, uint64_t count) {
    std::fprintf(events_file_, "%llu\t%llu\t%s\t%llu\n", static_cast<unsigned long long>(period),
                 static_cast<unsigned long long>(period * format_.period_size), name,
                 static_cast<unsigned long long>(count));
  };

  // A gap is periods the ring refused: they are missing from both files, so everything after it is
  // earlier in the files than in the device's timeline, by exactly this much.
  if (header.sequence != next_sequence_)
    event(next_sequence_, "gap", header.sequence - next_sequence_);
  next_sequence_ = header.sequence + 1;

  // Periods are numbered as the files have them, so `frame` is a position in capture.wav.
  const uint64_t period = capture_data_bytes_ / capture_bytes_;
  if (header.events.dropped != 0)
    event(period, "dropped", header.events.dropped);
  if (header.events.inserted != 0)
    event(period, "inserted", header.events.inserted);
  if (header.events.xruns != 0)
    event(period, "xrun", header.events.xruns);
  if (header.events.bursts != 0)
    event(period, "burst", header.events.bursts);

  if (!write_all(capture_file_, slot + sizeof(SlotHeader), capture_bytes_) ||
      !write_all(playback_file_, slot + sizeof(SlotHeader) + ((capture_bytes_ + 7) & ~std::size_t{7}),
                 playback_bytes_))
  {
    write_error_ = directory_ + ": " + std::strerror(errno);
    return false;
  }
  capture_data_bytes_ += capture_bytes_;
  playback_data_bytes_ += playback_bytes_;
  return true;
}

void Recorder::writer_loop()
{
  for (;;)
  {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    const uint64_t head = head_.load(std::memory_order_acquire);
    if (tail == head)
    {
      if (stopping_.load(std::memory_order_acquire))
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(kWriterIdleMs));
      continue;
    }
    const unsigned char* slot = slab_.data() + (tail % slot_count_) * slot_bytes_;
    // A failed writer keeps draining, so the render thread never sees a ring that will not empty.
    if (!failed_.load(std::memory_order_relaxed) && !write_slot(slot))
    {
      failed_.store(true, std::memory_order_release);
      complete_.store(true, std::memory_order_release);
    }
    tail_.store(tail + 1, std::memory_order_release);
  }
  finish_files();
}

void Recorder::finish_files()
{
  close_wav(capture_file_, format_, format_.capture_channels, capture_data_bytes_);
  close_wav(playback_file_, format_, format_.playback_channels, playback_data_bytes_);
  capture_file_ = nullptr;
  playback_file_ = nullptr;
  if (events_file_ != nullptr)
    std::fclose(events_file_);
  events_file_ = nullptr;
}

void Recorder::stop()
{
  if (stopped_)
    return;
  stopped_ = true;
  complete_.store(true, std::memory_order_release);
  stopping_.store(true, std::memory_order_release);
  if (writer_.joinable())
    writer_.join();
  else
    finish_files();
}

std::string Recorder::write_error() const
{
  return failed_.load(std::memory_order_acquire) ? write_error_ : std::string();
}

} // namespace daemon
} // namespace namchain
