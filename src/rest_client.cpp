#include "rest_client.h"
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <json/json.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include <vector>

namespace MarketMaker {

// Callback for CURL to write response data
static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

class RestClient::Impl {
public:
    std::vector<CURL*> curl_pool;
    std::mutex pool_mutex;
    std::string base_url;
    std::string api_key;
    std::string api_secret;
    struct curl_slist* headers;
    size_t pool_index = 0;
    std::vector<std::string> display_assets = {"USDT", "BTC"};  // Default assets to display

    Impl(const std::string& url, const std::string& key, const std::string& secret)
        : base_url(url), api_key(key), api_secret(secret), headers(nullptr) {
        curl_global_init(CURL_GLOBAL_DEFAULT);

        // Create connection pool with 4 persistent connections for parallel operations
        for (int i = 0; i < 4; i++) {
            CURL* curl = curl_easy_init();
            if (curl) {
                // Enable HTTP/2 for multiplexing (faster)
                curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
                curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
                curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);  // Reduced from 120
                curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 10L);  // Reduced from 60

                // Optimize for ultra-low latency
                curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
                curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 0L);  // Allow connection reuse
                curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 0L);  // Don't force fresh connection

                // DNS caching for faster resolution
                curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 3600L);  // Cache DNS for 1 hour

                // Reduce timeout for faster failure detection
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1000L);  // 1 second connect timeout
                curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);  // 5 second total timeout

                curl_pool.push_back(curl);
            }
        }

        // Debug: Show API key being used (first 10 chars only for security)
        std::cout << "RestClient using API Key: " << api_key.substr(0, 10) << "..." << std::endl;
        std::cout << "API Key length: " << api_key.length() << std::endl;

        // Set default headers
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Connection: keep-alive");
        std::string api_key_header = "X-MBX-APIKEY: " + api_key;
        headers = curl_slist_append(headers, api_key_header.c_str());
    }

    ~Impl() {
        for (auto curl : curl_pool) {
            if (curl) {
                curl_easy_cleanup(curl);
            }
        }
        if (headers) {
            curl_slist_free_all(headers);
        }
        curl_global_cleanup();
    }

    CURL* get_curl_handle() {
        std::lock_guard<std::mutex> lock(pool_mutex);
        CURL* curl = curl_pool[pool_index];
        pool_index = (pool_index + 1) % curl_pool.size();
        return curl;
    }

    std::string hmac_sha256(const std::string& key, const std::string& data) {
        unsigned char digest[32];  // Use local buffer instead of static pointer
        unsigned int digest_len = 32;

        HMAC(EVP_sha256(), key.c_str(), key.length(),
             (unsigned char*)data.c_str(), data.length(),
             digest, &digest_len);

        std::stringstream ss;
        for (unsigned int i = 0; i < digest_len; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
        }
        return ss.str();
    }

    long get_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    }
};

RestClient::RestClient(const std::string& base_url, const std::string& api_key, const std::string& api_secret)
    : pImpl(std::make_unique<Impl>(base_url, api_key, api_secret)) {}

RestClient::~RestClient() = default;

void RestClient::set_display_assets(const std::vector<std::string>& assets) {
    pImpl->display_assets = assets;
}

std::string RestClient::get_account_info() {
    auto response = send_signed_request("GET", "/api/v3/account", {});

    if (response.has_value()) {
        // Parse JSON to display important balances
        Json::Reader reader;
        Json::Value root;

        if (reader.parse(response.value(), root)) {
            std::cout << "\n====== ACCOUNT INFO ======" << std::endl;
            std::cout << "Can Trade: " << root["canTrade"].asBool() << std::endl;

            if (root.isMember("balances")) {
                std::cout << "\nRelevant Balances:" << std::endl;
                for (const auto& balance : root["balances"]) {
                    std::string asset = balance["asset"].asString();

                    // Check if asset is in display_assets list
                    bool should_display = false;
                    for (const auto& display_asset : pImpl->display_assets) {
                        if (asset == display_asset) {
                            should_display = true;
                            break;
                        }
                    }

                    if (should_display) {
                        std::string free_str = balance["free"].asString();
                        std::string locked_str = balance["locked"].asString();
                        double free_amount = std::stod(free_str);
                        double locked_amount = std::stod(locked_str);

                        // Show raw string and parsed value for debugging
                        if (free_amount > 0 || locked_amount > 0) {
                            std::cout << "  " << asset << ": " << std::endl;
                            std::cout << "    Raw free: '" << free_str << "'" << std::endl;
                            std::cout << "    Parsed free: " << std::fixed << std::setprecision(8)
                                     << free_amount << std::endl;
                            std::cout << "    Locked: " << locked_amount << std::endl;
                        }
                    }
                }
            }
            std::cout << "=========================\n" << std::endl;
        }

        return response.value();
    }

    return "{}";
}

