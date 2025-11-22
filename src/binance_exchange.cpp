#include "binance_exchange.h"
#include <json/json.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <cmath>
#include <algorithm>

namespace MarketMaker {

BinanceExchange::BinanceExchange() {
    // Constructor
}

BinanceExchange::~BinanceExchange() {
    disconnect();
}

bool BinanceExchange::initialize(const ExchangeConfig& config) {
    config_ = config;

    // Initialize REST client with Binance-specific configuration
    rest_client_ = std::make_shared<RestClient>(
        config.api_url,
        config.api_key,
        config.api_secret
    );

    // Configure REST client with display assets
    if (!config.display_assets.empty()) {
        rest_client_->set_display_assets(config.display_assets);
    }

    // Configure supported quote currencies for symbol conversion
    if (!config.supported_quote_currencies.empty()) {
        supported_quote_currencies_ = config.supported_quote_currencies;
    }

    // Initialize WebSocket client
    ws_client_ = std::make_shared<WebSocketClient>();

    // Set up WebSocket handlers
    ws_client_->set_message_handler([this](const std::string& msg) {
        handle_websocket_message(msg);
    });

    ws_client_->set_connection_handler([this](bool connected) {
        ws_connected_ = connected;

        if (connected && !subscribed_symbol_.empty()) {
            std::cout << "Reconnected - orderbook stream for " << subscribed_symbol_
                      << " should be active now" << std::endl;
        }

        if (connection_handler_) {
            connection_handler_(connected);
        }
    });

    ws_client_->enable_auto_reconnect(true);

    // Fetch exchange info to populate symbol cache
    auto exchange_info = get_exchange_info();
    if (!exchange_info) {
        std::cerr << "Failed to fetch Binance exchange info" << std::endl;
        return false;
    }

    initialized_ = true;
    std::cout << "BinanceExchange initialized successfully" << std::endl;

    // Check account balance on startup
    std::cout << "Checking account balance..." << std::endl;
    std::string account_info = rest_client_->get_account_info();
    std::cout << "Account check completed" << std::endl;

    return true;
}

bool BinanceExchange::connect() {
    if (!initialized_) {
        std::cerr << "BinanceExchange not initialized" << std::endl;
        return false;
    }

    // Don't connect yet - wait for subscribe_orderbook to build full URL
    // WebSocket connection will be established when subscribing to streams
    return true;
}

void BinanceExchange::disconnect() {
    if (ws_client_) {
        ws_client_->disconnect();
    }
    ws_connected_ = false;
}

bool BinanceExchange::is_connected() const {
    return ws_connected_.load();
}

// ========== Market Data ==========

bool BinanceExchange::subscribe_orderbook(const std::string& symbol, int depth) {
    if (!ws_client_) {
        return false;
    }

    // Save symbol for resubscription on reconnect
    subscribed_symbol_ = symbol;
    subscribed_depth_ = depth;

    // Convert symbol to Binance format (e.g., "BTC/USDT" -> "btcusdt")
    std::string binance_symbol = convert_symbol_to_binance(symbol);

    // Convert to lowercase for Binance WebSocket streams
    std::transform(binance_symbol.begin(), binance_symbol.end(), binance_symbol.begin(), ::tolower);

    // Build full WebSocket stream URL for Binance
    // Format: wss://stream.testnet.binance.vision:9443/ws/btcusdt@depth20@100ms
    std::string stream_url = config_.ws_url;

    // Remove trailing /ws if present
    if (stream_url.size() >= 3 && stream_url.substr(stream_url.size() - 3) == "/ws") {
        stream_url = stream_url.substr(0, stream_url.size() - 3);
    }

    // Add stream path
    stream_url += "/ws/" + binance_symbol + "@depth" + std::to_string(depth) + "@100ms";

    std::cout << "Connecting to Binance stream: " << stream_url << std::endl;

    // Connect with full stream URL
    if (!ws_client_->connect(stream_url)) {
        std::cerr << "Failed to connect to Binance WebSocket stream" << std::endl;
        return false;
    }

    ws_connected_ = true;
    return true;
}

bool BinanceExchange::subscribe_trades(const std::string& symbol) {
    if (!ws_client_ || !ws_connected_) {
        return false;
    }

    std::string binance_symbol = convert_symbol_to_binance(symbol);
    ws_client_->subscribe_trades(binance_symbol);
    return true;
}

bool BinanceExchange::unsubscribe([[maybe_unused]] const std::string& symbol) {
    // Implementation would depend on WebSocketClient supporting unsubscribe
    return true;
}

std::optional<OrderBook> BinanceExchange::get_orderbook(const std::string& symbol, int limit) {
    if (!rest_client_) {
        return std::nullopt;
    }

    std::string binance_symbol = convert_symbol_to_binance(symbol);
    return rest_client_->get_orderbook(binance_symbol, limit);
}

std::optional<double> BinanceExchange::get_current_price(const std::string& symbol) {
    if (!rest_client_) {
        return std::nullopt;
    }

    std::string binance_symbol = convert_symbol_to_binance(symbol);
    return rest_client_->get_current_price(binance_symbol);
}

std::optional<std::string> BinanceExchange::get_exchange_info() {
    if (!rest_client_) {
        return std::nullopt;
    }

    auto info = rest_client_->get_exchange_info();

    // Parse and cache symbol information
    if (info.has_value()) {
        try {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(info.value(), root)) {
                const Json::Value& symbols = root["symbols"];

                std::lock_guard<std::mutex> lock(cache_mutex_);
                for (const auto& symbol : symbols) {
                    SymbolInfo sym_info;
                    std::string symbol_name = symbol["symbol"].asString();

                    // Parse filters for precision and limits
                    const Json::Value& filters = symbol["filters"];
                    for (const auto& filter : filters) {
                        std::string filterType = filter["filterType"].asString();

                        if (filterType == "PRICE_FILTER") {
                            sym_info.tick_size = std::stod(filter["tickSize"].asString());
                            // Calculate precision from tick size
                            std::string tick_str = filter["tickSize"].asString();
                            size_t decimal_pos = tick_str.find('.');
                            if (decimal_pos != std::string::npos) {
                                sym_info.price_precision = tick_str.length() - decimal_pos - 1;
                            } else {
                                sym_info.price_precision = 0;
                            }
                        } else if (filterType == "LOT_SIZE") {
                            sym_info.min_qty = std::stod(filter["minQty"].asString());
                            sym_info.max_qty = std::stod(filter["maxQty"].asString());

                            // Calculate quantity precision
                            std::string step_str = filter["stepSize"].asString();
                            size_t decimal_pos = step_str.find('.');
                            if (decimal_pos != std::string::npos) {
                                sym_info.quantity_precision = step_str.length() - decimal_pos - 1;
                            } else {
                                sym_info.quantity_precision = 0;
                            }
                        }
                    }

                    symbol_cache_[symbol_name] = sym_info;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing Binance exchange info: " << e.what() << std::endl;
        }
    }

    return info;
}

// ========== Order Management ==========

std::optional<Order> BinanceExchange::place_limit_order(
    const std::string& symbol,
    OrderSide side,
    double price,
    double quantity,
    const std::string& client_order_id
) {
    if (!rest_client_) {
        return std::nullopt;
    }

    enforce_rate_limit();

    std::string binance_symbol = convert_symbol_to_binance(symbol);

    // Format price and quantity according to Binance requirements
    double formatted_price = format_price(price, binance_symbol);
    double formatted_qty = format_quantity(quantity, binance_symbol);

    return rest_client_->place_limit_order(
        binance_symbol,
        side,
        formatted_price,
        formatted_qty,
        client_order_id
    );
}

std::optional<Order> BinanceExchange::place_market_order(
    [[maybe_unused]] const std::string& symbol,
    [[maybe_unused]] OrderSide side,
    [[maybe_unused]] double quantity,
    [[maybe_unused]] const std::string& client_order_id
) {
    // Binance market orders can be implemented through REST API
    // For now, return nullopt as it's not in current RestClient
    return std::nullopt;
}

std::optional<bool> BinanceExchange::cancel_order(
    const std::string& symbol,
    const std::string& order_id
) {
    if (!rest_client_) {
        return std::nullopt;
    }

    enforce_rate_limit();

    std::string binance_symbol = convert_symbol_to_binance(symbol);
    return rest_client_->cancel_order(binance_symbol, order_id);
}

std::optional<bool> BinanceExchange::cancel_all_orders(const std::string& symbol) {
    if (!rest_client_) {
        return std::nullopt;
    }

    enforce_rate_limit();

    std::string binance_symbol = convert_symbol_to_binance(symbol);
    return rest_client_->cancel_all_orders(binance_symbol);
}

std::optional<Order> BinanceExchange::modify_order(
    const std::string& symbol,
    const std::string& order_id,
    double new_price,
    double new_quantity
) {
    if (!rest_client_) {
        return std::nullopt;
    }

    enforce_rate_limit();

    std::string binance_symbol = convert_symbol_to_binance(symbol);

    double formatted_price = format_price(new_price, binance_symbol);
    double formatted_qty = format_quantity(new_quantity, binance_symbol);

    // Use parallel modify_order for better performance
    // Note: We need to get the current order's side first
    auto open_orders = rest_client_->get_open_orders(binance_symbol);
    OrderSide side = OrderSide::BUY; // Default

    if (open_orders.has_value()) {
        for (const auto& order : open_orders.value()) {
            if (order.order_id == order_id) {
                side = order.side;
                break;
            }
        }
    }

    return rest_client_->modify_order_parallel(
        binance_symbol,
        order_id,
        side,
        formatted_price,
        formatted_qty
    );
}

std::optional<std::vector<Order>> BinanceExchange::get_open_orders(const std::string& symbol) {
    if (!rest_client_) {
        return std::nullopt;
    }

    std::string binance_symbol = convert_symbol_to_binance(symbol);
    return rest_client_->get_open_orders(binance_symbol);
}

std::optional<Order> BinanceExchange::get_order_status(
    [[maybe_unused]] const std::string& symbol,
    [[maybe_unused]] const std::string& order_id
) {
    // Would need to implement in RestClient
    return std::nullopt;
}

// ========== Account Information ==========

std::optional<std::string> BinanceExchange::get_account_info() {
    if (!rest_client_) {
        return std::nullopt;
    }

    return rest_client_->get_account_info();
}

std::optional<double> BinanceExchange::get_balance(const std::string& asset) {
    auto account_info = get_account_info();
    if (!account_info) {
        return std::nullopt;
    }

    try {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(account_info.value(), root)) {
            const Json::Value& balances = root["balances"];
            for (const auto& balance : balances) {
                if (balance["asset"].asString() == asset) {
                    return std::stod(balance["free"].asString());
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing balance: " << e.what() << std::endl;
    }

    return std::nullopt;
}

// ========== Event Handlers ==========

void BinanceExchange::set_orderbook_handler(OrderbookHandler handler) {
    orderbook_handler_ = handler;
}

void BinanceExchange::set_message_handler(MessageHandler handler) {
    message_handler_ = handler;
}

void BinanceExchange::set_connection_handler(ConnectionHandler handler) {
    connection_handler_ = handler;
}

// ========== Utility Methods ==========

bool BinanceExchange::get_symbol_info(
    const std::string& symbol,
    int& price_precision,
    int& quantity_precision
) {
    std::string binance_symbol = convert_symbol_to_binance(symbol);

    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = symbol_cache_.find(binance_symbol);
    if (it != symbol_cache_.end()) {
        price_precision = it->second.price_precision;
        quantity_precision = it->second.quantity_precision;
        return true;
    }

    // If not in cache, try to get from REST API
    return rest_client_->get_symbol_info(binance_symbol, price_precision, quantity_precision);
}

double BinanceExchange::format_price(double price, const std::string& symbol) {
    int price_precision = 2;  // Default
    int quantity_precision;

    get_symbol_info(symbol, price_precision, quantity_precision);

    double multiplier = std::pow(10, price_precision);
    return std::round(price * multiplier) / multiplier;
}

double BinanceExchange::format_quantity(double quantity, const std::string& symbol) {
    int price_precision;
    int quantity_precision = 8;  // Default

    get_symbol_info(symbol, price_precision, quantity_precision);

    double multiplier = std::pow(10, quantity_precision);
    return std::round(quantity * multiplier) / multiplier;
}

double BinanceExchange::get_min_order_size(const std::string& symbol) {
    std::string binance_symbol = convert_symbol_to_binance(symbol);

    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = symbol_cache_.find(binance_symbol);
    if (it != symbol_cache_.end()) {
        return it->second.min_qty;
    }

    return 0.00001;  // Default minimum
}

double BinanceExchange::get_max_order_size(const std::string& symbol) {
    std::string binance_symbol = convert_symbol_to_binance(symbol);

    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = symbol_cache_.find(binance_symbol);
    if (it != symbol_cache_.end()) {
        return it->second.max_qty;
    }

    return 10000000;  // Default maximum
}

double BinanceExchange::get_tick_size(const std::string& symbol) {
    std::string binance_symbol = convert_symbol_to_binance(symbol);

    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = symbol_cache_.find(binance_symbol);
    if (it != symbol_cache_.end()) {
        return it->second.tick_size;
    }

    return 0.01;  // Default tick size
}

// ========== Helper Methods ==========

void BinanceExchange::handle_websocket_message(const std::string& message) {
    // Pass through to generic handler if set
    if (message_handler_) {
        message_handler_(message);
    }

    // Process Binance-specific orderbook updates
    process_binance_orderbook(message);
}

void BinanceExchange::process_binance_orderbook(const std::string& json_str) {
    try {
        Json::Value root;
        Json::Reader reader;

        if (!reader.parse(json_str, root)) {
            return;
        }

        // Check if this is an orderbook update
        if (root.isMember("bids") && root.isMember("asks")) {
            OrderBook orderbook;

            // Parse bids
            const Json::Value& bids = root["bids"];
            for (const auto& bid : bids) {
                if (bid.isArray() && bid.size() >= 2) {
                    PriceLevel level;
                    level.price = std::stod(bid[0].asString());
                    level.quantity = std::stod(bid[1].asString());
                    orderbook.bids.push_back(level);
                }
            }

            // Parse asks
            const Json::Value& asks = root["asks"];
            for (const auto& ask : asks) {
                if (ask.isArray() && ask.size() >= 2) {
                    PriceLevel level;
                    level.price = std::stod(ask[0].asString());
                    level.quantity = std::stod(ask[1].asString());
                    orderbook.asks.push_back(level);
                }
            }

            // Update internal orderbook
            {
                std::lock_guard<std::mutex> lock(orderbook_mutex_);
                current_orderbook_ = orderbook;
            }

            // Notify handler
            if (orderbook_handler_) {
                orderbook_handler_(orderbook);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error processing Binance orderbook: " << e.what() << std::endl;
    }
}

std::string BinanceExchange::convert_symbol_to_binance(const std::string& symbol) {
    // Convert from generic format (e.g., "BTC/USDT") to Binance format ("BTCUSDT")
    std::string result = symbol;

    // Remove slash if present
    size_t pos = result.find('/');
    if (pos != std::string::npos) {
        result.erase(pos, 1);
    }

    // Convert to uppercase
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);

    return result;
}

void BinanceExchange::set_supported_quote_currencies(const std::vector<std::string>& currencies) {
    supported_quote_currencies_ = currencies;
}

std::string BinanceExchange::convert_symbol_from_binance(const std::string& symbol) {
    // Convert from Binance format ("BTCUSDT") to generic format ("BTC/USDT")
    // This is simplified - in production, you'd need a mapping of known base/quote pairs

    // Use configured quote currencies instead of hardcoded list
    for (const auto& quote : supported_quote_currencies_) {
        if (symbol.size() > quote.size()) {
            std::string potential_quote = symbol.substr(symbol.size() - quote.size());
            if (potential_quote == quote) {
                std::string base = symbol.substr(0, symbol.size() - quote.size());
                return base + "/" + quote;
            }
        }
    }

    return symbol;  // Return as-is if no match
}

Order BinanceExchange::parse_order_response(const std::string& json_str) {
    Order order;

    try {
        Json::Value root;
        Json::Reader reader;

        if (reader.parse(json_str, root)) {
            order.order_id = root["orderId"].asString();
            order.client_order_id = root["clientOrderId"].asString();
            order.symbol = root["symbol"].asString();
            order.side = (root["side"].asString() == "BUY") ? OrderSide::BUY : OrderSide::SELL;
            order.price = std::stod(root["price"].asString());
            order.quantity = std::stod(root["origQty"].asString());

            // Convert Binance status string to OrderStatus enum
            std::string status_str = root["status"].asString();
            if (status_str == "NEW") {
                order.status = OrderStatus::NEW;
            } else if (status_str == "PARTIALLY_FILLED") {
                order.status = OrderStatus::PARTIALLY_FILLED;
            } else if (status_str == "FILLED") {
                order.status = OrderStatus::FILLED;
            } else if (status_str == "CANCELED") {
                order.status = OrderStatus::CANCELED;
            } else if (status_str == "REJECTED") {
                order.status = OrderStatus::REJECTED;
            } else if (status_str == "EXPIRED") {
                order.status = OrderStatus::EXPIRED;
            } else {
                order.status = OrderStatus::NEW; // Default
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing order response: " << e.what() << std::endl;
    }

    return order;
}

bool BinanceExchange::parse_error_response(const std::string& json_str, std::string& error_msg) {
    try {
        Json::Value root;
        Json::Reader reader;

        if (reader.parse(json_str, root)) {
            if (root.isMember("msg")) {
                error_msg = root["msg"].asString();
                return true;
            }
        }
    } catch (const std::exception& e) {
        error_msg = "Unknown error";
    }

    return false;
}

void BinanceExchange::enforce_rate_limit() {
    std::lock_guard<std::mutex> lock(rate_limit_mutex_);

    auto now = std::chrono::steady_clock::now();
    auto time_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_request_time_
    );

    // Ensure minimum time between requests (100ms for 10 requests/second)
    int min_interval_ms = 1000 / config_.max_requests_per_second;

    if (time_since_last.count() < min_interval_ms) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(min_interval_ms - time_since_last.count())
        );
    }

    last_request_time_ = std::chrono::steady_clock::now();
}

} // namespace MarketMaker