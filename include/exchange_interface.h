#ifndef EXCHANGE_INTERFACE_H
#define EXCHANGE_INTERFACE_H

#include "types.h"
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <functional>

namespace MarketMaker {

// Forward declaration for exchange-specific configurations
struct ExchangeConfig {
    std::string api_url;
    std::string ws_url;
    std::string api_key;
    std::string api_secret;

    // WebSocket Trading API settings
    std::string ws_trading_url;        // WebSocket API endpoint for trading
    bool use_websocket_trading = false; // Use WebSocket API for orders instead of REST

    // Exchange-specific parameters
    std::string exchange_type;  // "binance", "coinbase", "kraken", etc.
    int price_precision = 2;
    int quantity_precision = 8;

    // Rate limiting
    int max_requests_per_second = 10;
    int max_orders_per_second = 5;

    // Connection settings
    bool use_testnet = false;
    int connection_timeout_ms = 5000;
    int request_timeout_ms = 10000;

    // Asset configuration
    std::vector<std::string> display_assets;  // Assets to display in account info
    std::vector<std::string> supported_quote_currencies;  // For symbol conversion
};

// Abstract interface for all exchanges
class IExchange {
public:
    using MessageHandler = std::function<void(const std::string&)>;
    using ConnectionHandler = std::function<void(bool)>;
    using OrderbookHandler = std::function<void(const OrderBook&)>;

    virtual ~IExchange() = default;

    // ========== Connection Management ==========
    virtual bool initialize(const ExchangeConfig& config) = 0;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // ========== Market Data ==========
    // WebSocket streaming
    virtual bool subscribe_orderbook(const std::string& symbol, int depth = 20) = 0;
    virtual bool subscribe_trades(const std::string& symbol) = 0;
    virtual bool unsubscribe(const std::string& symbol) = 0;

    // REST API queries
    virtual std::optional<OrderBook> get_orderbook(const std::string& symbol, int limit = 20) = 0;
    virtual std::optional<double> get_current_price(const std::string& symbol) = 0;
    virtual std::optional<std::string> get_exchange_info() = 0;

    // ========== Order Management ==========
    virtual std::optional<Order> place_limit_order(
        const std::string& symbol,
        OrderSide side,
        double price,
        double quantity,
        const std::string& client_order_id = ""
    ) = 0;

    virtual std::optional<Order> place_market_order(
        const std::string& symbol,
        OrderSide side,
        double quantity,
        const std::string& client_order_id = ""
    ) = 0;

    virtual std::optional<bool> cancel_order(
        const std::string& symbol,
        const std::string& order_id
    ) = 0;

    virtual std::optional<bool> cancel_all_orders(const std::string& symbol) = 0;

    virtual std::optional<Order> modify_order(
        const std::string& symbol,
        const std::string& order_id,
        double new_price,
        double new_quantity
    ) = 0;

    virtual std::optional<std::vector<Order>> get_open_orders(const std::string& symbol) = 0;
    virtual std::optional<Order> get_order_status(
        const std::string& symbol,
        const std::string& order_id
    ) = 0;

    // ========== Account Information ==========
    virtual std::optional<std::string> get_account_info() = 0;
    virtual std::optional<double> get_balance(const std::string& asset) = 0;

    // ========== Event Handlers ==========
    virtual void set_orderbook_handler(OrderbookHandler handler) = 0;
    virtual void set_message_handler(MessageHandler handler) = 0;
    virtual void set_connection_handler(ConnectionHandler handler) = 0;

    // ========== Utility Methods ==========
    virtual std::string get_exchange_name() const = 0;
    virtual bool supports_websocket_trading() const = 0;
    virtual bool get_symbol_info(
        const std::string& symbol,
        int& price_precision,
        int& quantity_precision
    ) = 0;

    // Format price/quantity according to exchange requirements
    virtual double format_price(double price, const std::string& symbol) = 0;
    virtual double format_quantity(double quantity, const std::string& symbol) = 0;

    // Get exchange-specific limits
    virtual double get_min_order_size(const std::string& symbol) = 0;
    virtual double get_max_order_size(const std::string& symbol) = 0;
    virtual double get_tick_size(const std::string& symbol) = 0;

protected:
    // Protected helper methods that derived classes can use
    ExchangeConfig config_;

    // Common handlers
    OrderbookHandler orderbook_handler_;
    MessageHandler message_handler_;
    ConnectionHandler connection_handler_;
};

// Type alias for convenience
using ExchangePtr = std::shared_ptr<IExchange>;

} // namespace MarketMaker

#endif // EXCHANGE_INTERFACE_H