#include "trading/latency_tracker.h"
#include <algorithm>
#include <numeric>

namespace MarketMaker {

LatencyTracker::LatencyTracker(size_t max_samples)
    : max_samples_(max_samples) {
    samples_.resize(max_samples_);
}

void LatencyTracker::record(int64_t duration_us) {
    samples_[head_ % max_samples_] = duration_us;
    head_++;
    if (count_ < max_samples_) {
        count_++;
    }
}

double LatencyTracker::percentile(double p) const {
    if (count_ == 0) return 0.0;

    // Copy active samples to temp vector
    std::vector<int64_t> active(count_);
    size_t start = (head_ > max_samples_) ? (head_ - max_samples_) : 0;
    for (size_t i = 0; i < count_; ++i) {
        active[i] = samples_[(start + i) % max_samples_];
    }

    // nth_element for O(n) percentile selection
    size_t idx = static_cast<size_t>(p * (count_ - 1));
    std::nth_element(active.begin(), active.begin() + idx, active.end());
    return static_cast<double>(active[idx]);
}

double LatencyTracker::mean() const {
    if (count_ == 0) return 0.0;

    int64_t sum = 0;
    size_t start = (head_ > max_samples_) ? (head_ - max_samples_) : 0;
    for (size_t i = 0; i < count_; ++i) {
        sum += samples_[(start + i) % max_samples_];
    }
    return static_cast<double>(sum) / count_;
}

int64_t LatencyTracker::max() const {
    if (count_ == 0) return 0;

    int64_t max_val = 0;
    size_t start = (head_ > max_samples_) ? (head_ - max_samples_) : 0;
    for (size_t i = 0; i < count_; ++i) {
        max_val = std::max(max_val, samples_[(start + i) % max_samples_]);
    }
    return max_val;
}

size_t LatencyTracker::count() const {
    return count_;
}

void LatencyTracker::reset() {
    head_ = 0;
    count_ = 0;
}

} // namespace MarketMaker
