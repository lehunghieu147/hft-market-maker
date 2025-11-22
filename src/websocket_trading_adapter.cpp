#include "websocket_trading_adapter.h"
#include <json/json.h>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <thread>
#include <chrono>

namespace MarketMaker {

WebSocketTradingAdapter::WebSocketTradingAdapter(
    const std::string& api_key,
    const std::string& api_secret,
    const std::string& ws_market_base_url,
    const std::string& ws_trading_base_url)
    : api_key_(api_key),
      api_secret_(api_secret),
      ws_market_base_url_(ws_market_base_url),
      ws_trading_base_url_(ws_trading_base_url) {

    // Initialize WebSocket clients
    ws_market_client_ = std::make_shared<WebSocketClient>();
    ws_trading_client_ = std::make_shared<WebSocketTradingClient>(api_key, api_secret);

    // Set up market data handler
    ws_market_client_->set_message_handler(
        [this](const std::string& msg) { handle_market_data_message(msg); }
    );

    // Set up trading response handler
    ws_trading_client_->set_order_response_handler(
        [this](const Json::Value& response) { handle_trading_response(response); }
    );

    // Enable auto-reconnect for both clients
    ws_market_client_->enable_auto_reconnect(true);
    ws_trading_client_->enable_auto_reconnect(true);

    std::cout << "WebSocket Trading Adapter initialized" << std::endl;
}

WebSocketTradingAdapter::~WebSocketTradingAdapter() {
    disconnect();
}

bool WebSocketTradingAdapter::is_connected() const {
    return ws_market_client_->is_connected() && ws_trading_client_->is_connected();
}

bool WebSocketTradingAdapter::connect() {
    [[maybe_unused]] bool market_connected = true;  // Market data connection is optional for trading
    bool trading_connected = false;

    // Connect to trading WebSocket with retry
    std::string trading_url = ws_trading_base_url_ + "/ws-api/v3";
    int max_retries = 100;
    int retry_delay_ms = 1000;

    std::cout << "Connecting to WebSocket Trading API: " << trading_url << std::endl;

    for (int attempt = 1; attempt <= max_retries && !trading_connected; ++attempt) {
        if (attempt > 1) {
            std::cout << "Retry attempt " << attempt << "/" << max_retries << "..." << std::endl;

            // Cleanup old connection before retry
            std::cout << "Cleaning up old connection..." << std::endl;
            ws_trading_client_->disconnect();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Recreate WebSocket client for clean state
            ws_trading_client_ = std::make_shared<WebSocketTradingClient>(api_key_, api_secret_);
            ws_trading_client_->set_order_response_handler(
                [this](const Json::Value& response) { handle_trading_response(response); }
            );
            ws_trading_client_->enable_auto_reconnect(true);

            std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms * attempt));
        }

        trading_connected = ws_trading_client_->connect(trading_url);

        if (!trading_connected && attempt < max_retries) {
            std::cerr << "Connection attempt " << attempt << " failed, retrying..." << std::endl;
        }
    }

    if (!trading_connected) {
        std::cerr << "Failed to connect to WebSocket Trading API after " << max_retries << " attempts" << std::endl;
        return false;
    }

    std::cout << "Successfully connected to WebSocket Trading API" << std::endl;

    // Set up connection handler to notify when both are connected
    if (connection_handler_) {
        connection_handler_(trading_connected);
    }

    return trading_connected;
}

void WebSocketTradingAdapter::disconnect() {
    if (ws_market_client_) {
        ws_market_client_->disconnect();
    }

    if (ws_trading_client_) {
        ws_trading_client_->disconnect();
    }

    if (connection_handler_) {
        connection_handler_(false);
    }
}

bool WebSocketTradingAdapter::subscribe_orderbook(const std::string& symbol, int depth) {
    if (!ws_market_client_->is_connected()) {
        // Connect to market data stream
        std::string symbol_lower = symbol;
        std::transform(symbol_lower.begin(), symbol_lower.end(), symbol_lower.begin(), ::tolower);
        std::string market_url = ws_market_base_url_ + "/" + symbol_lower + "@depth" +
                                std::to_string(depth) + "@100ms";

        if (!ws_market_client_->connect(market_url)) {
            std::cerr << "Failed to connect to market data WebSocket" << std::endl;
            return false;
        }
    }

    return true;
}

