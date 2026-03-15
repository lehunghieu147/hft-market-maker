#ifndef TRADE_FLOW_TRACKER_H
#define TRADE_FLOW_TRACKER_H

#include <deque>
#include <mutex>
#include <cmath>

namespace MarketMaker {

// Tracks trade-flow imbalance to detect toxic/informed flow.
// Thread-safe: on_fill() called from WS thread, get_spread_multiplier() from trading thread.
class TradeFlowTracker {
public:
    explicit TradeFlowTracker(size_t window_size = 50,
                               double imbalance_threshold = 0.7,
                               double spread_multiplier = 1.5)
        : window_size_(window_size)
        , imbalance_threshold_(imbalance_threshold)
        , spread_multiplier_(spread_multiplier) {}

    void on_fill(bool is_buy) {
        std::lock_guard<std::mutex> lock(mutex_);
        fills_.push_back(is_buy);
        if (fills_.size() > window_size_) {
            fills_.pop_front();
        }
    }

    double get_spread_multiplier() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fills_.size() < window_size_ / 2) return 1.0;
        size_t buys = 0;
        for (bool b : fills_) { if (b) ++buys; }
        double buy_ratio = static_cast<double>(buys) / fills_.size();
        double imbalance = std::max(buy_ratio, 1.0 - buy_ratio);
        if (imbalance >= imbalance_threshold_) {
            double t = (imbalance - imbalance_threshold_) / (1.0 - imbalance_threshold_);
            return 1.0 + t * (spread_multiplier_ - 1.0);
        }
        return 1.0;
    }

    bool is_toxic() const { return get_spread_multiplier() > 1.0; }

private:
    mutable std::mutex mutex_;
    std::deque<bool> fills_;
    size_t window_size_;
    double imbalance_threshold_;
    double spread_multiplier_;
};

} // namespace MarketMaker
#endif // TRADE_FLOW_TRACKER_H
