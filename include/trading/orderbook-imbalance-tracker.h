#ifndef ORDERBOOK_IMBALANCE_TRACKER_H
#define ORDERBOOK_IMBALANCE_TRACKER_H

#include "core/types.h"

namespace MarketMaker {

/// Tracks Order Book Imbalance (OBI) to detect buy/sell pressure.
/// OBI = (bid_vol - ask_vol) / (bid_vol + ask_vol), range [-1, +1]
/// Positive = buy pressure, Negative = sell pressure
class OrderBookImbalanceTracker {
public:
    explicit OrderBookImbalanceTracker(int levels = 5, double ema_alpha = 0.3, double min_volume = 50.0);

    /// Compute raw OBI from orderbook snapshot
    double compute(const OrderBook& book) const;

    /// Update with new orderbook data, returns smoothed OBI
    double update(const OrderBook& book);

    /// Get EMA-smoothed OBI value
    double smoothed_obi() const { return smoothed_obi_; }

    /// Get raw (unsmoothed) OBI from last update
    double raw_obi() const { return raw_obi_; }

    /// Whether last signal is significant (total volume > threshold)
    bool is_significant() const { return is_significant_; }

    void reset();

private:
    int levels_;
    double ema_alpha_;
    double min_volume_;
    double smoothed_obi_ = 0.0;
    double raw_obi_ = 0.0;
    bool is_significant_ = false;
    bool initialized_ = false;
};

} // namespace MarketMaker
#endif
