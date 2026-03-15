#ifndef LATENCY_TRACKER_H
#define LATENCY_TRACKER_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>

namespace MarketMaker {

// Thread-safe circular buffer latency tracker with percentile support.
class LatencyTracker {
public:
    explicit LatencyTracker(size_t max_samples = 10000);

    void record(int64_t duration_us);
    double percentile(double p) const;  // p in [0,1]
    double mean() const;
    int64_t max() const;
    size_t count() const;
    void reset();

private:
    mutable std::mutex mutex_;
    std::vector<int64_t> samples_;
    size_t head_ = 0;
    size_t count_ = 0;
    size_t max_samples_;
};

} // namespace MarketMaker
#endif // LATENCY_TRACKER_H
