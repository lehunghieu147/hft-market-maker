#include "trading/risk_manager.h"
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

RiskManager::RiskManager(const RiskConfig& config)
    : config_(config),
      position_tracker_(config.max_position_size),
      pnl_tracker_(config.max_daily_loss, config.max_drawdown,
                   config.maker_fee_rate, config.taker_fee_rate) {}

bool RiskManager::should_trade() const {
    if (kill_switch_.load()) {
        LOG_ERROR(get_logger(), "{}", "[RISK] Kill switch is active - trading halted");
        return false;
    }

    if (consecutive_errors_.load() >= config_.max_consecutive_errors) {
        LOG_ERROR(get_logger(), "[RISK] Too many consecutive errors ({}) - trading halted",
                  consecutive_errors_.load());
        return false;
    }

    if (!pnl_tracker_.is_within_limits()) {
        return false;
    }

    return true;
}

void RiskManager::activate_kill_switch(const std::string& reason) {
    kill_switch_.store(true);
    LOG_CRITICAL(get_logger(), "[RISK] KILL SWITCH ACTIVATED: {}", reason);
}

void RiskManager::deactivate_kill_switch() {
    kill_switch_.store(false);
    consecutive_errors_.store(0);
    LOG_INFO(get_logger(), "{}", "[RISK] Kill switch deactivated");
}

void RiskManager::on_error() {
    int errors = consecutive_errors_.fetch_add(1) + 1;
    if (errors >= config_.max_consecutive_errors) {
        activate_kill_switch("Max consecutive errors reached (" + std::to_string(errors) + ")");
    }
}

void RiskManager::on_success() {
    consecutive_errors_.store(0);
}

double RiskManager::adjusted_order_size(double base_size, double exponent,
                                         double min_mult, double max_mult) const {
    if (!volatility_tracker_) return base_size;

    double vol_ratio = volatility_tracker_->get_volatility_ratio();
    if (vol_ratio <= 0.0) return base_size;

    // Inverse relationship: high vol -> smaller size
    double multiplier = std::pow(1.0 / vol_ratio, exponent);
    multiplier = std::clamp(multiplier, min_mult, max_mult);
    return base_size * multiplier;
}

double RiskManager::adjusted_position_limit(double base_limit) const {
    if (!volatility_tracker_) return base_limit;

    double vol_ratio = volatility_tracker_->get_volatility_ratio();
    if (vol_ratio <= 0.0) return base_limit;

    // Inverse relationship: high vol -> tighter limit, clamped to [0.5x, 1.5x]
    double multiplier = std::clamp(1.0 / vol_ratio, 0.5, 1.5);
    return base_limit * multiplier;
}

} // namespace MarketMaker
