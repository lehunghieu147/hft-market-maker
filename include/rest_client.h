#ifndef REST_CLIENT_H
#define REST_CLIENT_H

#include "types.h"
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <future>

namespace MarketMaker {

class RestClient {
public:
    RestClient(const std::string& base_url, const std::string& api_key, const std::string& api_secret);
    ~RestClient();

    // Set display assets for account info filtering
    void set_display_assets(const std::vector<std::string>& assets);

    // Account endpoints
    std::string get_account_info();
    std::optional<std::vector<Order>> get_open_orders(const std::string& symbol);

    // Order management
    std::optional<Order> place_limit_order(
        const std::string& symbol,
        OrderSide side,
        double price,
        double quantity,
        const std::string& client_order_id = ""
    );

    std::optional<bool> cancel_order(
        const std::string& symbol,
        const std::string& order_id
    );

    std::optional<bool> cancel_all_orders(const std::string& symbol);

    // Order modification (faster than cancel + new)
    std::optional<Order> modify_order(
        const std::string& symbol,
        const std::string& order_id,
        double new_price,
        double new_quantity
    );

    // Parallel order modification - cancel and place simultaneously
    std::optional<Order> modify_order_parallel(
        const std::string& symbol,
        const std::string& order_id,
        OrderSide side,
        double new_price,
        double new_quantity,
        const std::string& client_order_id = ""
    );

    // Async methods for parallel execution
    std::future<std::optional<bool>> cancel_order_async(
        const std::string& symbol,
        const std::string& order_id
    );

    std::future<std::optional<Order>> place_limit_order_async(
        const std::string& symbol,
        OrderSide side,
        double price,
        double quantity,
        const std::string& client_order_id = ""
    );

    // Market data
    std::optional<OrderBook> get_orderbook(const std::string& symbol, int limit = 20);
    std::optional<double> get_current_price(const std::string& symbol);

    // Exchange info
    std::optional<std::string> get_exchange_info();
    bool get_symbol_info(const std::string& symbol, int& price_precision, int& quantity_precision);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;

    std::string generate_signature(const std::string& query_string);
    std::string build_query_string(const std::vector<std::pair<std::string, std::string>>& params);

    // Connection pool for persistent connections
    void init_connection_pool(size_t pool_size = 2);

    // HTTP request helpers
    std::optional<std::string> send_signed_request(
        const std::string& method,
        const std::string& endpoint,
        const std::vector<std::pair<std::string, std::string>>& params = {}
    );

    std::optional<std::string> send_public_request(
        const std::string& endpoint,
        const std::vector<std::pair<std::string, std::string>>& params = {}
    );
};

} // namespace MarketMaker

#endif // REST_CLIENT_H