bool WebSocketTradingAdapter::subscribe_trades([[maybe_unused]] const std::string& symbol) {
    // This would subscribe to trade stream if needed
    return true;
}

bool WebSocketTradingAdapter::unsubscribe([[maybe_unused]] const std::string& symbol) {
    // WebSocket streams are connection-based, so we'd need to disconnect
    // In a full implementation, we'd manage multiple subscriptions
    return true;
}

std::optional<OrderBook> WebSocketTradingAdapter::get_orderbook([[maybe_unused]] const std::string& symbol, int limit) {
    std::lock_guard<std::mutex> lock(orderbook_mutex_);

    if (current_orderbook_.bids.empty() && current_orderbook_.asks.empty()) {
        return std::nullopt;
    }

    OrderBook limited_book = current_orderbook_;

    // Limit the depth if requested
    if (limit > 0) {
        if (limited_book.bids.size() > static_cast<size_t>(limit)) {
            limited_book.bids.resize(limit);
        }
        if (limited_book.asks.size() > static_cast<size_t>(limit)) {
            limited_book.asks.resize(limit);
        }
    }

    return limited_book;
}

std::optional<double> WebSocketTradingAdapter::get_current_price([[maybe_unused]] const std::string& symbol) {
    std::lock_guard<std::mutex> lock(orderbook_mutex_);
    return current_orderbook_.get_mid_price();
}

std::optional<std::string> WebSocketTradingAdapter::get_exchange_info() {
    // This would typically be cached or fetched via REST API
    // For now, return a basic response
    return "{\"timezone\":\"UTC\",\"serverTime\":" +
           std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "}";
}

std::optional<Order> WebSocketTradingAdapter::place_limit_order(
    const std::string& symbol,
    OrderSide side,
    double price,
    double quantity,
    const std::string& client_order_id) {

    auto start_time = std::chrono::steady_clock::now();

    // Use WebSocket API to place order
    auto order_id = ws_trading_client_->place_limit_order(
        symbol, side, price, quantity, client_order_id, true
    );

    if (!order_id) {
        return std::nullopt;
    }

    // Calculate latency
    auto end_time = std::chrono::steady_clock::now();
    auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    ).count();

    std::cout << "WebSocket order placement latency: " << latency_ms << " ms" << std::endl;

    // Create Order object
    Order order;
    order.order_id = *order_id;
    order.client_order_id = client_order_id;
    order.symbol = symbol;
    order.side = side;
    order.price = price;
    order.quantity = quantity;
    order.status = OrderStatus::NEW;
    order.created_time = std::chrono::steady_clock::now();

    return order;
}

std::optional<Order> WebSocketTradingAdapter::place_market_order(
    [[maybe_unused]] const std::string& symbol,
    [[maybe_unused]] OrderSide side,
    [[maybe_unused]] double quantity,
    [[maybe_unused]] const std::string& client_order_id) {

    // Market orders via WebSocket would use type: "MARKET"
    // For now, not implemented as the bot uses limit orders
    return std::nullopt;
}

std::optional<bool> WebSocketTradingAdapter::cancel_order(
    const std::string& symbol,
    const std::string& order_id) {

    auto start_time = std::chrono::steady_clock::now();

    auto result = ws_trading_client_->cancel_order(symbol, order_id, true);

    // Calculate latency
    auto end_time = std::chrono::steady_clock::now();
    auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    ).count();

    std::cout << "WebSocket order cancellation latency: " << latency_ms << " ms" << std::endl;

    return result;
}

std::optional<bool> WebSocketTradingAdapter::cancel_all_orders(const std::string& symbol) {
    return ws_trading_client_->cancel_all_orders(symbol, true);
}

std::optional<Order> WebSocketTradingAdapter::modify_order(
    const std::string& symbol,
    const std::string& order_id,
    double new_price,
    double new_quantity) {

    // For Binance, modification requires cancel and replace
    // We can optimize this by sending both requests in parallel

    // Cancel existing order asynchronously
    ws_trading_client_->cancel_order(symbol, order_id, false);

    // Immediately place new order
    auto new_order_id = ws_trading_client_->place_limit_order(
        symbol, OrderSide::BUY, new_price, new_quantity, "", true
    );

    if (!new_order_id) {
        return std::nullopt;
    }

    Order order;
    order.order_id = *new_order_id;
    order.symbol = symbol;
    order.price = new_price;
    order.quantity = new_quantity;
    order.status = OrderStatus::NEW;

    return order;
}

