#include "websocket_trading_client.h"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <websocketpp/common/thread.hpp>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace MarketMaker {

void WebSocketTradingClient::TradingMetrics::update_response_time(double time_ms) {
    total_requests++;

    // Update min/max
    double current_min = min_response_time_ms.load();
    while (time_ms < current_min && !min_response_time_ms.compare_exchange_weak(current_min, time_ms));

    double current_max = max_response_time_ms.load();
    while (time_ms > current_max && !max_response_time_ms.compare_exchange_weak(current_max, time_ms));

    // Update average (simple moving average)
    double current_avg = avg_response_time_ms.load();
    double new_avg = (current_avg * (total_requests - 1) + time_ms) / total_requests;
    avg_response_time_ms = new_avg;
}

WebSocketTradingClient::WebSocketTradingClient(const std::string& api_key, const std::string& api_secret)
    : api_key_(api_key), api_secret_(api_secret) {

    ws_client_ = std::make_unique<WsClient>();

    // Initialize WebSocket client
    ws_client_->init_asio();
    ws_client_->set_reuse_addr(true);  // Allow socket reuse
    ws_client_->set_access_channels(websocketpp::log::alevel::none);
    ws_client_->set_error_channels(websocketpp::log::elevel::all);

    // Set up TLS handler with proper SSL initialization
    ws_client_->set_tls_init_handler([](websocketpp::connection_hdl) {
        auto ctx = websocketpp::lib::make_shared<websocketpp::lib::asio::ssl::context>(
            websocketpp::lib::asio::ssl::context::tlsv12_client);

        try {
            ctx->set_options(
                websocketpp::lib::asio::ssl::context::default_workarounds |
                websocketpp::lib::asio::ssl::context::no_sslv2 |
                websocketpp::lib::asio::ssl::context::no_sslv3 |
                websocketpp::lib::asio::ssl::context::single_dh_use
            );

            ctx->set_verify_mode(websocketpp::lib::asio::ssl::verify_none);

        } catch (std::exception& e) {
            std::cerr << "[SSL] Context setup error: " << e.what() << std::endl;
        }

        return ctx;
    });

    // Set up connection handlers
    ws_client_->set_open_handler([this](websocketpp::connection_hdl hdl) {
        this->on_open(hdl);
    });

    ws_client_->set_close_handler([this](websocketpp::connection_hdl hdl) {
        this->on_close(hdl);
    });

    ws_client_->set_fail_handler([this](websocketpp::connection_hdl hdl) {
        this->on_fail(hdl);
    });

    ws_client_->set_message_handler([this](websocketpp::connection_hdl hdl, WsMessagePtr msg) {
        this->on_message(hdl, msg);
    });

    std::cout << "WebSocket Trading Client initialized with API Key: "
              << api_key_.substr(0, 10) << "..." << std::endl;
}

WebSocketTradingClient::~WebSocketTradingClient() {
    disconnect();
}

bool WebSocketTradingClient::connect(const std::string& url) {
    if (connected_) {
        return true;
    }

    try {
        websocketpp::lib::error_code ec;
        auto con = ws_client_->get_connection(url, ec);

        if (ec) {
            std::cerr << "Connection initialization error: " << ec.message() << std::endl;
            return false;
        }

        connection_hdl_ = con->get_handle();
        ws_client_->connect(con);

        // Start the event loop
        running_ = true;
        ws_thread_ = std::thread(&WebSocketTradingClient::run_event_loop, this);

        // Wait for connection with longer timeout
        auto start = std::chrono::steady_clock::now();
        while (!connected_ &&
               std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!connected_) {
            std::cerr << "Connection timeout after 15 seconds" << std::endl;
            disconnect();
            return false;
        }

        // Start reconnect thread if auto-reconnect is enabled
        if (auto_reconnect_) {
            reconnect_thread_ = std::thread(&WebSocketTradingClient::handle_reconnect, this);
        }

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Connection error: " << e.what() << std::endl;
        return false;
    }
}

