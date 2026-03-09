#ifndef SIGNAL_ENGINE_H
#define SIGNAL_ENGINE_H

#include "trading/ema_engine.h"
#include "trading/vwap-tracker.h"
#include "core/config.h"
#include <chrono>
#include <cstdint>
#include <deque>

namespace MarketMaker {

enum class Signal { NONE, BUY, SELL };

class SignalEngine {
public:
    explicit SignalEngine(const MomentumConfig& cfg);

    // Feed L2 top-of-book. Returns signal.
    [[nodiscard]] Signal on_tick(double best_bid, double best_ask);

    // Feed trade data for VWAP and volume tracking (multi-timeframe mode)
    void on_trade(double price, double volume);

    // Accessors
    [[nodiscard]] double ema_value() const;
    [[nodiscard]] double fast_ema_value() const;
    [[nodiscard]] double slow_ema_value() const;
    [[nodiscard]] double vwap_value() const;
    [[nodiscard]] Signal last_signal() const;
    [[nodiscard]] int64_t ticks_processed() const;
    [[nodiscard]] bool is_multi_timeframe() const { return use_multi_timeframe_; }

    void reset();

private:
    // Single-EMA mode (original)
    EmaEngine ema_;
    double epsilon_;
    std::chrono::milliseconds cooldown_;
    std::chrono::steady_clock::time_point last_signal_time_;
    Signal last_signal_ = Signal::NONE;
    bool crossed_zero_ = true;
    int64_t tick_count_ = 0;

    // Multi-timeframe mode
    bool use_multi_timeframe_ = false;
    EmaEngine fast_ema_;                // Fast EMA (e.g. 8-period)
    EmaEngine slow_ema_;                // Slow EMA (e.g. 50-period)
    VwapTracker vwap_;
    double volume_expansion_threshold_ = 1.2;

    // Rolling volume tracker (10-tick window)
    std::deque<double> recent_volumes_;
    static constexpr size_t VOLUME_WINDOW = 10;
    double volume_sum_ = 0.0;

    bool is_volume_expanding() const;

    // Multi-timeframe signal logic
    [[nodiscard]] Signal check_multi_timeframe_signal(double mid, double best_bid, double best_ask);
};

} // namespace MarketMaker
#endif // SIGNAL_ENGINE_H