std::optional<std::vector<Order>> WebSocketTradingAdapter::get_open_orders(const std::string& symbol) {
    auto json_orders = ws_trading_client_->get_open_orders(symbol, true);

    if (!json_orders) {
        return std::nullopt;
    }

    std::vector<Order> orders;
    if (json_orders->isArray()) {
        for (const auto& json_order : *json_orders) {
            orders.push_back(json_to_order(json_order));
        }
    }

    return orders;
}

std::optional<Order> WebSocketTradingAdapter::get_order_status(
    const std::string& symbol,
    const std::string& order_id) {

    auto json_order = ws_trading_client_->query_order(symbol, order_id, true);

    if (!json_order) {
        return std::nullopt;
    }

    return json_to_order(*json_order);
}

std::optional<std::string> WebSocketTradingAdapter::get_account_info() {
    // Account info would typically be fetched via REST API
    // or a specific WebSocket method if available
    return std::nullopt;
}

std::optional<double> WebSocketTradingAdapter::get_balance([[maybe_unused]] const std::string& asset) {
    // Balance queries would typically be done via REST API
    return std::nullopt;
}

void WebSocketTradingAdapter::set_orderbook_handler(OrderbookHandler handler) {
    orderbook_handler_ = handler;
}

void WebSocketTradingAdapter::set_message_handler(MessageHandler handler) {
    message_handler_ = handler;
}

void WebSocketTradingAdapter::set_connection_handler(ConnectionHandler handler) {
    connection_handler_ = handler;

    // Also set for individual clients
    ws_market_client_->set_connection_handler(
        [this, handler]([[maybe_unused]] bool connected) {
            if (handler) {
                handler(is_connected());
            }
        }
    );

    ws_trading_client_->set_connection_handler(
        [this, handler]([[maybe_unused]] bool connected) {
            if (handler) {
                handler(is_connected());
            }
        }
    );
}

bool WebSocketTradingAdapter::get_symbol_info(
    const std::string& symbol,
    int& price_precision,
    int& quantity_precision) {

    std::lock_guard<std::mutex> lock(cache_mutex_);

    // Check cache first
    auto it = symbol_info_cache_.find(symbol);
    if (it != symbol_info_cache_.end()) {
        price_precision = it->second.price_precision;
        quantity_precision = it->second.quantity_precision;
        return true;
    }

    // Default values for common symbols
    // In production, this would be fetched from exchange info
    if (symbol == "BTCUSDT" || symbol == "ETHUSDT") {
        price_precision = 2;
        quantity_precision = 5;
    } else {
        price_precision = 4;
        quantity_precision = 6;
    }

    // Cache the values
    SymbolInfo info;
    info.price_precision = price_precision;
    info.quantity_precision = quantity_precision;
    symbol_info_cache_[symbol] = info;

    return true;
}

double WebSocketTradingAdapter::format_price(double price, const std::string& symbol) {
    int precision = 2;
    int dummy;
    get_symbol_info(symbol, precision, dummy);

    double multiplier = std::pow(10, precision);
    return std::round(price * multiplier) / multiplier;
}

double WebSocketTradingAdapter::format_quantity(double quantity, const std::string& symbol) {
    int dummy;
    int precision = 5;
    get_symbol_info(symbol, dummy, precision);

    double multiplier = std::pow(10, precision);
    return std::round(quantity * multiplier) / multiplier;
}

double WebSocketTradingAdapter::get_min_order_size(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = symbol_info_cache_.find(symbol);
    if (it != symbol_info_cache_.end()) {
        return it->second.min_quantity;
    }
    return 0.00001;
}

double WebSocketTradingAdapter::get_max_order_size(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = symbol_info_cache_.find(symbol);
    if (it != symbol_info_cache_.end()) {
        return it->second.max_quantity;
    }
    return 10000000;
}

double WebSocketTradingAdapter::get_tick_size(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = symbol_info_cache_.find(symbol);
    if (it != symbol_info_cache_.end()) {
        return it->second.tick_size;
    }
    return 0.01;
}

