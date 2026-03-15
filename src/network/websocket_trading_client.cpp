#include "network/websocket_trading_client.h"
#include "core/app_logger.h"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <websocketpp/common/thread.hpp>
#include <sstream>
#include <iomanip>

namespace {
    quill::Logger* get_logger() {
        static quill::Logger* logger = MarketMaker::AppLogger::get("network");
        return logger;
    }
}

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

WebSocketTradingClient::WebSocketTradingClient(const std::string& api_key, const std::string& api_secret,
                                               int price_precision, int quantity_precision)
    : api_key_(api_key), api_secret_(api_secret),
      price_precision_(price_precision), quantity_precision_(quantity_precision) {

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

            ctx->set_verify_mode(websocketpp::lib::asio::ssl::verify_peer);
            ctx->set_default_verify_paths();

        } catch (std::exception& e) {
            LOG_ERROR(get_logger(), "[SSL] Context setup error: {}", e.what());
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

    LOG_INFO(get_logger(), "{}", "WebSocket Trading Client initialized");
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
            LOG_ERROR(get_logger(), "Connection initialization error: {}", ec.message());
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
            LOG_ERROR(get_logger(), "{}", "Connection timeout after 2 seconds");
            disconnect();
            return false;
        }

        // Start reconnect thread if auto-reconnect is enabled
        if (auto_reconnect_) {
            reconnect_thread_ = std::thread(&WebSocketTradingClient::handle_reconnect, this);
        }

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR(get_logger(), "Connection error: {}", e.what());
        return false;
    }
}

