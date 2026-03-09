#include "trading/orderbook-imbalance-tracker.h"
#include <algorithm>
#include <cmath>

namespace MarketMaker {

OrderBookImbalanceTracker::OrderBookImbalanceTracker(int levels, double ema_alpha, double min_volume)
    : levels_(levels), ema_alpha_(ema_alpha), min_volume_(min_volume) {}

double OrderBookImbalanceTracker::compute(const OrderBook& book) const {
    if (book.bids.empty() || book.asks.empty()) return 0.0;

    double bid_vol = 0.0, ask_vol = 0.0;
    size_t bid_depth = std::min(static_cast<size_t>(levels_), book.bids.size());
    size_t ask_depth = std::min(static_cast<size_t>(levels_), book.asks.size());

    for (size_t i = 0; i < bid_depth; ++i) bid_vol += book.bids[i].quantity;
    for (size_t i = 0; i < ask_depth; ++i) ask_vol += book.asks[i].quantity;

    double total = bid_vol + ask_vol;
    if (total <= 0.0) return 0.0;

    return (bid_vol - ask_vol) / total;
}

double OrderBookImbalanceTracker::update(const OrderBook& book) {
    raw_obi_ = compute(book);

    // Check volume significance
    double total_vol = 0.0;
    size_t bid_depth = std::min(static_cast<size_t>(levels_), book.bids.size());
    size_t ask_depth = std::min(static_cast<size_t>(levels_), book.asks.size());
    for (size_t i = 0; i < bid_depth; ++i) total_vol += book.bids[i].quantity;
    for (size_t i = 0; i < ask_depth; ++i) total_vol += book.asks[i].quantity;
    is_significant_ = total_vol >= min_volume_;

    // EMA smoothing
    if (!initialized_) {
        smoothed_obi_ = raw_obi_;
        initialized_ = true;
    } else {
        smoothed_obi_ = ema_alpha_ * raw_obi_ + (1.0 - ema_alpha_) * smoothed_obi_;
    }

    return smoothed_obi_;
}

void OrderBookImbalanceTracker::reset() {
    smoothed_obi_ = 0.0;
    raw_obi_ = 0.0;
    is_significant_ = false;
    initialized_ = false;
}

} // namespace MarketMaker
