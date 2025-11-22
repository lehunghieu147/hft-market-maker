#ifndef BINANCE_EXCHANGE_H
#define BINANCE_EXCHANGE_H

#include "exchange_interface.h"
#include "websocket_client.h"
#include "rest_client.h"
#include <memory>
#include <mutex>
#include <atomic>
#include <map>

namespace MarketMaker {

class BinanceExchange : public IExchange {
public:
    BinanceExchange();
    ~BinanceExchange() override;

    // ========== Connection Management ==========
    bool initialize(const ExchangeConfig& config) override;
    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;

    // ========== Market Data ==========
    bool subscribe_orderbook(const std::string& symbol, int depth = 20) override;
    bool subscribe_trades(const std::string& symbol) override;
    bool unsubscribe(const std::string& symbol) override;

    std::optional<OrderBook> get_orderbook(const std::string& symbol, int limit = 20) override;
    std::optional<double> get_current_price(const std::string& symbol) override;
    std::optional<std::string> get_exchange_info() override;

    // ========== Order Management ==========
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

    // ========== Account Information ==========
    std::optional<std::string> get_account_info() override;
    std::optional<double> get_balance(const std::string& asset) override;

    // ========== Event Handlers ==========
    void set_orderbook_handler(OrderbookHandler handler) override;
    void set_message_handler(MessageHandler handler) override;
    void set_connection_handler(ConnectionHandler handler) override;

    // ========== Utility Methods ==========
    std::string get_exchange_name() const override { return "Binance"; }
    bool supports_websocket_trading() const override { return false; } // Binance testnet doesn't support WS trading

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

    // Configuration
    void set_supported_quote_currencies(const std::vector<std::string>& currencies);

private:
    // Binance-specific components
    std::shared_ptr<WebSocketClient> ws_client_;
    std::shared_ptr<RestClient> rest_client_;

    // Symbol info cache
    struct SymbolInfo {
        int price_precision;
        int quantity_precision;
        double min_qty;
        double max_qty;
        double tick_size;
    };
    std::map<std::string, SymbolInfo> symbol_cache_;
    std::mutex cache_mutex_;

    // Connection state
    std::atomic<bool> initialized_{false};
    std::atomic<bool> ws_connected_{false};

    // Current orderbook (from WebSocket)
    OrderBook current_orderbook_;
    std::mutex orderbook_mutex_;

    // Subscribed symbol for resubscription on reconnect
    std::string subscribed_symbol_;
    int subscribed_depth_{20};

    // Configuration
    std::vector<std::string> supported_quote_currencies_ = {"USDT", "BUSD", "ETH", "BNB"};

    // Helper methods
    void handle_websocket_message(const std::string& message);
    void process_binance_orderbook(const std::string& json_str);
    std::string convert_symbol_to_binance(const std::string& symbol);
    std::string convert_symbol_from_binance(const std::string& symbol);

    // Parse Binance responses
    Order parse_order_response(const std::string& json_str);
    bool parse_error_response(const std::string& json_str, std::string& error_msg);

    // Rate limiting
    std::chrono::steady_clock::time_point last_request_time_;
    std::mutex rate_limit_mutex_;
    void enforce_rate_limit();
};

} // namespace MarketMaker

#endif // BINANCE_EXCHANGE_H