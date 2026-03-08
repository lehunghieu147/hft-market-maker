#ifndef USER_DATA_STREAM_H
#define USER_DATA_STREAM_H

#include "core/types.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

namespace MarketMaker {

// Callback types for user data events
using FillCallback = std::function<void(
    const std::string& order_id,
    const std::string& client_order_id,
    OrderSide side,
    OrderStatus status,
    double price,
    double quantity,
    double cumulative_quantity
)>;

using BalanceCallback = std::function<void(
    const std::string& asset,
    double free_balance,
    double locked_balance
)>;

class UserDataStream {
public:
    UserDataStream(const std::string& api_key,
                   const std::string& api_secret,
                   const std::string& rest_base_url,
                   const std::string& ws_base_url);
    ~UserDataStream();

    // Set event callbacks
    void set_fill_callback(FillCallback callback);
    void set_balance_callback(BalanceCallback callback);

    // Lifecycle
    bool start();
    void stop();
    bool is_connected() const { return connected_.load(); }

private:
    std::string api_key_;
    std::string api_secret_;
    std::string rest_base_url_;
    std::string ws_base_url_;

    std::string listen_key_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    // Callbacks
    FillCallback fill_callback_;
    BalanceCallback balance_callback_;
    std::mutex callback_mutex_;

    // Threads
    std::thread stream_thread_;
    std::thread keepalive_thread_;

    // SSL resources
    void* ssl_ctx_ = nullptr;
    void* ssl_ = nullptr;
    int socket_fd_ = -1;
    std::mutex ssl_mutex_;

    // Listen key management (REST API)
    bool create_listen_key();
    bool keepalive_listen_key();
    void delete_listen_key();

    // WebSocket connection
    void stream_loop();
    void keepalive_loop();
    bool connect_websocket();
    void process_message(const std::string& message);

    // WebSocket frame helpers
    bool send_pong(const std::vector<uint8_t>& payload);
    std::string read_websocket_frame(std::vector<uint8_t>& ping_payload, bool& is_ping);
};

} // namespace MarketMaker

#endif // USER_DATA_STREAM_H
