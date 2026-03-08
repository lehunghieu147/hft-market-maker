#include "trading/risk_manager.h"
#include <iostream>

namespace MarketMaker {

RiskManager::RiskManager(const RiskConfig& config)
    : config_(config),
      position_tracker_(config.max_position_size),
      pnl_tracker_(config.max_daily_loss, config.max_drawdown,
                   config.maker_fee_rate, config.taker_fee_rate) {}

bool RiskManager::should_trade() const {
    if (kill_switch_.load()) {
        std::cerr << "[RISK] Kill switch is active - trading halted" << std::endl;
        return false;
    }

    if (consecutive_errors_.load() >= config_.max_consecutive_errors) {
        std::cerr << "[RISK] Too many consecutive errors ("
                  << consecutive_errors_.load() << ") - trading halted" << std::endl;
        return false;
    }

    if (!pnl_tracker_.is_within_limits()) {
        return false;
    }

    return true;
}

void RiskManager::activate_kill_switch(const std::string& reason) {
    kill_switch_.store(true);
    std::cerr << "[RISK] KILL SWITCH ACTIVATED: " << reason << std::endl;
}

void RiskManager::deactivate_kill_switch() {
    kill_switch_.store(false);
    consecutive_errors_.store(0);
    std::cout << "[RISK] Kill switch deactivated" << std::endl;
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

} // namespace MarketMaker
