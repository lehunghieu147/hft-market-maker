#ifndef VWAP_TRACKER_H
#define VWAP_TRACKER_H

#include <cmath>
#include <algorithm>

namespace MarketMaker {

// Rolling VWAP (Volume-Weighted Average Price) tracker.
// Tracks cumulative price*volume / volume with optional standard deviation bands.
class VwapTracker {
public:
    explicit VwapTracker(int max_samples = 10000)
        : max_samples_(max_samples) {}

    // Feed a price+volume observation
    void update(double price, double volume) {
        if (volume <= 0 || !std::isfinite(price)) return;

        // Welford's weighted variance: compute delta from OLD mean before updating
        double old_vwap = (cum_vol_ > 0) ? cum_pv_ / cum_vol_ : price;
        cum_pv_ += price * volume;
        cum_vol_ += volume;
        double new_vwap = cum_pv_ / cum_vol_;
        cum_sq_dev_ += volume * (price - old_vwap) * (price - new_vwap);
        sample_count_++;

        // Prevent unbounded growth: decay old data gradually
        if (sample_count_ > max_samples_) {
            double decay = 0.999;
            cum_pv_ *= decay;
            cum_vol_ *= decay;
            cum_sq_dev_ *= decay;
        }
    }

    // Current VWAP value
    [[nodiscard]] double value() const {
        return cum_vol_ > 0 ? cum_pv_ / cum_vol_ : 0.0;
    }

    // Standard deviation of prices around VWAP
    [[nodiscard]] double stddev() const {
        if (cum_vol_ <= 0) return 0.0;
        double variance = cum_sq_dev_ / cum_vol_;
        return std::sqrt(std::max(0.0, variance));
    }

    // Upper band: VWAP + num_stddev * stddev
    [[nodiscard]] double upper_band(double num_stddev = 2.0) const {
        return value() + num_stddev * stddev();
    }

    // Lower band: VWAP - num_stddev * stddev
    [[nodiscard]] double lower_band(double num_stddev = 2.0) const {
        return value() - num_stddev * stddev();
    }

    [[nodiscard]] bool ready() const { return sample_count_ >= 10; }
    [[nodiscard]] int sample_count() const { return sample_count_; }

    void reset() {
        cum_pv_ = 0.0;
        cum_vol_ = 0.0;
        cum_sq_dev_ = 0.0;
        sample_count_ = 0;
    }

private:
    int max_samples_;
    double cum_pv_ = 0.0;       // Cumulative price * volume
    double cum_vol_ = 0.0;      // Cumulative volume
    double cum_sq_dev_ = 0.0;   // Weighted sum of squared deviations
    int sample_count_ = 0;
};

} // namespace MarketMaker

#endif // VWAP_TRACKER_H
