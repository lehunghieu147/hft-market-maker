#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include "types.h"
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>

// Forward declarations for WebSocket++ library
namespace websocketpp {
    template <typename config> class client;
    namespace config {
        struct asio_tls_client;
    }
}

namespace MarketMaker {

class WebSocketClient {
public:
    using MessageHandler = std::function<void(const std::string&)>;
    using ConnectionHandler = std::function<void(bool)>;  // true = connected, false = disconnected

    WebSocketClient();
    ~WebSocketClient();

    // Connection management
    bool connect(const std::string& uri);
    void disconnect();
    bool is_connected() const;

    // Subscribe to market data
    void subscribe_orderbook(const std::string& symbol, int depth = 20);
    void subscribe_trades(const std::string& symbol);

    // Event handlers
    void set_message_handler(MessageHandler handler);
    void set_connection_handler(ConnectionHandler handler);

    // Auto-reconnect
    void enable_auto_reconnect(bool enable = true);
    void set_reconnect_delay(std::chrono::milliseconds delay);

private:
    class Impl;  // PIMPL idiom for WebSocket++ dependencies
    std::unique_ptr<Impl> pImpl;

    std::atomic<bool> connected_{false};
    std::atomic<bool> auto_reconnect_{true};
    std::atomic<bool> should_run_{true};

    std::chrono::milliseconds reconnect_delay_{5000};
    std::string current_uri_;

    MessageHandler message_handler_;
    ConnectionHandler connection_handler_;

    std::thread worker_thread_;
    std::thread reconnect_thread_;
    std::thread heartbeat_thread_;

    std::chrono::steady_clock::time_point last_message_time_;
    std::mutex last_message_mutex_;

    void run_worker();
    void handle_reconnect();
    void process_message(const std::string& message);
    void run_heartbeat();
    void send_ping();
};

} // namespace MarketMaker

#endif // WEBSOCKET_CLIENT_H