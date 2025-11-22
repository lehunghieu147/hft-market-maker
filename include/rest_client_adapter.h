#ifndef REST_CLIENT_ADAPTER_H
#define REST_CLIENT_ADAPTER_H

#include "exchange_interface.h"
#include "rest_client.h"
#include <memory>

namespace MarketMaker {

// Adapter to wrap RestClient as IExchange for backward compatibility with V1
class RestClientAdapter : public IExchange {
public:
    explicit RestClientAdapter(std::shared_ptr<RestClient> rest_client)
        : rest_client_(rest_client) {}

    // Exchange info
    std::string get_exchange_name() const override { return "binance"; }
    bool is_connected() const override { return true; }  // REST is always "connected"

    // Connection management
    bool connect() override { return true; }
    void disconnect() override {}

    // Market data (not used in OrderManager, but required by interface)
    bool subscribe_orderbook(const std::string&, int) override { return true; }
    bool subscribe_trades(const std::string&) override { return true; }
    bool unsubscribe(const std::string&) override { return true; }
    std::optional<OrderBook> get_orderbook(const std::string& symbol, int limit = 20) override {
        return rest_client_->get_orderbook(symbol, limit);
    }
    std::optional<double> get_current_price(const std::string& symbol) override {
        return rest_client_->get_current_price(symbol);
    }
    std::optional<std::string> get_exchange_info() override {
        return rest_client_->get_exchange_info();
    }

    // Order management (delegate to RestClient)
    std::optional<Order> place_limit_order(
        const std::string& symbol,
        OrderSide side,
        double price,
        double quantity,
        const std::string& client_order_id = "") override {
        return rest_client_->place_limit_order(symbol, side, price, quantity, client_order_id);
    }

    std::optional<Order> place_market_order(
        const std::string&, OrderSide, double, const std::string& = "") override {
        return std::nullopt;  // Not implemented
    }

    std::optional<bool> cancel_order(const std::string& symbol, const std::string& order_id) override {
        return rest_client_->cancel_order(symbol, order_id);
    }

    std::optional<bool> cancel_all_orders(const std::string& symbol) override {
        return rest_client_->cancel_all_orders(symbol);
    }

    std::optional<Order> modify_order(
        const std::string& symbol,
        const std::string& order_id,
        double new_price,
        double new_quantity) override {
        return rest_client_->modify_order(symbol, order_id, new_price, new_quantity);
    }

    std::optional<std::vector<Order>> get_open_orders(const std::string& symbol) override {
        return rest_client_->get_open_orders(symbol);
    }

    std::optional<Order> get_order_status(const std::string&, const std::string&) override {
        return std::nullopt;  // Not implemented
    }

    // Account info
    std::optional<std::string> get_account_info() override {
        return rest_client_->get_account_info();
    }

    std::optional<double> get_balance(const std::string&) override {
        return std::nullopt;  // Not implemented
    }

    // Event handlers (not used for REST-only adapter)
    void set_orderbook_handler(OrderbookHandler) override {}
    void set_message_handler(MessageHandler) override {}
    void set_connection_handler(ConnectionHandler) override {}

    // Utility methods
    bool get_symbol_info(const std::string& symbol, int& price_precision, int& quantity_precision) override {
        return rest_client_->get_symbol_info(symbol, price_precision, quantity_precision);
    }

    double format_price(double price, const std::string&) override { return price; }
    double format_quantity(double quantity, const std::string&) override { return quantity; }
    double get_min_order_size(const std::string&) override { return 0.00001; }
    double get_max_order_size(const std::string&) override { return 10000000; }
    double get_tick_size(const std::string&) override { return 0.01; }

private:
    std::shared_ptr<RestClient> rest_client_;
};

} // namespace MarketMaker

#endif // REST_CLIENT_ADAPTER_H
