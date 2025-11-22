#ifndef WEBSOCKET_TRADING_CLIENT_H
#define WEBSOCKET_TRADING_CLIENT_H

#include "types.h"
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <string>
#include <memory>
#include <optional>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <chrono>
#include <json/json.h>

namespace MarketMaker {

using WsClient = websocketpp::client<websocketpp::config::asio_tls_client>;
using WsMessagePtr = websocketpp::config::asio_client::message_type::ptr;
using WsConnectionPtr = websocketpp::client<websocketpp::config::asio_tls_client>::connection_ptr;

class WebSocketTradingClient {
public:
    using OrderResponseHandler = std::function<void(const Json::Value&)>;
    using ConnectionHandler = std::function<void(bool)>;
    using ErrorHandler = std::function<void(const std::string&)>;

    WebSocketTradingClient(const std::string& api_key, const std::string& api_secret);
    ~WebSocketTradingClient();

    // Connection management
    bool connect(const std::string& url);
    void disconnect();
    bool is_connected() const { return connected_.load(); }

    // Auto-reconnect settings
    void enable_auto_reconnect(bool enable) { auto_reconnect_ = enable; }
    void set_reconnect_delay(std::chrono::milliseconds delay) { reconnect_delay_ = delay; }

    // Order operations via WebSocket
    std::optional<std::string> place_limit_order(
        const std::string& symbol,
        OrderSide side,
        double price,
        double quantity,
        const std::string& client_order_id = "",
        bool wait_for_response = true
    );

    std::optional<bool> cancel_order(
        const std::string& symbol,
        const std::string& order_id,
        bool wait_for_response = true
    );

    std::optional<bool> cancel_all_orders(
        const std::string& symbol,
        bool wait_for_response = true
    );

    std::optional<Json::Value> query_order(
        const std::string& symbol,
        const std::string& order_id,
        bool wait_for_response = true
    );

    std::optional<Json::Value> get_open_orders(
        const std::string& symbol,
        bool wait_for_response = true
    );

    // Batch operations for efficiency
    void place_orders_batch(
        const std::vector<std::tuple<std::string, OrderSide, double, double>>& orders,
        OrderResponseHandler handler
    );

    // Set handlers
    void set_order_response_handler(OrderResponseHandler handler) {
        order_response_handler_ = handler;
    }

    void set_connection_handler(ConnectionHandler handler) {
        connection_handler_ = handler;
    }

    void set_error_handler(ErrorHandler handler) {
        error_handler_ = handler;
    }

    // Metrics
    struct TradingMetrics {
        std::atomic<uint64_t> total_requests{0};
        std::atomic<uint64_t> successful_orders{0};
        std::atomic<uint64_t> failed_orders{0};
        std::atomic<uint64_t> cancelled_orders{0};
        std::atomic<double> avg_response_time_ms{0};
        std::atomic<double> min_response_time_ms{999999};
        std::atomic<double> max_response_time_ms{0};

        void update_response_time(double time_ms);
    };

    const TradingMetrics& get_metrics() const { return metrics_; }

private:
    // WebSocket client
    std::unique_ptr<WsClient> ws_client_;
    websocketpp::connection_hdl connection_hdl_;

    // Authentication
    std::string api_key_;
    std::string api_secret_;

    // Connection state
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    bool auto_reconnect_{true};
    std::chrono::milliseconds reconnect_delay_{5000};

    // Request tracking
    struct PendingRequest {
        std::string method;
        std::chrono::steady_clock::time_point sent_time;
        std::promise<Json::Value> promise;
        bool waiting{true};
    };

    std::mutex requests_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingRequest>> pending_requests_;
    std::atomic<uint64_t> request_id_counter_{1};

    // Handlers
    OrderResponseHandler order_response_handler_;
    ConnectionHandler connection_handler_;
    ErrorHandler error_handler_;

    // Metrics
    mutable TradingMetrics metrics_;

    // Threads
    std::thread ws_thread_;
    std::thread reconnect_thread_;

    // Internal methods
    void run_event_loop();
    void handle_reconnect();

    // WebSocket callbacks
    void on_open(websocketpp::connection_hdl hdl);
    void on_close(websocketpp::connection_hdl hdl);
    void on_fail(websocketpp::connection_hdl hdl);
    void on_message(websocketpp::connection_hdl hdl, WsMessagePtr msg);

    // Message handling
    void process_message(const std::string& message);
    void handle_order_response(const Json::Value& response);
    void handle_error_response(const Json::Value& response);

    // Request management
    std::string generate_request_id();
    Json::Value create_signed_request(
        const std::string& method,
        const Json::Value& params
    );

    std::optional<Json::Value> send_request_and_wait(
        const std::string& method,
        const Json::Value& params,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)
    );

    void send_request_async(
        const std::string& method,
        const Json::Value& params,
        std::function<void(const Json::Value&)> callback = nullptr
    );

    // Authentication
    std::string generate_signature(const std::string& query_string);
    int64_t get_timestamp();

    // Utility
    std::string format_price(double price, int precision = 2);
    std::string format_quantity(double quantity, int precision = 5);
};

} // namespace MarketMaker

#endif // WEBSOCKET_TRADING_CLIENT_H