WebSocketTradingAdapter::CombinedMetrics WebSocketTradingAdapter::get_metrics() const {
    CombinedMetrics metrics;

    if (ws_trading_client_) {
        const auto& trading_metrics = ws_trading_client_->get_metrics();
        metrics.total_orders = trading_metrics.total_requests;
        metrics.successful_orders = trading_metrics.successful_orders;
        metrics.failed_orders = trading_metrics.failed_orders;
        metrics.avg_order_latency_ms = trading_metrics.avg_response_time_ms;
    }

    // Market data metrics would come from ws_market_client_
    // if it had a metrics interface

    return metrics;
}

Order WebSocketTradingAdapter::json_to_order(const Json::Value& json_order) {
    Order order;

    if (json_order.isMember("orderId")) {
        order.order_id = json_order["orderId"].asString();
    }

    if (json_order.isMember("clientOrderId")) {
        order.client_order_id = json_order["clientOrderId"].asString();
    }

    if (json_order.isMember("symbol")) {
        order.symbol = json_order["symbol"].asString();
    }

    if (json_order.isMember("side")) {
        std::string side = json_order["side"].asString();
        order.side = (side == "BUY") ? OrderSide::BUY : OrderSide::SELL;
    }

    // Note: Order struct doesn't have a type field in the current implementation
    // Type information can be inferred from price (LIMIT if price > 0, MARKET otherwise)

    if (json_order.isMember("price")) {
        order.price = std::stod(json_order["price"].asString());
    }

    if (json_order.isMember("origQty")) {
        order.quantity = std::stod(json_order["origQty"].asString());
    }

    if (json_order.isMember("executedQty")) {
        order.executed_quantity = std::stod(json_order["executedQty"].asString());
    }

    if (json_order.isMember("status")) {
        std::string status = json_order["status"].asString();
        if (status == "NEW") {
            order.status = OrderStatus::NEW;
        } else if (status == "FILLED") {
            order.status = OrderStatus::FILLED;
        } else if (status == "CANCELED") {
            order.status = OrderStatus::CANCELED;
        } else if (status == "PARTIALLY_FILLED") {
            order.status = OrderStatus::PARTIALLY_FILLED;
        }
    }

    return order;
}

void WebSocketTradingAdapter::handle_market_data_message(const std::string& message) {
    Json::Reader reader;
    Json::Value data;

    if (!reader.parse(message, data)) {
        return;
    }

    update_orderbook_from_message(data);

    if (message_handler_) {
        message_handler_(message);
    }
}

void WebSocketTradingAdapter::handle_trading_response(const Json::Value& response) {
    // Log trading responses for monitoring
    if (response.isMember("result")) {
        const Json::Value& result = response["result"];
        if (result.isMember("orderId")) {
            std::cout << "Order response received - ID: " << result["orderId"].asString() << std::endl;
        }
    } else if (response.isMember("error")) {
        std::cerr << "Trading error: " << response["error"]["msg"].asString() << std::endl;
    }
}

void WebSocketTradingAdapter::update_orderbook_from_message(const Json::Value& data) {
    std::lock_guard<std::mutex> lock(orderbook_mutex_);

    current_orderbook_.timestamp = std::chrono::steady_clock::now();
    current_orderbook_.bids.clear();
    current_orderbook_.asks.clear();

    // Process bids
    if (data.isMember("bids")) {
        for (const auto& bid : data["bids"]) {
            if (bid.isArray() && bid.size() >= 2) {
                double price = std::stod(bid[0].asString());
                double quantity = std::stod(bid[1].asString());
                current_orderbook_.bids.emplace_back(price, quantity);
            }
        }
    }

    // Process asks
    if (data.isMember("asks")) {
        for (const auto& ask : data["asks"]) {
            if (ask.isArray() && ask.size() >= 2) {
                double price = std::stod(ask[0].asString());
                double quantity = std::stod(ask[1].asString());
                current_orderbook_.asks.emplace_back(price, quantity);
            }
        }
    }

    // Sort bids (descending) and asks (ascending)
    std::sort(current_orderbook_.bids.begin(), current_orderbook_.bids.end(),
              [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
    std::sort(current_orderbook_.asks.begin(), current_orderbook_.asks.end(),
              [](const PriceLevel& a, const PriceLevel& b) { return a.price < b.price; });

    if (orderbook_handler_) {
        orderbook_handler_(current_orderbook_);
    }
}

} // namespace MarketMaker