void WebSocketTradingClient::disconnect() {
    if (!running_) {
        return;
    }

    LOG_INFO(get_logger(), "{}", "[WS Trading] Disconnecting...");

    running_ = false;
    auto was_connected = connected_.load();
    connected_ = false;

    // Close WebSocket connection gracefully
    if (ws_client_ && was_connected) {
        try {
            websocketpp::lib::error_code ec;
            ws_client_->close(connection_hdl_, websocketpp::close::status::going_away, "Client disconnect", ec);
            if (ec) {
                LOG_ERROR(get_logger(), "[WS Trading] Close error: {}", ec.message());
            }
            // Give time for close handshake
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } catch (const std::exception& e) {
            LOG_ERROR(get_logger(), "[WS Trading] Exception during close: {}", e.what());
        }
    }

    // Join user_data_thread_ before stopping event loop — it calls send_request_and_wait
    // which needs the event loop running. Must join before ws_client_->stop().
    if (user_data_thread_.joinable()) {
        LOG_DEBUG(get_logger(), "{}", "[WS Trading] Waiting for user data subscribe thread...");
        user_data_thread_.join();
    }

    // Stop the client
    if (ws_client_) {
        try {
            ws_client_->stop();
        } catch (const std::exception& e) {
            LOG_ERROR(get_logger(), "[WS Trading] Exception during stop: {}", e.what());
        }
    }

    // Wait for threads with timeout
    if (ws_thread_.joinable()) {
        LOG_DEBUG(get_logger(), "{}", "[WS Trading] Waiting for event loop thread...");
        ws_thread_.join();
    }

    if (reconnect_thread_.joinable()) {
        LOG_DEBUG(get_logger(), "{}", "[WS Trading] Waiting for reconnect thread...");
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

    LOG_INFO(get_logger(), "{}", "[WS Trading] Disconnected cleanly");
}

void WebSocketTradingClient::run_event_loop() {
    try {
        ws_client_->run();
    } catch (const std::exception& e) {
        LOG_ERROR(get_logger(), "Event loop error: {}", e.what());
    }
}

void WebSocketTradingClient::handle_reconnect() {
    while (running_ && auto_reconnect_) {
        if (!connected_) {
            std::this_thread::sleep_for(reconnect_delay_);

            if (!running_) break;

            LOG_INFO(get_logger(), "{}", "Attempting to reconnect...");

            // Try to reconnect
            // Note: This is simplified - in production you'd want to recreate the connection properly
            connected_ = false;

            // Guard: only invoke connection_handler_ while still running to avoid
            // calling into a partially-destroyed MarketMakerBot during shutdown.
            if (running_ && connection_handler_) {
                connection_handler_(false);
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void WebSocketTradingClient::on_open([[maybe_unused]] websocketpp::connection_hdl hdl) {
    LOG_INFO(get_logger(), "{}", "WebSocket Trading connection opened");
    connected_ = true;

    // Auto-subscribe to user data stream if callbacks are set
    // Must dispatch to separate thread — send_request_and_wait blocks, and
    // on_open runs on the ASIO event loop thread that delivers responses
    {
        std::lock_guard<std::mutex> lock(user_data_mutex_);
        if (fill_callback_ || balance_callback_) {
            // Join any previous user_data_thread_ before spawning a new one
            if (user_data_thread_.joinable()) {
                user_data_thread_.join();
            }
            user_data_thread_ = std::thread([this]() { subscribe_user_data_stream(); });
        }
    }

    if (connection_handler_) {
        connection_handler_(true);
    }
}

void WebSocketTradingClient::on_close([[maybe_unused]] websocketpp::connection_hdl hdl) {
    LOG_INFO(get_logger(), "{}", "WebSocket Trading connection closed");
    connected_ = false;

    // Only notify upstream if the client is still running (not during shutdown).
    // During shutdown running_ is false; the disconnect() caller handles notification.
    if (running_ && connection_handler_) {
        connection_handler_(false);
    }
}

void WebSocketTradingClient::on_fail([[maybe_unused]] websocketpp::connection_hdl hdl) {
    LOG_ERROR(get_logger(), "{}", "WebSocket Trading connection failed");
    connected_ = false;

    if (running_ && connection_handler_) {
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
        LOG_ERROR(get_logger(), "Failed to parse WebSocket message: {}", message.substr(0, 200));
        return;
    }

    // User data stream events: no "id" field, wrapped in {"event": {...}, "subscriptionId": N}
    if (!response.isMember("id") && response.isMember("event")) {
        handle_user_data_event(response["event"]);
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

        LOG_DEBUG(get_logger(), "Order successful - ID: {} Side: {} Price: {}",
                  result["orderId"].asString(),
                  result.isMember("side") ? result["side"].asString() : "N/A",
                  result.isMember("price") ? result["price"].asString() : "N/A");
    } else if (result.isMember("status") && result["status"].asString() == "CANCELED") {
        metrics_.cancelled_orders++;
        LOG_INFO(get_logger(), "{}", "Order cancelled successfully");
    }
}

void WebSocketTradingClient::handle_error_response(const Json::Value& response) {
    if (!response.isMember("error")) {
        return;
    }

    metrics_.failed_orders++;

    const Json::Value& error = response["error"];
    LOG_ERROR(get_logger(), "WebSocket API Error - Code: {} Message: {}",
              error["code"].asInt(), error["msg"].asString());

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
        LOG_ERROR(get_logger(), "{}", "Not connected to WebSocket");
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
        LOG_ERROR(get_logger(), "Failed to send WebSocket message: {}", ec.message());

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
        LOG_ERROR(get_logger(), "Request timeout for method: {}", method);

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
        LOG_ERROR(get_logger(), "Failed to send async WebSocket message: {}", ec.message());
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
    params["price"] = format_price(price, price_precision_);
    params["quantity"] = format_quantity(quantity, quantity_precision_);

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

std::optional<std::string> WebSocketTradingClient::place_ioc_order(
    const std::string& symbol,
    OrderSide side,
    double price,
    double quantity,
    const std::string& client_order_id) {

    Json::Value params;
    params["symbol"] = symbol;
    params["side"] = (side == OrderSide::BUY) ? "BUY" : "SELL";
    params["type"] = "LIMIT";
    params["timeInForce"] = "IOC";
    params["price"] = format_price(price, price_precision_);
    params["quantity"] = format_quantity(quantity, quantity_precision_);

    if (!client_order_id.empty()) {
        params["newClientOrderId"] = client_order_id;
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

std::optional<std::string> WebSocketTradingClient::place_market_order(
    const std::string& symbol,
    OrderSide side,
    double quantity,
    const std::string& client_order_id) {

    Json::Value params;
    params["symbol"] = symbol;
    params["side"] = (side == OrderSide::BUY) ? "BUY" : "SELL";
    params["type"] = "MARKET";
    params["quantity"] = format_quantity(quantity, quantity_precision_);

    if (!client_order_id.empty()) {
        params["newClientOrderId"] = client_order_id;
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

std::optional<std::string> WebSocketTradingClient::cancel_replace_order(
    const std::string& symbol,
    const std::string& cancel_order_id,
    OrderSide side,
    double price,
    double quantity,
    const std::string& client_order_id) {

    Json::Value params;
    params["symbol"] = symbol;
    params["side"] = (side == OrderSide::BUY) ? "BUY" : "SELL";
    params["type"] = "LIMIT";
    params["cancelReplaceMode"] = "STOP_ON_FAILURE";
    params["timeInForce"] = "GTC";
    params["cancelOrderId"] = Json::Int64(std::stoll(cancel_order_id));
    params["price"] = format_price(price, price_precision_);
    params["quantity"] = format_quantity(quantity, quantity_precision_);

    if (!client_order_id.empty()) {
        params["newClientOrderId"] = client_order_id;
    }

    auto response = send_request_and_wait("order.cancelReplace", params);

    if (!response || !response->isMember("result")) {
        return std::nullopt;
    }

    const Json::Value& result = (*response)["result"];
    if (result.isMember("newOrderResponse") && result["newOrderResponse"].isMember("orderId")) {
        return result["newOrderResponse"]["orderId"].asString();
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

void WebSocketTradingClient::set_fill_callback(FillCallback callback) {
    std::lock_guard<std::mutex> lock(user_data_mutex_);
    fill_callback_ = std::move(callback);
}

void WebSocketTradingClient::set_balance_callback(BalanceCallback callback) {
    std::lock_guard<std::mutex> lock(user_data_mutex_);
    balance_callback_ = std::move(callback);
}

bool WebSocketTradingClient::subscribe_user_data_stream() {
    if (!connected_) {
        LOG_ERROR(get_logger(), "{}", "[USER_STREAM] Cannot subscribe - not connected");
        return false;
    }

    // Use userDataStream.subscribe.signature (supports HMAC-SHA256)
    Json::Value params;
    auto response = send_request_and_wait("userDataStream.subscribe.signature", params);

    if (!response) {
        LOG_ERROR(get_logger(), "{}", "[USER_STREAM] Subscribe request failed/timed out");
        return false;
    }

    if (response->isMember("error")) {
        const auto& error = (*response)["error"];
        LOG_ERROR(get_logger(), "[USER_STREAM] Subscribe error - code: {} msg: {}",
                  error["code"].asInt(), error["msg"].asString());
        return false;
    }

    LOG_INFO(get_logger(), "{}", "[USER_STREAM] Subscribed to user data stream via WS API");
    return true;
}

void WebSocketTradingClient::handle_user_data_event(const Json::Value& event) {
    std::string event_type = event.get("e", "").asString();

    if (event_type == "executionReport") {
        std::string order_id = event.isMember("i")
            ? std::to_string(event["i"].asInt64()) : "";
        std::string client_order_id = event.get("c", "").asString();
        std::string side_str = event.get("S", "").asString();
        std::string status_str = event.get("X", "").asString();

        double price = 0.0, qty = 0.0, cum_qty = 0.0;
        try {
            price = std::stod(event.get("L", "0").asString());
            qty = std::stod(event.get("l", "0").asString());
            cum_qty = std::stod(event.get("z", "0").asString());
        } catch (const std::exception& e) {
            LOG_ERROR(get_logger(), "[USER_STREAM] Failed to parse fill data: {}", e.what());
            return;
        }

        OrderSide side = (side_str == "BUY") ? OrderSide::BUY : OrderSide::SELL;

        OrderStatus status = OrderStatus::NEW;
        if (status_str == "FILLED") status = OrderStatus::FILLED;
        else if (status_str == "PARTIALLY_FILLED") status = OrderStatus::PARTIALLY_FILLED;
        else if (status_str == "CANCELED") status = OrderStatus::CANCELED;
        else if (status_str == "REJECTED") status = OrderStatus::REJECTED;
        else if (status_str == "EXPIRED") status = OrderStatus::EXPIRED;

        LOG_INFO(get_logger(), "[USER_STREAM] ExecutionReport: {} {} price={} qty={} cum_qty={}",
                 side_str, status_str, price, qty, cum_qty);

        // Copy-then-invoke to avoid holding lock during callback (prevents lock inversion)
        FillCallback cb;
        { std::lock_guard<std::mutex> lock(user_data_mutex_); cb = fill_callback_; }
        if (cb) {
            cb(order_id, client_order_id, side, status, price, qty, cum_qty);
        }
    } else if (event_type == "outboundAccountPosition") {
        auto balances = event["B"];
        if (balances.isArray()) {
            BalanceCallback cb;
            { std::lock_guard<std::mutex> lock(user_data_mutex_); cb = balance_callback_; }
            if (!cb) return;
            for (const auto& bal : balances) {
                std::string asset = bal.get("a", "").asString();
                double free_bal = 0.0, locked_bal = 0.0;
                try {
                    free_bal = std::stod(bal.get("f", "0").asString());
                    locked_bal = std::stod(bal.get("l", "0").asString());
                } catch (...) {
                    continue;
                }
                cb(asset, free_bal, locked_bal);
            }
        }
    }
}

} // namespace MarketMaker