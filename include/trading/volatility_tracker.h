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
                                double max_spread = 0.05);

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

    // Recompute Welford state from scratch (after window eviction)
    void recompute_welford();
};

} // namespace MarketMaker

#endif // VOLATILITY_TRACKER_H
