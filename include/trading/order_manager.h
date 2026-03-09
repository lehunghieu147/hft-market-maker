#ifndef ORDER_MANAGER_H
#define ORDER_MANAGER_H

#include "core/types.h"
#include "core/config.h"
#include "core/object-pool.h"
#include "quill/Logger.h"
#include "exchange/exchange_interface.h"
#include "trading/risk_manager.h"
#include "trading/order_validator.h"
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
    OrderManager(std::shared_ptr<IExchange> exchange, const Config& config,
                 std::shared_ptr<RiskManager> risk_manager = nullptr);
    ~OrderManager();

    // Order management
    [[nodiscard]] bool place_market_maker_orders(double mid_price);
    [[nodiscard]] bool place_market_maker_orders(double mid_price, const std::chrono::steady_clock::time_point& orderbook_time);
    // Place orders with explicit bid/ask prices (used by Avellaneda-Stoikov model)
    [[nodiscard]] bool place_market_maker_orders_with_prices(double mid_price, double bid_price, double ask_price,
                                                             const std::chrono::steady_clock::time_point& orderbook_time);
    // Single taker order (IOC limit or market)
    bool place_taker_order(OrderSide side, double price, double quantity,
                           const std::string& order_type = "ioc",
                           const std::chrono::steady_clock::time_point& orderbook_time = {});

    bool cancel_all_active_orders();
    bool update_orders_if_needed(double new_mid_price);
    bool update_orders_if_needed(double new_mid_price, const std::chrono::steady_clock::time_point& orderbook_time);

    // Fill event from User Data Stream (real fill tracking)
    void on_fill_event(const std::string& order_id,
                       const std::string& client_order_id,
                       OrderSide side,
                       OrderStatus status,
                       double price,
                       double quantity,
                       double cumulative_quantity);

    // Get current orders
    std::pair<std::shared_ptr<Order>, std::shared_ptr<Order>> get_active_orders() const;

    // Metrics
    [[nodiscard]] LatencyMetrics get_metrics() const;
    void reset_metrics();

    // Price formatting
    double format_price(double price) const;
    double format_quantity(double quantity) const;

private:
    std::shared_ptr<IExchange> exchange_;
    Config config_;
    std::shared_ptr<RiskManager> risk_manager_;
    OrderValidator order_validator_;

    // Pre-allocated pool for Order objects (eliminates heap allocation on hot path)
    static constexpr std::size_t ORDER_POOL_SIZE = 128;
    ObjectPool<Order, ORDER_POOL_SIZE> order_pool_;

    // Create shared_ptr with custom deleter that returns to pool
    std::shared_ptr<Order> make_pooled_order(const Order& src);

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
                       const std::chrono::steady_clock::time_point& orderbook_time,
                       bool bid_success, bool ask_success);
    std::string generate_client_order_id(OrderSide side);

    quill::Logger* logger_ = nullptr;
};

} // namespace MarketMaker

#endif // ORDER_MANAGER_H