void WebSocketTradingClient::disconnect() {
    if (!running_) {
        return;
    }

    std::cout << "[WS Trading] Disconnecting..." << std::endl;

    running_ = false;
    auto was_connected = connected_.load();
    connected_ = false;

    // Close WebSocket connection gracefully
    if (ws_client_ && was_connected) {
        try {
            websocketpp::lib::error_code ec;
            ws_client_->close(connection_hdl_, websocketpp::close::status::going_away, "Client disconnect", ec);
            if (ec) {
                std::cerr << "[WS Trading] Close error: " << ec.message() << std::endl;
            }
            // Give time for close handshake
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } catch (const std::exception& e) {
            std::cerr << "[WS Trading] Exception during close: " << e.what() << std::endl;
        }
    }

    // Stop the client
    if (ws_client_) {
        try {
            ws_client_->stop();
        } catch (const std::exception& e) {
            std::cerr << "[WS Trading] Exception during stop: " << e.what() << std::endl;
        }
    }

    // Wait for threads with timeout
    if (ws_thread_.joinable()) {
        std::cout << "[WS Trading] Waiting for event loop thread..." << std::endl;
        ws_thread_.join();
    }

    if (reconnect_thread_.joinable()) {
        std::cout << "[WS Trading] Waiting for reconnect thread..." << std::endl;
        reconnect_thread_.join();
    }

    // Clear pending requests
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        for (auto& [id, request] : pending_requests_) {
            if (request && request->waiting) {
                Json::Value error;
                error["error"] = "Connection closed";
                request->promise.set_value(error);
            }
        }
        pending_requests_.clear();
    }

    std::cout << "[WS Trading] Disconnected cleanly" << std::endl;
}

void WebSocketTradingClient::run_event_loop() {
    try {
        ws_client_->run();
    } catch (const std::exception& e) {
        std::cerr << "Event loop error: " << e.what() << std::endl;
    }
}

