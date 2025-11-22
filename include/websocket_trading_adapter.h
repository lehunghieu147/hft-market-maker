#ifndef WEBSOCKET_TRADING_ADAPTER_H
#define WEBSOCKET_TRADING_ADAPTER_H

#include "exchange_interface.h"
#include "websocket_trading_client.h"
#include "websocket_client.h"
#include <memory>
#include <string>

namespace MarketMaker {

/**
 * Adapter class that combines WebSocket market data (existing WebSocketClient)
 * with WebSocket trading operations (new WebSocketTradingClient)
 * to provide a complete IExchange implementation using WebSocket APIs
 */
class WebSocketTradingAdapter : public IExchange {
public:
    WebSocketTradingAdapter(
        const std::string& api_key,
        const std::string& api_secret,
        const std::string& ws_market_base_url,
        const std::string& ws_trading_base_url
    );

    ~WebSocketTradingAdapter();

    // Exchange info
    std::string get_exchange_name() const override { return "binance_ws"; }
    bool is_connected() const override;
    bool supports_websocket_trading() const override { return true; }

    // Initialize method (required by interface)
    bool initialize([[maybe_unused]] const ExchangeConfig& config) override {
        // Already initialized in constructor
        return true;
    }

    // Connection management
    bool connect() override;
    void disconnect() override;

    // Market data (using existing WebSocketClient)
    bool subscribe_orderbook(const std::string& symbol, int depth) override;
    bool subscribe_trades(const std::string& symbol) override;
    bool unsubscribe(const std::string& symbol) override;

    std::optional<OrderBook> get_orderbook(const std::string& symbol, int limit = 20) override;
    std::optional<double> get_current_price(const std::string& symbol) override;
    std::optional<std::string> get_exchange_info() override;

    // Order management (using new WebSocketTradingClient)
    std::optional<Order> place_limit_order(
        const std::string& symbol,
        OrderSide side,
        double price,
        double quantity,
        const std::string& client_order_id = ""
    ) override;

    std::optional<Order> place_market_order(
        const std::string& symbol,
        OrderSide side,
        double quantity,
        const std::string& client_order_id = ""
    ) override;

    std::optional<bool> cancel_order(
        const std::string& symbol,
        const std::string& order_id
    ) override;

    std::optional<bool> cancel_all_orders(const std::string& symbol) override;

    std::optional<Order> modify_order(
        const std::string& symbol,
        const std::string& order_id,
        double new_price,
        double new_quantity
    ) override;

    std::optional<std::vector<Order>> get_open_orders(const std::string& symbol) override;

    std::optional<Order> get_order_status(
        const std::string& symbol,
        const std::string& order_id
    ) override;

    // Account info
    std::optional<std::string> get_account_info() override;
    std::optional<double> get_balance(const std::string& asset) override;

    // Event handlers
    void set_orderbook_handler(OrderbookHandler handler) override;
    void set_message_handler(MessageHandler handler) override;
    void set_connection_handler(ConnectionHandler handler) override;

    // Utility methods
    bool get_symbol_info(
        const std::string& symbol,
        int& price_precision,
        int& quantity_precision
    ) override;

    double format_price(double price, const std::string& symbol) override;
    double format_quantity(double quantity, const std::string& symbol) override;
    double get_min_order_size(const std::string& symbol) override;
    double get_max_order_size(const std::string& symbol) override;
    double get_tick_size(const std::string& symbol) override;

    // Performance metrics
    struct CombinedMetrics {
        // From WebSocketTradingClient
        uint64_t total_orders = 0;
        uint64_t successful_orders = 0;
        uint64_t failed_orders = 0;
        double avg_order_latency_ms = 0.0;

        // From WebSocketClient (market data)
        uint64_t messages_received = 0;
        uint64_t reconnect_count = 0;
    };

    CombinedMetrics get_metrics() const;

private:
    // WebSocket clients
    std::shared_ptr<WebSocketClient> ws_market_client_;       // For market data
    std::shared_ptr<WebSocketTradingClient> ws_trading_client_; // For trading

    // Configuration
    std::string api_key_;
    std::string api_secret_;
    std::string ws_market_base_url_;
    std::string ws_trading_base_url_;

    // Symbol info cache
    struct SymbolInfo {
        int price_precision = 2;
        int quantity_precision = 5;
        double min_quantity = 0.00001;
        double max_quantity = 10000000;
        double tick_size = 0.01;
    };
    mutable std::unordered_map<std::string, SymbolInfo> symbol_info_cache_;
    mutable std::mutex cache_mutex_;

    // Current orderbook (updated from market data WebSocket)
    mutable OrderBook current_orderbook_;
    mutable std::mutex orderbook_mutex_;

    // Handlers
    OrderbookHandler orderbook_handler_;
    MessageHandler message_handler_;
    ConnectionHandler connection_handler_;

    // Helper methods
    Order json_to_order(const Json::Value& json_order);
    void handle_market_data_message(const std::string& message);
    void handle_trading_response(const Json::Value& response);
    void update_orderbook_from_message(const Json::Value& data);
};

} // namespace MarketMaker

#endif // WEBSOCKET_TRADING_ADAPTER_H