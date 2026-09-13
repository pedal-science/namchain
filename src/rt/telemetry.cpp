#include "namchain/telemetry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace namchain
{

BlockTimer::BlockTimer(uint64_t deadline_ns, uint64_t expected_blocks) : deadline_ns_(deadline_ns)
{
  // Reserved from the caller's own count rather than from a fixed worst case: the run that asks for
  // a second and the run that asks for an hour differ by six orders of magnitude in what they need
  // to keep, and reserving the hour's worth for both is 44 MB a timer.
  if (expected_blocks > 0)
    durations_.reserve(static_cast<size_t>(expected_blocks));
}

void BlockTimer::record(uint64_t duration_ns)
{
  durations_.push_back(duration_ns);
  total_ns_ += duration_ns;
  if (duration_ns > deadline_ns_)
    ++overruns_;
  if (duration_ns > max_ns_)
    max_ns_ = duration_ns;
}

uint64_t BlockTimer::percentile(double quantile) const
{
  if (durations_.empty())
    return 0;
  std::vector<uint64_t> sorted(durations_);
  std::sort(sorted.begin(), sorted.end());
  // Nearest-rank definition: p of n samples is the value at rank ceil(q*n). Using the
  // interpolated definition instead would report a p50 between two samples, which is not a
  // block time that ever occurred -- misleading in a report about observed lateness.
  const double rank = std::ceil(quantile * static_cast<double>(sorted.size()));
  const size_t index = static_cast<size_t>(std::max(1.0, rank)) - 1;
  return sorted[std::min(index, sorted.size() - 1)];
}

uint64_t BlockTimer::p50_ns() const
{
  return percentile(0.50);
}

uint64_t BlockTimer::p99_ns() const
{
  return percentile(0.99);
}

uint64_t BlockTimer::max_ns() const
{
  return max_ns_;
}

double BlockTimer::p99_percent_of_deadline() const
{
  if (deadline_ns_ == 0 || durations_.empty())
    return 0.0;
  return 100.0 * static_cast<double>(p99_ns()) / static_cast<double>(deadline_ns_);
}

void BlockTimer::reset()
{
  durations_.clear();
  overruns_ = 0;
  total_ns_ = 0;
  max_ns_ = 0;
}

} // namespace namchain
