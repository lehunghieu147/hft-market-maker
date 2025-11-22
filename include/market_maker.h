#ifndef MARKET_MAKER_H
#define MARKET_MAKER_H

#include "config.h"
#include "types.h"
#include "websocket_client.h"
#include "rest_client.h"
#include "order_manager.h"
#include "logger.h"
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>

// Forward declaration
namespace Json {
    class Value;
}

namespace MarketMaker {

class MarketMakerBot {
public:
    explicit MarketMakerBot(const Config& config);
    ~MarketMakerBot();

    // Main control methods
    bool initialize();
    void run();
    void stop();

    // Status
    bool is_running() const { return running_; }
    LatencyMetrics get_metrics() const;

private:
    Config config_;

    // Core components
    std::shared_ptr<WebSocketClient> ws_client_;
    std::shared_ptr<RestClient> rest_client_;
    std::shared_ptr<OrderManager> order_manager_;
    std::shared_ptr<Logger> logger_;

    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};

    // Market data
    OrderBook current_orderbook_;
    std::mutex orderbook_mutex_;
    std::atomic<double> current_mid_price_{0.0};

    // Threads
    std::thread main_thread_;

    // WebSocket message handlers
    void handle_websocket_message(const std::string& message);
    void handle_connection_status(bool connected);
    void process_orderbook_update(const Json::Value& data);

    // Core logic
    void main_loop();
    void update_mid_price();
    void check_and_update_orders();

    // Utilities
    bool validate_config();
    bool fetch_symbol_info();
    void print_status();
};

} // namespace MarketMaker

#endif // MARKET_MAKER_H