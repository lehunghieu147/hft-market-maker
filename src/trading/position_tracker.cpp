#include "trading/position_tracker.h"
#include "core/app_logger.h"
#include <cmath>

namespace {
    quill::Logger* get_logger() {
        static quill::Logger* logger = MarketMaker::AppLogger::get("risk");
        return logger;
    }
}

namespace MarketMaker {

PositionTracker::PositionTracker(double max_position_size)
    : max_position_size_(max_position_size)
    , max_long_position_(max_position_size)
    , max_short_position_(max_position_size) {}

void PositionTracker::set_asymmetric_limits(double max_long, double max_short) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_long_position_ = max_long;
    max_short_position_ = max_short;
}

bool PositionTracker::can_place_order(OrderSide side, double quantity) const {
    std::lock_guard<std::mutex> lock(mutex_);

    double projected = current_position_ + (side == OrderSide::BUY ? quantity : -quantity);

    // Asymmetric limits: check long and short separately
    if (projected > max_long_position_) {
        LOG_WARNING(get_logger(), "[RISK] Long limit would be exceeded: {:.6f} > {:.6f}", projected, max_long_position_);
        return false;
    }
    if (projected < -max_short_position_) {
        LOG_WARNING(get_logger(), "[RISK] Short limit would be exceeded: {:.6f} < -{:.6f}", projected, max_short_position_);
        return false;
    }
    return true;
}

bool PositionTracker::can_place_pair(double buy_qty, double sell_qty) const {
    std::lock_guard<std::mutex> lock(mutex_);
    double after_buy = current_position_ + buy_qty;
    double after_sell = current_position_ - sell_qty;
    return after_buy <= max_long_position_ && after_sell >= -max_short_position_;
}

void PositionTracker::on_fill(OrderSide side, double price, double quantity) {
    if (!std::isfinite(price) || !std::isfinite(quantity) || price <= 0 || quantity <= 0) {
        LOG_WARNING(get_logger(), "{}", "[POSITION] Invalid fill data - skipping");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (side == OrderSide::BUY) {
        entry_value_ += price * quantity;
        current_position_ += quantity;
    } else {
        entry_value_ -= price * quantity;
        current_position_ -= quantity;
    }

    LOG_INFO(get_logger(), "[POSITION] {} fill: qty={} @ {} | Net position: {}",
             (side == OrderSide::BUY ? "BUY" : "SELL"), quantity, price, current_position_);
}

double PositionTracker::get_position() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_position_;
}

double PositionTracker::get_entry_value() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entry_value_;
}

double PositionTracker::get_average_entry_price() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::abs(current_position_) < 1e-10) return 0.0;
    return std::abs(entry_value_ / current_position_);
}

void PositionTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    current_position_ = 0.0;
    entry_value_ = 0.0;
}

} // namespace MarketMaker
