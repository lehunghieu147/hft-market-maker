#include "trading/signal_engine.h"
#include "core/app_logger.h"

namespace {
    quill::Logger* get_logger() {
        static quill::Logger* logger = MarketMaker::AppLogger::get("trading");
        return logger;
    }
}

namespace MarketMaker {

SignalEngine::SignalEngine(const MomentumConfig& cfg)
    : ema_(cfg.ema_window)
    , epsilon_(cfg.epsilon)
    , cooldown_(cfg.cooldown_ms) {}

Signal SignalEngine::on_tick(double best_bid, double best_ask) {
    double mid = (best_bid + best_ask) / 2.0;
    ema_.update(mid);
    tick_count_++;

    if (!ema_.ready()) return Signal::NONE;

    // Check cooldown
    auto now = std::chrono::steady_clock::now();
    if (last_signal_ != Signal::NONE &&
        (now - last_signal_time_) < cooldown_) {
        return Signal::NONE;
    }

    double ema_val = ema_.value();
    double upper = ema_val * (1.0 + epsilon_);
    double lower = ema_val * (1.0 - epsilon_);

    // Hysteresis: require price to cross back through EMA before reversing
    if (last_signal_ == Signal::BUY) {
        if (mid < ema_val) {
            crossed_zero_ = true;
        }
    } else if (last_signal_ == Signal::SELL) {
        if (mid > ema_val) {
            crossed_zero_ = true;
        }
    }

    // Signal detection
    if (best_ask > upper && crossed_zero_) {
        last_signal_ = Signal::BUY;
        last_signal_time_ = now;
        crossed_zero_ = false;
        LOG_INFO(get_logger(), "[SIGNAL] BUY ema={:.2f} mid={:.2f} ask={:.2f} tick={}",
                 ema_val, mid, best_ask, tick_count_);
        return Signal::BUY;
    }

    if (best_bid < lower && crossed_zero_) {
        last_signal_ = Signal::SELL;
        last_signal_time_ = now;
        crossed_zero_ = false;
        LOG_INFO(get_logger(), "[SIGNAL] SELL ema={:.2f} mid={:.2f} bid={:.2f} tick={}",
                 ema_val, mid, best_bid, tick_count_);
        return Signal::SELL;
    }

    return Signal::NONE;
}

double SignalEngine::ema_value() const {
    return ema_.value();
}

Signal SignalEngine::last_signal() const {
    return last_signal_;
}

int64_t SignalEngine::ticks_processed() const {
    return tick_count_;
}

void SignalEngine::reset() {
    ema_.reset();
    last_signal_ = Signal::NONE;
    crossed_zero_ = true;
    tick_count_ = 0;
}

} // namespace MarketMaker
