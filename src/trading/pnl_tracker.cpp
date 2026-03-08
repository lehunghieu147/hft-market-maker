#include "trading/pnl_tracker.h"
#include <cmath>
#include <iostream>
#include <algorithm>

namespace MarketMaker {

PnLTracker::PnLTracker(double max_daily_loss, double max_drawdown,
                       double maker_fee_rate, double taker_fee_rate)
    : max_daily_loss_(max_daily_loss),
      max_drawdown_(max_drawdown),
      maker_fee_rate_(maker_fee_rate),
      taker_fee_rate_(taker_fee_rate) {}

void PnLTracker::on_trade(double entry_price, double exit_price, double quantity, bool is_maker) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!std::isfinite(entry_price) || !std::isfinite(exit_price) ||
        !std::isfinite(quantity) || quantity <= 0) {
        std::cerr << "[P&L] Invalid trade data - skipping" << std::endl;
        return;
    }

    double gross_pnl = (exit_price - entry_price) * quantity;
    double fee_rate = is_maker ? maker_fee_rate_ : taker_fee_rate_;
    // Signed fees: negative rate = rebate (reduces cost), positive = expense
    double fee = entry_price * quantity * fee_rate + exit_price * quantity * fee_rate;
    double net_pnl = gross_pnl - fee;

    realized_pnl_ += net_pnl;
    daily_pnl_ += net_pnl;
    total_fees_ += fee;

    // Track drawdown
    peak_pnl_ = std::max(peak_pnl_, realized_pnl_);
    double current_drawdown = realized_pnl_ - peak_pnl_;
    max_drawdown_hit_ = std::min(max_drawdown_hit_, current_drawdown);

    std::cout << "[P&L] Trade: gross=" << gross_pnl << " fee=" << fee
              << " net=" << net_pnl << " | Daily: " << daily_pnl_
              << " | Total: " << realized_pnl_ << std::endl;
}

bool PnLTracker::is_within_limits() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (daily_pnl_ < max_daily_loss_) {
        std::cerr << "[RISK] Daily loss limit breached: " << daily_pnl_
                  << " < " << max_daily_loss_ << std::endl;
        return false;
    }

    double current_drawdown = realized_pnl_ - peak_pnl_;
    if (current_drawdown < max_drawdown_) {
        std::cerr << "[RISK] Max drawdown breached: " << current_drawdown
                  << " < " << max_drawdown_ << std::endl;
        return false;
    }

    return true;
}

double PnLTracker::get_realized_pnl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return realized_pnl_;
}

double PnLTracker::get_daily_pnl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return daily_pnl_;
}

double PnLTracker::get_total_fees() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_fees_;
}

double PnLTracker::get_max_drawdown_hit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_drawdown_hit_;
}

void PnLTracker::reset_daily() {
    std::lock_guard<std::mutex> lock(mutex_);
    daily_pnl_ = 0.0;
    std::cout << "[P&L] Daily counters reset" << std::endl;
}

} // namespace MarketMaker
