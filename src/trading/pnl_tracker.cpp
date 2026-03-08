#include "trading/pnl_tracker.h"
#include "core/app_logger.h"
#include <cmath>
#include <algorithm>

namespace {
    quill::Logger* get_logger() {
        static quill::Logger* logger = MarketMaker::AppLogger::get("risk");
        return logger;
    }
}

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
        LOG_WARNING(get_logger(), "{}", "[P&L] Invalid trade data - skipping");
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

    LOG_INFO(get_logger(), "[P&L] Trade: gross={:.4f} fee={:.4f} net={:.4f} | Daily: {:.4f} | Total: {:.4f}",
             gross_pnl, fee, net_pnl, daily_pnl_, realized_pnl_);
}

bool PnLTracker::is_within_limits() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (daily_pnl_ < max_daily_loss_) {
        LOG_ERROR(get_logger(), "[RISK] Daily loss limit breached: {} < {}", daily_pnl_, max_daily_loss_);
        return false;
    }

    double current_drawdown = realized_pnl_ - peak_pnl_;
    if (current_drawdown < max_drawdown_) {
        LOG_ERROR(get_logger(), "[RISK] Max drawdown breached: {} < {}", current_drawdown, max_drawdown_);
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
    LOG_INFO(get_logger(), "{}", "[P&L] Daily counters reset");
}

} // namespace MarketMaker
