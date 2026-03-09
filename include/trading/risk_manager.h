#ifndef RISK_MANAGER_H
#define RISK_MANAGER_H

#include "trading/position_tracker.h"
#include "trading/pnl_tracker.h"
#include <atomic>
#include <mutex>

namespace MarketMaker {

struct RiskConfig {
    double max_daily_loss = -100.0;
    double max_position_size = 0.5;
    double max_drawdown = -500.0;
    int max_consecutive_errors = 5;
    double maker_fee_rate = -0.0001;
    double taker_fee_rate = 0.001;
};

class RiskManager {
public:
    explicit RiskManager(const RiskConfig& config);

    // Pre-trade gate: checks all risk conditions
    [[nodiscard]] bool should_trade() const;

    // Emergency stop
    void activate_kill_switch(const std::string& reason);
    void deactivate_kill_switch();
    [[nodiscard]] bool is_kill_switch_active() const { return kill_switch_.load(); }

    // Error tracking
    void on_error();
    void on_success();

    // Access sub-trackers
    PositionTracker& position_tracker() { return position_tracker_; }
    PnLTracker& pnl_tracker() { return pnl_tracker_; }
    const PositionTracker& position_tracker() const { return position_tracker_; }
    const PnLTracker& pnl_tracker() const { return pnl_tracker_; }

    // Get config
    const RiskConfig& get_config() const { return config_; }

private:
    RiskConfig config_;
    PositionTracker position_tracker_;
    PnLTracker pnl_tracker_;

    std::atomic<bool> kill_switch_{false};
    std::atomic<int> consecutive_errors_{0};
};

} // namespace MarketMaker

#endif // RISK_MANAGER_H
