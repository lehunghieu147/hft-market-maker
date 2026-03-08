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
    : max_position_size_(max_position_size) {}

bool PositionTracker::can_place_order(OrderSide side, double quantity) const {
    std::lock_guard<std::mutex> lock(mutex_);

    double projected = current_position_;
    if (side == OrderSide::BUY) {
        projected += quantity;
    } else {
        projected -= quantity;
    }

    if (std::abs(projected) > max_position_size_) {
        LOG_WARNING(get_logger(), "[RISK] Position limit would be exceeded: {} > {}", std::abs(projected), max_position_size_);
        return false;
    }
    return true;
}

bool PositionTracker::can_place_pair(double buy_qty, double sell_qty) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::abs(current_position_ + buy_qty) <= max_position_size_ &&
           std::abs(current_position_ - sell_qty) <= max_position_size_;
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
