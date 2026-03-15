#ifndef VOLATILITY_TRACKER_H
#define VOLATILITY_TRACKER_H

#include <deque>
#include <mutex>
#include <cstddef>

namespace MarketMaker {

class VolatilityTracker {
public:
    explicit VolatilityTracker(size_t window_size = 100,
                                double min_spread = 0.001,
                                double max_spread = 0.05,
                                size_t fast_window_size = 20,
                                double regime_threshold = 2.0,
                                double regime_spread_mult = 2.0);

    // Feed a new price observation
    void on_price(double price);

    // Get current volatility (standard deviation)
    double get_volatility() const;

    // Get spread adjusted by relative volatility
    double get_adjusted_spread(double base_spread) const;

    // Get baseline volatility (slow-moving EWMA of rolling stddev)
    double get_baseline_volatility() const;

    // Get volatility ratio (current / baseline), clamped to [0.5, 2.0]
    double get_volatility_ratio() const;

    // Fast-window volatility for regime detection
    double get_fast_volatility() const;

    // Returns spread multiplier >= 1.0 when fast vol >> slow vol (regime shift)
    double get_regime_spread_multiplier() const;

    // Reset all state
    void reset();

private:
    size_t window_size_;
    double min_spread_;
    double max_spread_;

    // Welford's online algorithm state
    double mean_ = 0.0;
    double m2_ = 0.0;      // Sum of squared differences from mean
    size_t count_ = 0;

    std::deque<double> prices_;
    mutable std::mutex mutex_;

    // Baseline volatility for relative scaling
    double baseline_volatility_ = 0.0;
    bool baseline_set_ = false;

    // Fast window for regime detection
    size_t fast_window_size_;
    double fast_mean_ = 0.0;
    double fast_m2_ = 0.0;
    size_t fast_count_ = 0;
    std::deque<double> fast_prices_;
    double regime_threshold_;
    double regime_spread_mult_;

    // Recompute Welford state from scratch (after window eviction)
    void recompute_welford();
    void recompute_fast_welford();
};

} // namespace MarketMaker

#endif // VOLATILITY_TRACKER_H