std::string RestClient::generate_signature(const std::string& query_string) {
    return pImpl->hmac_sha256(pImpl->api_secret, query_string);
}

std::string RestClient::build_query_string(const std::vector<std::pair<std::string, std::string>>& params) {
    std::stringstream ss;
    bool first = true;
    for (const auto& param : params) {
        if (!first) ss << "&";
        ss << param.first << "=" << param.second;
        first = false;
    }
    return ss.str();
}

std::optional<std::string> RestClient::send_public_request(
    const std::string& endpoint,
    const std::vector<std::pair<std::string, std::string>>& params) {

    std::string url = pImpl->base_url + endpoint;
    if (!params.empty()) {
        url += "?" + build_query_string(params);
    }

    std::string response;

    // Get a curl handle from the pool
    CURL* curl = pImpl->get_curl_handle();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "CURL error: " << curl_easy_strerror(res) << std::endl;
        return std::nullopt;
    }

    return response;
}

std::optional<std::string> RestClient::send_signed_request(
    const std::string& method,
    const std::string& endpoint,
    const std::vector<std::pair<std::string, std::string>>& params) {

    auto params_copy = params;

    // Add timestamp for time sync
    long timestamp = pImpl->get_timestamp();
    // recvWindow is not needed for testnet
    // params_copy.push_back({"recvWindow", "5000"});  // 5 second window
    params_copy.push_back({"timestamp", std::to_string(timestamp)});

    std::string query_string = build_query_string(params_copy);
    std::string signature = generate_signature(query_string);
    query_string += "&signature=" + signature;

    // Debug log
    std::cout << "Query string: " << query_string.substr(0, 100) << "..." << std::endl;
    std::cout << "Signature: " << signature << std::endl;

    std::string url = pImpl->base_url + endpoint;
    std::string response;

    // Get a curl handle from the pool
    CURL* curl = pImpl->get_curl_handle();

    // Reset all options to avoid conflicts between different request types
    curl_easy_reset(curl);

    // Set up options
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // Recreate headers for this request
    struct curl_slist* request_headers = nullptr;
    request_headers = curl_slist_append(request_headers, "Content-Type: application/json");
    std::string api_key_header = "X-MBX-APIKEY: " + pImpl->api_key;
    request_headers = curl_slist_append(request_headers, api_key_header.c_str());

    // Set headers
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers);

    if (method == "POST") {
        // For Binance API, POST parameters go in URL for signed requests
        url += "?" + query_string;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, NULL);
    } else if (method == "DELETE") {
        url += "?" + query_string;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else {  // GET
        url += "?" + query_string;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    CURLcode res = curl_easy_perform(curl);

    // Clean up headers
    curl_slist_free_all(request_headers);

    if (res != CURLE_OK) {
        std::cerr << "CURL error: " << curl_easy_strerror(res) << std::endl;
        return std::nullopt;
    }

    return response;
}

std::optional<Order> RestClient::place_limit_order(
    const std::string& symbol,
    OrderSide side,
    double price,
    double quantity,
    const std::string& client_order_id) {

    // OPTIMIZATION: Use faster number formatting with pre-allocated buffer
    char price_buffer[32];
    char quantity_buffer[32];

    snprintf(price_buffer, sizeof(price_buffer), "%.2f", price);
    snprintf(quantity_buffer, sizeof(quantity_buffer), "%.5f", quantity);

    std::string price_formatted(price_buffer);
    std::string quantity_formatted(quantity_buffer);

    std::vector<std::pair<std::string, std::string>> params = {
        {"symbol", symbol},
        {"side", side == OrderSide::BUY ? "BUY" : "SELL"},
        {"type", "LIMIT"},
        {"timeInForce", "GTC"},
        {"quantity", quantity_formatted},
        {"price", price_formatted}
    };

    if (!client_order_id.empty()) {
        params.push_back({"newClientOrderId", client_order_id});
    }

    auto response = send_signed_request("POST", "/api/v3/order", params);
    if (!response) {
        std::cerr << "No response received from order endpoint" << std::endl;
        return std::nullopt;
    }

    // Enhanced order response logging
    if (response->find("\"status\":\"NEW\"") != std::string::npos) {
        // Success response - parse and display nicely
        Json::Reader tmpReader;
        Json::Value tmpRoot;
        if (tmpReader.parse(*response, tmpRoot)) {
            std::string order_id = tmpRoot["orderId"].asString();
            std::string side_str = tmpRoot["side"].asString();
            std::string price_str = tmpRoot["price"].asString();
            std::string qty_str = tmpRoot["origQty"].asString();

            if (side_str == "BUY") {
                std::cout << "[SUCCESS] BID Order Placed" << std::endl;
                std::cout << "  Order ID: " << order_id << " | Price: $" << price_str
                         << " | Qty: " << qty_str << std::endl;
            } else {
                std::cout << "[SUCCESS] ASK Order Placed" << std::endl;
                std::cout << "  Order ID: " << order_id << " | Price: $" << price_str
                         << " | Qty: " << qty_str << std::endl;
            }
        }
    } else if (response->find("\"code\"") != std::string::npos) {
        // Error response
        Json::Reader tmpReader;
        Json::Value tmpRoot;
        if (tmpReader.parse(*response, tmpRoot)) {
            std::cout << "[ERROR] Order Failed: " << tmpRoot["msg"].asString() << std::endl;
        } else {
            std::cout << "[ERROR] Order Failed: " << *response << std::endl;
        }
    } else {
        // Unknown response
        std::cout << "Order response: " << *response << std::endl;
    }

    Json::Reader reader;
    Json::Value root;
    if (!reader.parse(*response, root)) {
        std::cerr << "Failed to parse order response" << std::endl;
        return std::nullopt;
    }

    // Check for error response
    if (root.isMember("code") && root.isMember("msg")) {
        std::cerr << "Order error: " << root["msg"].asString()
                  << " (code: " << root["code"].asInt() << ")" << std::endl;
        return std::nullopt;
    }

    Order order;

    // Parse orderId - can be numeric or string
    if (root["orderId"].isNumeric()) {
        order.order_id = std::to_string(root["orderId"].asInt64());
    } else {
        order.order_id = root["orderId"].asString();
    }

    order.client_order_id = root["clientOrderId"].asString();
    order.symbol = root["symbol"].asString();
    order.side = root["side"].asString() == "BUY" ? OrderSide::BUY : OrderSide::SELL;

    // Parse prices and quantities - handle both string and numeric formats
    if (root["price"].isString()) {
        order.price = std::stod(root["price"].asString());
    } else {
        order.price = root["price"].asDouble();
    }

    if (root["origQty"].isString()) {
        order.quantity = std::stod(root["origQty"].asString());
    } else {
        order.quantity = root["origQty"].asDouble();
    }

    if (root["executedQty"].isString()) {
        order.executed_quantity = std::stod(root["executedQty"].asString());
    } else {
        order.executed_quantity = root["executedQty"].asDouble();
    }

    order.status = OrderStatus::NEW;
    order.created_time = std::chrono::steady_clock::now();

    return order;
}

std::optional<bool> RestClient::cancel_order(const std::string& symbol, const std::string& order_id) {
    std::vector<std::pair<std::string, std::string>> params = {
        {"symbol", symbol},
        {"orderId", order_id}
    };

    auto response = send_signed_request("DELETE", "/api/v3/order", params);
    if (!response) {
        return false;
    }

    Json::Reader reader;
    Json::Value root;
    if (!reader.parse(*response, root)) {
        std::cerr << "Failed to parse cancel response" << std::endl;
        return false;
    }

    return root["status"].asString() == "CANCELED";
}

std::optional<bool> RestClient::cancel_all_orders(const std::string& symbol) {
    std::vector<std::pair<std::string, std::string>> params = {
        {"symbol", symbol}
    };

    auto response = send_signed_request("DELETE", "/api/v3/openOrders", params);
    return response.has_value();
}

std::optional<Order> RestClient::modify_order(
    const std::string& symbol,
    const std::string& order_id,
    double new_price,
    double new_quantity) {

    // OPTIMIZATION: Cancel and replace in single API call if supported
    // For Binance, we need to cancel and place new order, but we can optimize this

    // Format price and quantity with pre-allocated buffers
    char price_buffer[32];
    char quantity_buffer[32];

    snprintf(price_buffer, sizeof(price_buffer), "%.2f", new_price);
    snprintf(quantity_buffer, sizeof(quantity_buffer), "%.5f", new_quantity);

    // Cancel existing order
    std::vector<std::pair<std::string, std::string>> cancel_params = {
        {"symbol", symbol},
        {"orderId", order_id}
    };

    auto cancel_response = send_signed_request("DELETE", "/api/v3/order", cancel_params);

    if (!cancel_response) {
        return std::nullopt;
    }

    // Immediately place new order (reuse connection)
    std::vector<std::pair<std::string, std::string>> new_params = {
        {"symbol", symbol},
        {"side", "BUY"},  // This should be passed as parameter
        {"type", "LIMIT"},
        {"timeInForce", "GTC"},
        {"quantity", quantity_buffer},
        {"price", price_buffer}
    };

    auto new_response = send_signed_request("POST", "/api/v3/order", new_params);

    if (!new_response) {
        return std::nullopt;
    }

    // Parse response
    Json::Reader reader;
    Json::Value root;
    if (!reader.parse(*new_response, root)) {
        return std::nullopt;
    }

    Order order;
    order.order_id = std::to_string(root["orderId"].asInt64());
    order.symbol = root["symbol"].asString();
    order.price = std::stod(root["price"].asString());
    order.quantity = std::stod(root["origQty"].asString());

    return order;
}

// Async cancel order
std::future<std::optional<bool>> RestClient::cancel_order_async(
    const std::string& symbol,
    const std::string& order_id) {

    return std::async(std::launch::async, [this, symbol, order_id]() {
        return cancel_order(symbol, order_id);
    });
}

// Async place limit order
std::future<std::optional<Order>> RestClient::place_limit_order_async(
    const std::string& symbol,
    OrderSide side,
    double price,
    double quantity,
    const std::string& client_order_id) {

    return std::async(std::launch::async, [this, symbol, side, price, quantity, client_order_id]() {
        return place_limit_order(symbol, side, price, quantity, client_order_id);
    });
}

// PARALLEL modify order - cancel and place simultaneously
std::optional<Order> RestClient::modify_order_parallel(
    const std::string& symbol,
    const std::string& order_id,
    OrderSide side,
    double new_price,
    double new_quantity,
    const std::string& client_order_id) {

    // Launch cancel and place operations in parallel using connection pool
    auto cancel_future = cancel_order_async(symbol, order_id);
    auto place_future = place_limit_order_async(symbol, side, new_price, new_quantity, client_order_id);

    // Wait for both operations to complete
    auto cancel_result = cancel_future.get();
    auto place_result = place_future.get();

    // Check if cancel succeeded (optional - we might still want the new order even if cancel failed)
    if (!cancel_result || !cancel_result.value()) {
        std::cerr << "Warning: Cancel order failed during modify_order_parallel" << std::endl;
        // Continue anyway - the new order might still be valid
    }

    // Return the new order result
    return place_result;
}

std::optional<OrderBook> RestClient::get_orderbook(const std::string& symbol, int limit) {
    std::vector<std::pair<std::string, std::string>> params = {
        {"symbol", symbol},
        {"limit", std::to_string(limit)}
    };

    auto response = send_public_request("/api/v3/depth", params);
    if (!response) {
        return std::nullopt;
    }

    Json::Reader reader;
    Json::Value root;
    if (!reader.parse(*response, root)) {
        std::cerr << "Failed to parse orderbook response" << std::endl;
        return std::nullopt;
    }

    OrderBook orderbook;
    orderbook.timestamp = std::chrono::steady_clock::now();

    // Parse bids
    for (const auto& bid : root["bids"]) {
        double price = std::stod(bid[0].asString());
        double quantity = std::stod(bid[1].asString());
        orderbook.bids.emplace_back(price, quantity);
    }

    // Parse asks
    for (const auto& ask : root["asks"]) {
        double price = std::stod(ask[0].asString());
        double quantity = std::stod(ask[1].asString());
        orderbook.asks.emplace_back(price, quantity);
    }

    return orderbook;
}

std::optional<double> RestClient::get_current_price(const std::string& symbol) {
    std::vector<std::pair<std::string, std::string>> params = {
        {"symbol", symbol}
    };

    auto response = send_public_request("/api/v3/ticker/price", params);
    if (!response) {
        return std::nullopt;
    }

    Json::Reader reader;
    Json::Value root;
    if (!reader.parse(*response, root)) {
        return std::nullopt;
    }

    return std::stod(root["price"].asString());
}

std::optional<std::vector<Order>> RestClient::get_open_orders(const std::string& symbol) {
    std::vector<std::pair<std::string, std::string>> params = {
        {"symbol", symbol}
    };

    auto response = send_signed_request("GET", "/api/v3/openOrders", params);
    if (!response) {
        return std::nullopt;
    }

    Json::Reader reader;
    Json::Value root;
    if (!reader.parse(*response, root)) {
        return std::nullopt;
    }

    std::vector<Order> orders;
    for (const auto& order_json : root) {
        Order order;
        order.order_id = order_json["orderId"].asString();
        order.client_order_id = order_json["clientOrderId"].asString();
        order.symbol = order_json["symbol"].asString();
        order.side = order_json["side"].asString() == "BUY" ? OrderSide::BUY : OrderSide::SELL;
        order.price = std::stod(order_json["price"].asString());
        order.quantity = std::stod(order_json["origQty"].asString());
        order.executed_quantity = std::stod(order_json["executedQty"].asString());
        order.status = OrderStatus::NEW;
        orders.push_back(order);
    }

    return orders;
}

std::optional<std::string> RestClient::get_exchange_info() {
    return send_public_request("/api/v3/exchangeInfo", {});
}


bool RestClient::get_symbol_info(const std::string& symbol, int& price_precision, int& quantity_precision) {
    auto response = send_public_request("/api/v3/exchangeInfo", {});
    if (!response) {
        std::cerr << "Failed to get exchange info" << std::endl;
        return false;
    }

    Json::Reader reader;
    Json::Value root;
    if (!reader.parse(*response, root)) {
        std::cerr << "Failed to parse exchange info response" << std::endl;
        return false;
    }

    if (!root.isMember("symbols")) {
        std::cerr << "No symbols in exchange info" << std::endl;
        return false;
    }

    for (const auto& sym : root["symbols"]) {
        if (sym["symbol"].asString() == symbol) {
            // Get base and quote precision
            if (sym.isMember("quotePrecision")) {
                price_precision = sym["quotePrecision"].asInt();
            } else if (sym.isMember("pricePrecision")) {
                price_precision = sym["pricePrecision"].asInt();
            }

            if (sym.isMember("baseAssetPrecision")) {
                quantity_precision = sym["baseAssetPrecision"].asInt();
            } else if (sym.isMember("quantityPrecision")) {
                quantity_precision = sym["quantityPrecision"].asInt();
            }

            // Override with filter values if available
            for (const auto& filter : sym["filters"]) {
                if (filter["filterType"].asString() == "PRICE_FILTER") {
                    std::string tick_size = filter["tickSize"].asString();
                    // Count decimal places from tick size
                    size_t decimal_pos = tick_size.find('.');
                    if (decimal_pos != std::string::npos) {
                        size_t trailing_zeros = tick_size.find_last_not_of('0');
                        if (trailing_zeros > decimal_pos) {
                            price_precision = trailing_zeros - decimal_pos;
                        }
                    }
                }
                if (filter["filterType"].asString() == "LOT_SIZE") {
                    std::string step_size = filter["stepSize"].asString();
                    // Count decimal places from step size
                    size_t decimal_pos = step_size.find('.');
                    if (decimal_pos != std::string::npos) {
                        size_t trailing_zeros = step_size.find_last_not_of('0');
                        if (trailing_zeros > decimal_pos) {
                            quantity_precision = trailing_zeros - decimal_pos;
                        }
                    }
                }
            }

            std::cout << "Symbol " << symbol << " - Price precision: " << price_precision
                      << ", Quantity precision: " << quantity_precision << std::endl;
            return true;
        }
    }

    std::cerr << "Symbol " << symbol << " not found in exchange info" << std::endl;
    return false;
}

} // namespace MarketMaker