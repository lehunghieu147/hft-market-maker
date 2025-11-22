#ifndef ORDER_MANAGER_H
#define ORDER_MANAGER_H

#include "types.h"
#include "config.h"
#include "exchange_interface.h"
#include <memory>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <chrono>
#include <future>
#include <thread>

namespace MarketMaker {

class OrderManager {
public:
    OrderManager(std::shared_ptr<IExchange> exchange, const Config& config);
    ~OrderManager();

    // Order management
    bool place_market_maker_orders(double mid_price);
    bool place_market_maker_orders(double mid_price, const std::chrono::steady_clock::time_point& orderbook_time);
    bool cancel_all_active_orders();
    bool update_orders_if_needed(double new_mid_price);
    bool update_orders_if_needed(double new_mid_price, const std::chrono::steady_clock::time_point& orderbook_time);

    // Get current orders
    std::pair<std::shared_ptr<Order>, std::shared_ptr<Order>> get_active_orders() const;

    // Metrics
    LatencyMetrics get_metrics() const;
    void reset_metrics();

    // Price formatting
    double format_price(double price) const;
    double format_quantity(double quantity) const;

private:
    std::shared_ptr<IExchange> exchange_;
    Config config_;

    mutable std::mutex orders_mutex_;
    std::shared_ptr<Order> active_bid_order_;
    std::shared_ptr<Order> active_ask_order_;

    std::atomic<double> last_mid_price_{0.0};
    std::chrono::steady_clock::time_point last_order_update_;

    LatencyMetrics metrics_;
    mutable std::mutex metrics_mutex_;

    // Helper methods
    bool place_order(OrderSide side, double price, double quantity);
    bool cancel_order(const std::shared_ptr<Order>& order);
    bool should_update_orders(double new_mid_price) const;
    void update_metrics(const std::chrono::steady_clock::time_point& start_time,
                       const std::chrono::steady_clock::time_point& orderbook_time);
    std::string generate_client_order_id(OrderSide side);
};

} // namespace MarketMaker

#endif // ORDER_MANAGER_H