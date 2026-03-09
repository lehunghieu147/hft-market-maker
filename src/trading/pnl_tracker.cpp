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

    // Track win/loss
    if (net_pnl >= 0) {
        winning_trades_++;
    } else {
        losing_trades_++;
    }

    LOG_INFO(get_logger(), "[P&L] Trade #{}: gross={:.4f} fee={:.4f} net={:.4f} | Daily: {:.4f} | Total: {:.4f} | W/L: {}/{}",
             winning_trades_ + losing_trades_, gross_pnl, fee, net_pnl, daily_pnl_, realized_pnl_,
             winning_trades_, losing_trades_);
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

long PnLTracker::get_winning_trades() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return winning_trades_;
}

long PnLTracker::get_losing_trades() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return losing_trades_;
}

long PnLTracker::get_total_trades() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return winning_trades_ + losing_trades_;
}

void PnLTracker::on_fill(OrderSide side, double price, double quantity,
                         double position_before, double avg_entry_price, bool is_maker) {
    if (!std::isfinite(price) || !std::isfinite(quantity) || quantity <= 0) {
        return;
    }

    // Determine if this fill reduces the position (closing trade)
    bool is_reducing = (side == OrderSide::SELL && position_before > 0) ||
                       (side == OrderSide::BUY && position_before < 0);

    double fee_rate = is_maker ? maker_fee_rate_ : taker_fee_rate_;
    double fee = price * quantity * fee_rate;

    if (is_reducing && std::abs(avg_entry_price) > 1e-10) {
        // Compute realized P&L on the closing portion
        double close_qty = std::min(quantity, std::abs(position_before));
        double gross_pnl = (position_before > 0)
            ? (price - avg_entry_price) * close_qty   // long close: sell > avg = profit
            : (avg_entry_price - price) * close_qty;  // short close: avg > buy = profit

        double net_pnl = gross_pnl - fee;

        std::lock_guard<std::mutex> lock(mutex_);
        realized_pnl_ += net_pnl;
        daily_pnl_ += net_pnl;
        total_fees_ += fee;

        peak_pnl_ = std::max(peak_pnl_, realized_pnl_);
        double current_drawdown = realized_pnl_ - peak_pnl_;
        max_drawdown_hit_ = std::min(max_drawdown_hit_, current_drawdown);

        if (net_pnl >= 0) { winning_trades_++; } else { losing_trades_++; }

        LOG_INFO(get_logger(),
                 "[P&L] Fill #{}: {} {:.6f} @ {:.2f} (avg_entry={:.2f}) gross={:.4f} fee={:.4f} net={:.4f} | Daily: {:.4f} | Total: {:.4f}",
                 winning_trades_ + losing_trades_,
                 side == OrderSide::BUY ? "BUY" : "SELL", close_qty, price,
                 avg_entry_price, gross_pnl, fee, net_pnl, daily_pnl_, realized_pnl_);
    } else {
        // Position increasing — only track fees
        std::lock_guard<std::mutex> lock(mutex_);
        total_fees_ += fee;
    }
}

double PnLTracker::get_unrealized_pnl(double current_price, double position, double avg_entry_price) const {
    if (std::abs(position) < 1e-10 || !std::isfinite(current_price)) return 0.0;
    return (current_price - avg_entry_price) * position;
}

double PnLTracker::get_total_pnl(double current_price, double position, double avg_entry_price) const {
    std::lock_guard<std::mutex> lock(mutex_);
    double unrealized = (std::abs(position) > 1e-10 && std::isfinite(current_price))
        ? (current_price - avg_entry_price) * position : 0.0;
    return realized_pnl_ + unrealized;
}

void PnLTracker::print_session_summary(double current_price, double position, double avg_entry_price) const {
    std::lock_guard<std::mutex> lock(mutex_);
    double unrealized = (std::abs(position) > 1e-10 && std::isfinite(current_price))
        ? (current_price - avg_entry_price) * position : 0.0;
    double total = realized_pnl_ + unrealized;
    long total_trades = winning_trades_ + losing_trades_;
    double win_rate = total_trades > 0 ? (100.0 * winning_trades_ / total_trades) : 0.0;

    LOG_INFO(get_logger(), "{}", "========== SESSION P&L SUMMARY ==========");
    LOG_INFO(get_logger(), "[P&L] Realized P&L:   {:.4f} USDT", realized_pnl_);
    LOG_INFO(get_logger(), "[P&L] Unrealized P&L: {:.4f} USDT", unrealized);
    LOG_INFO(get_logger(), "[P&L] Total P&L:      {:.4f} USDT", total);
    LOG_INFO(get_logger(), "[P&L] Total Fees:     {:.4f} USDT", total_fees_);
    LOG_INFO(get_logger(), "[P&L] Max Drawdown:   {:.4f} USDT", max_drawdown_hit_);
    LOG_INFO(get_logger(), "[P&L] Trades: {} (W:{} L:{} WR:{:.1f}%)",
             total_trades, winning_trades_, losing_trades_, win_rate);
    LOG_INFO(get_logger(), "[P&L] Position:       {:.6f} @ avg {:.2f}", position, avg_entry_price);
    LOG_INFO(get_logger(), "{}", "=========================================");
}

void PnLTracker::reset_daily() {
    std::lock_guard<std::mutex> lock(mutex_);
    daily_pnl_ = 0.0;
    LOG_INFO(get_logger(), "{}", "[P&L] Daily counters reset");
}

} // namespace MarketMaker