void WebSocketTradingClient::handle_reconnect() {
    while (running_ && auto_reconnect_) {
        if (!connected_) {
            std::this_thread::sleep_for(reconnect_delay_);

            if (!running_) break;

            std::cout << "Attempting to reconnect..." << std::endl;

            // Try to reconnect
            // Note: This is simplified - in production you'd want to recreate the connection properly
            connected_ = false;

            if (connection_handler_) {
                connection_handler_(false);
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void WebSocketTradingClient::on_open([[maybe_unused]] websocketpp::connection_hdl hdl) {
    std::cout << "WebSocket Trading connection opened" << std::endl;
    connected_ = true;

    if (connection_handler_) {
        connection_handler_(true);
    }
}

void WebSocketTradingClient::on_close([[maybe_unused]] websocketpp::connection_hdl hdl) {
    std::cout << "WebSocket Trading connection closed" << std::endl;
    connected_ = false;

    if (connection_handler_) {
        connection_handler_(false);
    }
}

void WebSocketTradingClient::on_fail([[maybe_unused]] websocketpp::connection_hdl hdl) {
    std::cerr << "WebSocket Trading connection failed" << std::endl;
    connected_ = false;

    if (connection_handler_) {
        connection_handler_(false);
    }
}

void WebSocketTradingClient::on_message([[maybe_unused]] websocketpp::connection_hdl hdl, WsMessagePtr msg) {
    process_message(msg->get_payload());
}

void WebSocketTradingClient::process_message(const std::string& message) {
    Json::Reader reader;
    Json::Value response;

    if (!reader.parse(message, response)) {
        std::cerr << "Failed to parse WebSocket message: " << message << std::endl;
        return;
    }

    // Check if this is a response to a request
    if (response.isMember("id")) {
        std::string request_id = response["id"].asString();

        std::lock_guard<std::mutex> lock(requests_mutex_);
        auto it = pending_requests_.find(request_id);
        if (it != pending_requests_.end()) {
            auto request = it->second;

            // Calculate response time
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - request->sent_time
            );
            metrics_.update_response_time(duration.count());

            // Set the promise value
            if (request->waiting) {
                request->promise.set_value(response);
                request->waiting = false;
            }

            // Handle the response
            if (response.isMember("result")) {
                handle_order_response(response);
            } else if (response.isMember("error")) {
                handle_error_response(response);
            }

            pending_requests_.erase(it);
        }
    }

    // Also send to general handler if set
    if (order_response_handler_) {
        order_response_handler_(response);
    }
}

void WebSocketTradingClient::handle_order_response(const Json::Value& response) {
    if (!response.isMember("result")) {
        return;
    }

    const Json::Value& result = response["result"];

    // Check if this is an order response
    if (result.isMember("orderId")) {
        metrics_.successful_orders++;

        std::cout << "Order successful - ID: " << result["orderId"].asString();
        if (result.isMember("side")) {
            std::cout << ", Side: " << result["side"].asString();
        }
        if (result.isMember("price")) {
            std::cout << ", Price: " << result["price"].asString();
        }
        std::cout << std::endl;
    } else if (result.isMember("status") && result["status"].asString() == "CANCELED") {
        metrics_.cancelled_orders++;
        std::cout << "Order cancelled successfully" << std::endl;
    }
}

void WebSocketTradingClient::handle_error_response(const Json::Value& response) {
    if (!response.isMember("error")) {
        return;
    }

    metrics_.failed_orders++;

    const Json::Value& error = response["error"];
    std::cerr << "WebSocket API Error - Code: " << error["code"].asInt()
              << ", Message: " << error["msg"].asString() << std::endl;

    if (error_handler_) {
        error_handler_(error["msg"].asString());
    }
}

std::string WebSocketTradingClient::generate_request_id() {
    return "req_" + std::to_string(request_id_counter_++);
}

std::string WebSocketTradingClient::generate_signature(const std::string& query_string) {
    unsigned char digest[32];
    unsigned int digest_len = 32;

    HMAC(EVP_sha256(), api_secret_.c_str(), api_secret_.length(),
         (unsigned char*)query_string.c_str(), query_string.length(),
         digest, &digest_len);

    std::stringstream ss;
    for (unsigned int i = 0; i < digest_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    }
    return ss.str();
}

int64_t WebSocketTradingClient::get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

Json::Value WebSocketTradingClient::create_signed_request(
    const std::string& method,
    const Json::Value& params) {

    Json::Value request;
    request["id"] = generate_request_id();
    request["method"] = method;

    // Create a copy of params and add authentication
    Json::Value signed_params = params;
    signed_params["apiKey"] = api_key_;
    signed_params["timestamp"] = Json::Int64(get_timestamp());

    // Build query string for signature
    std::stringstream query_stream;
    bool first = true;
    for (const auto& key : signed_params.getMemberNames()) {
        if (!first) query_stream << "&";
        query_stream << key << "=" << signed_params[key].asString();
        first = false;
    }

    std::string query_string = query_stream.str();
    std::string signature = generate_signature(query_string);

    signed_params["signature"] = signature;
    request["params"] = signed_params;

    return request;
}

std::optional<Json::Value> WebSocketTradingClient::send_request_and_wait(
    const std::string& method,
    const Json::Value& params,
    std::chrono::milliseconds timeout) {

    if (!connected_) {
        std::cerr << "Not connected to WebSocket" << std::endl;
        return std::nullopt;
    }

    Json::Value request = create_signed_request(method, params);
    std::string request_id = request["id"].asString();

    // Create pending request
    auto pending = std::make_shared<PendingRequest>();
    pending->method = method;
    pending->sent_time = std::chrono::steady_clock::now();

    // Store the pending request
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        pending_requests_[request_id] = pending;
    }

    // Send the request
    Json::FastWriter writer;
    std::string message = writer.write(request);

    websocketpp::lib::error_code ec;
    ws_client_->send(connection_hdl_, message, websocketpp::frame::opcode::text, ec);

    if (ec) {
        std::cerr << "Failed to send WebSocket message: " << ec.message() << std::endl;

        // Remove pending request
        {
            std::lock_guard<std::mutex> lock(requests_mutex_);
            pending_requests_.erase(request_id);
        }

        return std::nullopt;
    }

    // Wait for response
    auto future = pending->promise.get_future();
    if (future.wait_for(timeout) == std::future_status::timeout) {
        std::cerr << "Request timeout for method: " << method << std::endl;

        // Mark as not waiting and remove
        {
            std::lock_guard<std::mutex> lock(requests_mutex_);
            pending->waiting = false;
            pending_requests_.erase(request_id);
        }

        return std::nullopt;
    }

    return future.get();
}

void WebSocketTradingClient::send_request_async(
    const std::string& method,
    const Json::Value& params,
    std::function<void(const Json::Value&)> callback) {

    if (!connected_) {
        if (callback) {
            Json::Value error;
            error["error"] = "Not connected";
            callback(error);
        }
        return;
    }

    Json::Value request = create_signed_request(method, params);

    // Send the request without waiting
    Json::FastWriter writer;
    std::string message = writer.write(request);

    websocketpp::lib::error_code ec;
    ws_client_->send(connection_hdl_, message, websocketpp::frame::opcode::text, ec);

    if (ec) {
        std::cerr << "Failed to send async WebSocket message: " << ec.message() << std::endl;
        if (callback) {
            Json::Value error;
            error["error"] = ec.message();
            callback(error);
        }
    }
}

std::optional<std::string> WebSocketTradingClient::place_limit_order(
    const std::string& symbol,
    OrderSide side,
    double price,
    double quantity,
    const std::string& client_order_id,
    bool wait_for_response) {

    Json::Value params;
    params["symbol"] = symbol;
    params["side"] = (side == OrderSide::BUY) ? "BUY" : "SELL";
    params["type"] = "LIMIT";
    params["timeInForce"] = "GTC";
    params["price"] = format_price(price);
    params["quantity"] = format_quantity(quantity);

    if (!client_order_id.empty()) {
        params["newClientOrderId"] = client_order_id;
    }

    if (!wait_for_response) {
        send_request_async("order.place", params);
        return "async_request_sent";
    }

    auto response = send_request_and_wait("order.place", params);

    if (!response || !response->isMember("result")) {
        return std::nullopt;
    }

    const Json::Value& result = (*response)["result"];
    if (result.isMember("orderId")) {
        return result["orderId"].asString();
    }

    return std::nullopt;
}

std::optional<bool> WebSocketTradingClient::cancel_order(
    const std::string& symbol,
    const std::string& order_id,
    bool wait_for_response) {

    Json::Value params;
    params["symbol"] = symbol;
    params["orderId"] = Json::Int64(std::stoll(order_id));

    if (!wait_for_response) {
        send_request_async("order.cancel", params);
        return true;
    }

    auto response = send_request_and_wait("order.cancel", params);

    if (!response || !response->isMember("result")) {
        return false;
    }

    const Json::Value& result = (*response)["result"];
    return result.isMember("status") && result["status"].asString() == "CANCELED";
}

std::optional<bool> WebSocketTradingClient::cancel_all_orders(
    const std::string& symbol,
    bool wait_for_response) {

    Json::Value params;
    params["symbol"] = symbol;

    if (!wait_for_response) {
        send_request_async("openOrders.cancelAll", params);
        return true;
    }

    auto response = send_request_and_wait("openOrders.cancelAll", params);

    return response.has_value();
}

std::optional<Json::Value> WebSocketTradingClient::query_order(
    const std::string& symbol,
    const std::string& order_id,
    bool wait_for_response) {

    Json::Value params;
    params["symbol"] = symbol;
    params["orderId"] = Json::Int64(std::stoll(order_id));

    if (!wait_for_response) {
        send_request_async("order.status", params);
        return Json::Value();
    }

    auto response = send_request_and_wait("order.status", params);

    if (!response || !response->isMember("result")) {
        return std::nullopt;
    }

    return (*response)["result"];
}

std::optional<Json::Value> WebSocketTradingClient::get_open_orders(
    const std::string& symbol,
    bool wait_for_response) {

    Json::Value params;
    params["symbol"] = symbol;

    if (!wait_for_response) {
        send_request_async("openOrders.status", params);
        return Json::Value();
    }

    auto response = send_request_and_wait("openOrders.status", params);

    if (!response || !response->isMember("result")) {
        return std::nullopt;
    }

    return (*response)["result"];
}

void WebSocketTradingClient::place_orders_batch(
    const std::vector<std::tuple<std::string, OrderSide, double, double>>& orders,
    [[maybe_unused]] OrderResponseHandler handler) {

    // Send all orders asynchronously for maximum speed
    for (const auto& [symbol, side, price, quantity] : orders) {
        place_limit_order(symbol, side, price, quantity, "", false);
    }
}

std::string WebSocketTradingClient::format_price(double price, int precision) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(precision) << price;
    return ss.str();
}

std::string WebSocketTradingClient::format_quantity(double quantity, int precision) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(precision) << quantity;
    return ss.str();
}

} // namespace MarketMaker