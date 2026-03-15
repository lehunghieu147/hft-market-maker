#ifndef POSITION_TRACKER_H
#define POSITION_TRACKER_H

#include "core/types.h"
#include <mutex>
#include <atomic>

namespace MarketMaker {

class PositionTracker {
public:
    explicit PositionTracker(double max_position_size = 1.0);

    // Pre-trade check: can we place this order without exceeding limits?
    bool can_place_order(OrderSide side, double quantity) const;

    // Atomic check for a market maker pair (buy + sell) under single lock
    bool can_place_pair(double buy_qty, double sell_qty) const;

    // Called after a fill is confirmed
    void on_fill(OrderSide side, double price, double quantity);

    // Set asymmetric limits (0 = use symmetric max_position_size for that side)
    void set_asymmetric_limits(double max_long, double max_short);

    // Getters
    double get_position() const;
    double get_entry_value() const;
    double get_average_entry_price() const;
    double get_max_position_size() const { return max_position_size_; }

    // Reset (e.g., on reconciliation)
    void reset();

private:
    mutable std::mutex mutex_;
    double current_position_ = 0.0;   // Net base asset (positive = long, negative = short)
    double entry_value_ = 0.0;        // Total cost basis
    double max_position_size_;         // Max absolute position allowed
    double max_long_position_;         // Max long (defaults to max_position_size_)
    double max_short_position_;        // Max short (defaults to max_position_size_)
};

} // namespace MarketMaker

#endif // POSITION_TRACKER_H
