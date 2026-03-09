#ifndef SIGNAL_ENGINE_H
#define SIGNAL_ENGINE_H

#include "trading/ema_engine.h"
#include "core/config.h"
#include <chrono>
#include <cstdint>

namespace MarketMaker {

enum class Signal { NONE, BUY, SELL };

class SignalEngine {
public:
    explicit SignalEngine(const MomentumConfig& cfg);

    // Feed L2 top-of-book. Returns signal.
    [[nodiscard]] Signal on_tick(double best_bid, double best_ask);

    // Accessors
    [[nodiscard]] double ema_value() const;
    [[nodiscard]] Signal last_signal() const;
    [[nodiscard]] int64_t ticks_processed() const;

    void reset();

private:
    EmaEngine ema_;
    double epsilon_;
    std::chrono::milliseconds cooldown_;
    std::chrono::steady_clock::time_point last_signal_time_;
    Signal last_signal_ = Signal::NONE;
    bool crossed_zero_ = true;  // hysteresis: start allowing signals
    int64_t tick_count_ = 0;
};

} // namespace MarketMaker
#endif // SIGNAL_ENGINE_H
