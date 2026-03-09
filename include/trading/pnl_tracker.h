#ifndef PNL_TRACKER_H
#define PNL_TRACKER_H

#include "core/types.h"
#include <mutex>
#include <chrono>

namespace MarketMaker {

class PnLTracker {
public:
    PnLTracker(double max_daily_loss = -100.0,
               double max_drawdown = -500.0,
               double maker_fee_rate = -0.0001,
               double taker_fee_rate = 0.001);

    // Record a completed trade (buy+sell pair or partial)
    void on_trade(double entry_price, double exit_price, double quantity, bool is_maker);

    // Record a fill event — computes realized P&L when position reduces
    // position_before: net position before this fill, avg_entry_price: average cost basis
    void on_fill(OrderSide side, double price, double quantity,
                 double position_before, double avg_entry_price, bool is_maker);

    // Unrealized P&L on open position (mark-to-market)
    double get_unrealized_pnl(double current_price, double position, double avg_entry_price) const;

    // Total P&L = realized + unrealized
    double get_total_pnl(double current_price, double position, double avg_entry_price) const;

    // Print session summary on shutdown
    void print_session_summary(double current_price, double position, double avg_entry_price) const;

    // Check if within daily loss limits
    bool is_within_limits() const;

    // Getters
    double get_realized_pnl() const;
    double get_daily_pnl() const;
    double get_total_fees() const;
    double get_max_drawdown_hit() const;
    long get_winning_trades() const;
    long get_losing_trades() const;
    long get_total_trades() const;

    // Reset daily counters (call at start of each trading day)
    void reset_daily();

private:
    mutable std::mutex mutex_;

    double realized_pnl_ = 0.0;
    double daily_pnl_ = 0.0;
    double total_fees_ = 0.0;
    double peak_pnl_ = 0.0;           // For drawdown calculation
    double max_drawdown_hit_ = 0.0;   // Worst drawdown observed
    long winning_trades_ = 0;
    long losing_trades_ = 0;

    // Limits
    double max_daily_loss_;
    double max_drawdown_;
    double maker_fee_rate_;
    double taker_fee_rate_;
};

} // namespace MarketMaker

#endif // PNL_TRACKER_H
