#pragma once

// A diagnostic recorder for the daemon: what the device delivered and what the engine gave back, a
// period at a time, as two WAV files a person can line up and compare.
//
// The question it exists to answer is "where does a click come from": the engine (the rig), the
// backend's queue (a dropped or inserted period, which is not an xrun and so is invisible in the
// xrun count), or something downstream of the daemon. So it records the device's own bytes -- every
// capture channel, which on an interface with a loopback return also carries what the interface
// received -- and the playback period the render loop offered, and it logs, per period, whether the
// backend dropped it or inserted periods after it.
//
// The render thread only copies into a preallocated ring; a writer thread of its own does the file
// I/O. Nothing on the render side allocates, locks or blocks.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "namchain/pcm.h"

namespace namchain
{
namespace daemon
{

/// The stream a recording is of. Fixed for the recording's life: a device that reopens with a
/// different shape ends it rather than writing a file whose header lies.
struct RecorderFormat
{
  unsigned sample_rate = 0;
  unsigned period_size = 0;
  unsigned capture_channels = 0;
  unsigned playback_channels = 0;
  PcmFormat format = PcmFormat::Unknown;
};

/// What happened to one period beyond its audio: the deltas of the backend's counters across it.
struct RecorderEvents
{
  /// The period was not written (the output queue was full, or the target refused it).
  uint32_t dropped = 0;
  /// Periods the output-queue floor inserted after this one.
  uint32_t inserted = 0;
  /// Driver xruns (either direction) while this period was read or written.
  uint32_t xruns = 0;
  /// A capture release with two periods already waiting: the loop had fallen a period behind.
  uint32_t bursts = 0;
};

class Recorder
{
public:
  /// \brief Create \p directory (which must not exist) and start the writer thread.
  ///
  /// Writes `capture.wav`, `playback.wav`, `events.tsv` and `meta.json` (the \p meta text, as given)
  /// there. The recording ends by itself after \p max_seconds of audio.
  static std::unique_ptr<Recorder> start(const std::string& directory, const RecorderFormat& format,
                                         double max_seconds, const std::string& meta, std::string& error);

  ~Recorder();
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  /// \brief Render thread: queue one period. Never allocates, locks or blocks.
  ///
  /// \p capture and \p playback are the device's interleaved bytes, of exactly the sizes the format
  /// implies. \return false when the period was not queued: the recording is complete, or the ring
  /// was full (counted as an overflow, and logged as a gap by the writer).
  bool push(const void* capture, std::size_t capture_bytes, const void* playback, std::size_t playback_bytes,
            const RecorderEvents& events) noexcept;

  /// \brief Control thread: stop taking periods, write out what is queued, and close the files.
  /// Idempotent.
  void stop();

  /// True once the recording has all the audio it will get (its length, or a format change).
  bool complete() const noexcept { return complete_.load(std::memory_order_acquire); }

  const std::string& directory() const noexcept { return directory_; }
  const RecorderFormat& format() const noexcept { return format_; }
  /// Periods queued by the render thread, and those refused because the ring was full.
  uint64_t periods() const noexcept { return periods_.load(std::memory_order_relaxed); }
  uint64_t overflows() const noexcept { return overflows_.load(std::memory_order_relaxed); }
  /// Totals of the events recorded so far, for a summary.
  uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
  uint64_t inserted() const noexcept { return inserted_.load(std::memory_order_relaxed); }
  uint64_t xruns() const noexcept { return xruns_.load(std::memory_order_relaxed); }
  /// Why the writer failed (a full disk, say), or empty. A failed recording stops taking periods.
  std::string write_error() const;

private:
  Recorder() = default;
  void writer_loop();
  bool write_slot(const unsigned char* slot);
  void finish_files();

  std::string directory_;
  RecorderFormat format_;
  std::size_t capture_bytes_ = 0;
  std::size_t playback_bytes_ = 0;
  std::size_t slot_bytes_ = 0;
  std::size_t slot_count_ = 0;
  uint64_t max_periods_ = 0;
  std::vector<unsigned char> slab_;

  // The ring: slot indices, one producer (render) and one consumer (writer).
  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) std::atomic<uint64_t> tail_{0};

  /// Render thread only: every period offered, queued or not, so the writer can see a gap.
  uint64_t offered_ = 0;
  alignas(64) std::atomic<uint64_t> periods_{0};
  std::atomic<uint64_t> overflows_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> inserted_{0};
  std::atomic<uint64_t> xruns_{0};
  std::atomic<bool> complete_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> failed_{false};

  std::thread writer_;
  std::FILE* capture_file_ = nullptr;
  std::FILE* playback_file_ = nullptr;
  std::FILE* events_file_ = nullptr;
  uint64_t next_sequence_ = 0;  ///< writer: the sequence number it expects next, to log gaps
  uint64_t capture_data_bytes_ = 0;
  uint64_t playback_data_bytes_ = 0;
  /// Written by the writer once, before it sets \ref failed_; read only after seeing it set.
  std::string write_error_;
  bool stopped_ = false;        ///< control thread
};

} // namespace daemon
} // namespace namchain
