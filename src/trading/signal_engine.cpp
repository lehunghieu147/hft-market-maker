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
    , cooldown_(cfg.cooldown_ms)
    , use_multi_timeframe_(cfg.use_multi_timeframe)
    , fast_ema_(cfg.fast_ema_window)
    , slow_ema_(cfg.slow_ema_window)
    , volume_expansion_threshold_(cfg.volume_expansion_threshold) {}

Signal SignalEngine::on_tick(double best_bid, double best_ask) {
    double mid = (best_bid + best_ask) / 2.0;
    tick_count_++;

    // Always update all EMAs
    ema_.update(mid);
    if (use_multi_timeframe_) {
        fast_ema_.update(mid);
        slow_ema_.update(mid);
    }

    // Check cooldown
    auto now = std::chrono::steady_clock::now();
    if (last_signal_ != Signal::NONE &&
        (now - last_signal_time_) < cooldown_) {
        return Signal::NONE;
    }

    if (use_multi_timeframe_) {
        return check_multi_timeframe_signal(mid, best_bid, best_ask);
    }

    // Original single-EMA logic
    if (!ema_.ready()) return Signal::NONE;

    double ema_val = ema_.value();
    double upper = ema_val * (1.0 + epsilon_);
    double lower = ema_val * (1.0 - epsilon_);

    // Hysteresis: require price to cross back through EMA before reversing
    if (last_signal_ == Signal::BUY) {
        if (mid < ema_val) crossed_zero_ = true;
    } else if (last_signal_ == Signal::SELL) {
        if (mid > ema_val) crossed_zero_ = true;
    }

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

Signal SignalEngine::check_multi_timeframe_signal(double mid, double best_bid, double best_ask) {
    if (!fast_ema_.ready() || !slow_ema_.ready()) return Signal::NONE;

    double fast = fast_ema_.value();
    double slow = slow_ema_.value();
    double vwap_val = vwap_.value();

    // Hysteresis
    if (last_signal_ == Signal::BUY && mid < slow) crossed_zero_ = true;
    if (last_signal_ == Signal::SELL && mid > slow) crossed_zero_ = true;

    if (!crossed_zero_) return Signal::NONE;

    // Volume filter: only signal on expanding volume
    bool vol_ok = is_volume_expanding();

    // BUY: fast EMA > slow EMA AND price > VWAP (or VWAP not ready) AND volume expanding
    bool vwap_ok = !vwap_.ready() || mid > vwap_val;
    if (fast > slow && vwap_ok && vol_ok) {
        // Also check epsilon threshold against slow EMA
        if (best_ask > slow * (1.0 + epsilon_)) {
            auto now = std::chrono::steady_clock::now();
            last_signal_ = Signal::BUY;
            last_signal_time_ = now;
            crossed_zero_ = false;
            LOG_INFO(get_logger(),
                     "[SIGNAL-MT] BUY fast={:.2f} slow={:.2f} vwap={:.2f} mid={:.2f} vol_expand={} tick={}",
                     fast, slow, vwap_val, mid, vol_ok, tick_count_);
            return Signal::BUY;
        }
    }

    // SELL: fast EMA < slow EMA AND price < VWAP AND volume expanding
    vwap_ok = !vwap_.ready() || mid < vwap_val;
    if (fast < slow && vwap_ok && vol_ok) {
        if (best_bid < slow * (1.0 - epsilon_)) {
            auto now = std::chrono::steady_clock::now();
            last_signal_ = Signal::SELL;
            last_signal_time_ = now;
            crossed_zero_ = false;
            LOG_INFO(get_logger(),
                     "[SIGNAL-MT] SELL fast={:.2f} slow={:.2f} vwap={:.2f} mid={:.2f} vol_expand={} tick={}",
                     fast, slow, vwap_val, mid, vol_ok, tick_count_);
            return Signal::SELL;
        }
    }

    return Signal::NONE;
}

void SignalEngine::on_trade(double price, double volume) {
    if (!use_multi_timeframe_) return;

    vwap_.update(price, volume);

    // Rolling volume window
    recent_volumes_.push_back(volume);
    volume_sum_ += volume;
    if (recent_volumes_.size() > VOLUME_WINDOW) {
        volume_sum_ -= recent_volumes_.front();
        recent_volumes_.pop_front();
    }
}

bool SignalEngine::is_volume_expanding() const {
    if (recent_volumes_.size() < VOLUME_WINDOW) return true;  // Not enough data, allow signals
    double avg = volume_sum_ / static_cast<double>(recent_volumes_.size());
    double current = recent_volumes_.back();
    return current >= avg * volume_expansion_threshold_;
}

double SignalEngine::ema_value() const {
    return ema_.value();
}

double SignalEngine::fast_ema_value() const {
    return fast_ema_.value();
}

double SignalEngine::slow_ema_value() const {
    return slow_ema_.value();
}

double SignalEngine::vwap_value() const {
    return vwap_.value();
}

Signal SignalEngine::last_signal() const {
    return last_signal_;
}

int64_t SignalEngine::ticks_processed() const {
    return tick_count_;
}

void SignalEngine::reset() {
    ema_.reset();
    fast_ema_.reset();
    slow_ema_.reset();
    vwap_.reset();
    recent_volumes_.clear();
    volume_sum_ = 0.0;
    last_signal_ = Signal::NONE;
    crossed_zero_ = true;
    tick_count_ = 0;
}

} // namespace MarketMaker
