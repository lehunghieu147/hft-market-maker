#include "trading/volatility_tracker.h"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace MarketMaker {

VolatilityTracker::VolatilityTracker(size_t window_size,
                                     double min_spread,
                                     double max_spread,
                                     size_t fast_window_size,
                                     double regime_threshold,
                                     double regime_spread_mult)
    : window_size_(window_size),
      min_spread_(min_spread),
      max_spread_(max_spread),
      fast_window_size_(fast_window_size),
      regime_threshold_(regime_threshold),
      regime_spread_mult_(regime_spread_mult) {}

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

    // Fast window maintenance for regime detection
    fast_prices_.push_back(price);
    if (fast_prices_.size() <= fast_window_size_) {
        fast_count_++;
        double fd = price - fast_mean_;
        fast_mean_ += fd / static_cast<double>(fast_count_);
        double fd2 = price - fast_mean_;
        fast_m2_ += fd * fd2;
    } else {
        fast_prices_.pop_front();
        recompute_fast_welford();
    }

    // Baseline volatility: initialize once, then slowly adapt via EWMA
    if (count_ >= window_size_ / 2) {
        double variance = (count_ > 1) ? m2_ / static_cast<double>(count_ - 1) : 0.0;
        double current_vol = std::sqrt(std::max(0.0, variance));
        if (!baseline_set_ && current_vol > 0) {
            baseline_volatility_ = current_vol;
            baseline_set_ = true;
        } else if (baseline_set_) {
            constexpr double baseline_alpha = 0.001;  // Very slow adaptation
            baseline_volatility_ = baseline_alpha * current_vol
                                 + (1.0 - baseline_alpha) * baseline_volatility_;
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

double VolatilityTracker::get_baseline_volatility() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return baseline_volatility_;
}

double VolatilityTracker::get_volatility_ratio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!baseline_set_ || baseline_volatility_ <= 0 || count_ < 2) return 1.0;

    double variance = m2_ / static_cast<double>(count_ - 1);
    double current_vol = std::sqrt(std::max(0.0, variance));
    double ratio = current_vol / baseline_volatility_;
    return std::clamp(ratio, 0.5, 2.0);
}

double VolatilityTracker::get_fast_volatility() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fast_count_ < 2) return 0.0;
    double variance = fast_m2_ / static_cast<double>(fast_count_ - 1);
    return std::sqrt(std::max(0.0, variance));
}

double VolatilityTracker::get_regime_spread_multiplier() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fast_count_ < fast_window_size_ / 2 || count_ < window_size_ / 2) return 1.0;

    double slow_vol = (count_ > 1) ? std::sqrt(std::max(0.0, m2_ / (count_ - 1))) : 0.0;
    double fast_vol = (fast_count_ > 1) ? std::sqrt(std::max(0.0, fast_m2_ / (fast_count_ - 1))) : 0.0;

    if (slow_vol < 1e-12) return 1.0;
    double ratio = fast_vol / slow_vol;
    if (ratio >= regime_threshold_) {
        double t = std::min((ratio - regime_threshold_) / regime_threshold_, 1.0);
        return 1.0 + t * (regime_spread_mult_ - 1.0);
    }
    return 1.0;
}

void VolatilityTracker::recompute_fast_welford() {
    fast_count_ = fast_prices_.size();
    fast_mean_ = 0.0;
    fast_m2_ = 0.0;
    if (fast_count_ == 0) return;
    double n = 0;
    for (double p : fast_prices_) {
        n++;
        double delta = p - fast_mean_;
        fast_mean_ += delta / n;
        double delta2 = p - fast_mean_;
        fast_m2_ += delta * delta2;
    }
}

void VolatilityTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    prices_.clear();
    mean_ = 0.0;
    m2_ = 0.0;
    count_ = 0;
    fast_prices_.clear();
    fast_mean_ = 0.0;
    fast_m2_ = 0.0;
    fast_count_ = 0;
    baseline_volatility_ = 0.0;
    baseline_set_ = false;
}

} // namespace MarketMaker
