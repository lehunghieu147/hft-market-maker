#ifndef MARKET_MAKER_V2_H
#define MARKET_MAKER_V2_H

#include "config.h"
#include "types.h"
#include "exchange_interface.h"
#include "order_manager.h"
#include "logger.h"
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace MarketMaker {

// Refactored Market Maker Bot that works with any exchange
class MarketMakerBotV2 {
public:
    explicit MarketMakerBotV2(const Config& config);
    ~MarketMakerBotV2();

    // Main control methods
    bool initialize();
    void run();
    void stop();

    // Status
    bool is_running() const { return running_; }
    LatencyMetrics get_metrics() const;

private:
    Config config_;

    // Core components - now using exchange interface
    std::shared_ptr<IExchange> exchange_;  // Generic exchange interface
    std::shared_ptr<OrderManager> order_manager_;
    std::shared_ptr<Logger> logger_;

    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};

    // Market data
    OrderBook current_orderbook_;
    std::mutex orderbook_mutex_;
    std::atomic<double> current_mid_price_{0.0};
    std::chrono::steady_clock::time_point last_orderbook_time_;
    std::atomic<bool> price_changed_{false};
    std::condition_variable price_change_cv_;
    std::mutex price_change_mutex_;

    // Threads
    std::thread main_thread_;

    // Event handlers
    void handle_orderbook_update(const OrderBook& orderbook);
    void handle_connection_status(bool connected);

    // Core logic
    void main_loop();
    void update_mid_price();
    void check_and_update_orders();

    // Utilities
    bool validate_config();
    bool setup_exchange();
    void print_status();
    std::string format_symbol_for_exchange();
};

} // namespace MarketMaker

#endif // MARKET_MAKER_V2_H