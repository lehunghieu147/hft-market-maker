#include "trading/volatility_tracker.h"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace MarketMaker {

VolatilityTracker::VolatilityTracker(size_t window_size,
                                     double min_spread,
                                     double max_spread)
    : window_size_(window_size),
      min_spread_(min_spread),
      max_spread_(max_spread) {}

void VolatilityTracker::on_price(double price) {
    if (!std::isfinite(price) || price <= 0) return;

    std::lock_guard<std::mutex> lock(mutex_);

    prices_.push_back(price);

    if (prices_.size() <= window_size_) {
        // Window not full yet: use Welford's incremental update
        count_++;
        double delta = price - mean_;
        mean_ += delta / static_cast<double>(count_);
        double delta2 = price - mean_;
        m2_ += delta * delta2;
    } else {
        // Window full: evict oldest, recompute from scratch
        // Welford's removal is numerically tricky, so recompute is safer
        prices_.pop_front();
        recompute_welford();
    }

    // Set baseline after enough samples
    if (!baseline_set_ && count_ >= window_size_ / 2) {
        double variance = (count_ > 1) ? m2_ / static_cast<double>(count_ - 1) : 0.0;
        baseline_volatility_ = std::sqrt(std::max(0.0, variance));
        if (baseline_volatility_ > 0) {
            baseline_set_ = true;
        }
    }
}

void VolatilityTracker::recompute_welford() {
    count_ = prices_.size();
    mean_ = 0.0;
    m2_ = 0.0;

    if (count_ == 0) return;

    // Welford's algorithm from scratch
    double n = 0;
    for (double p : prices_) {
        n++;
        double delta = p - mean_;
        mean_ += delta / n;
        double delta2 = p - mean_;
        m2_ += delta * delta2;
    }
}

double VolatilityTracker::get_volatility() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ < 2) return 0.0;
    double variance = m2_ / static_cast<double>(count_ - 1);
    return std::sqrt(std::max(0.0, variance));
}

double VolatilityTracker::get_adjusted_spread(double base_spread) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!baseline_set_ || count_ < 2) {
        return std::clamp(base_spread, min_spread_, max_spread_);
    }

    double variance = m2_ / static_cast<double>(count_ - 1);
    double current_vol = std::sqrt(std::max(0.0, variance));

    // Scale spread by relative volatility (current / baseline)
    double vol_ratio = (baseline_volatility_ > 0)
        ? current_vol / baseline_volatility_
        : 1.0;

    double adjusted = base_spread * vol_ratio;
    return std::clamp(adjusted, min_spread_, max_spread_);
}

void VolatilityTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    prices_.clear();
    mean_ = 0.0;
    m2_ = 0.0;
    count_ = 0;
    baseline_volatility_ = 0.0;
    baseline_set_ = false;
}

} // namespace MarketMaker
