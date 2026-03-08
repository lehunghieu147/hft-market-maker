#ifndef MOMENTUM_TAKER_H
#define MOMENTUM_TAKER_H

#include "core/config.h"
#include "core/types.h"
#include "exchange/exchange_interface.h"
#include "trading/order_manager.h"
#include "trading/risk_manager.h"
#include "trading/signal_engine.h"
#include "trading/latency_tracker.h"
#include "network/user_data_stream.h"
#include "quill/Logger.h"
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace MarketMaker {

class MomentumTakerBot {
public:
    explicit MomentumTakerBot(const Config& config);
    ~MomentumTakerBot();

    bool initialize();
    void run();
    void stop();

    bool is_running() const { return running_; }
    LatencyMetrics get_metrics() const;

private:
    Config config_;

    // Core components
    std::shared_ptr<IExchange> exchange_;
    std::shared_ptr<OrderManager> order_manager_;
    std::shared_ptr<RiskManager> risk_manager_;
    std::unique_ptr<SignalEngine> signal_engine_;
    std::unique_ptr<LatencyTracker> latency_tracker_;
    std::unique_ptr<UserDataStream> user_data_stream_;
    quill::Logger* quill_logger_ = nullptr;

    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};

    // Signal state (written by orderbook callback, read by main_loop)
    struct SignalState {
        Signal signal = Signal::NONE;
        double best_bid = 0.0;
        double best_ask = 0.0;
        std::chrono::steady_clock::time_point orderbook_time;
    };
    SignalState cached_signal_;
    std::mutex signal_mutex_;
    std::atomic<bool> signal_fired_{false};
    std::condition_variable signal_cv_;
    std::mutex signal_cv_mutex_;

    // Stats
    std::atomic<int64_t> signals_fired_{0};
    std::atomic<int64_t> orders_attempted_{0};
    std::atomic<int64_t> orders_filled_{0};

    // Thread
    std::thread main_thread_;
    std::once_flag stop_flag_;

    // Event handlers
    void handle_orderbook_update(const OrderBook& orderbook);
    void handle_connection_status(bool connected);

    // Core logic
    void main_loop();
    void execute_signal(const SignalState& state);
    void print_status();

    // Setup
    bool setup_exchange();
    bool validate_config();
    std::string format_symbol_for_exchange();
};

} // namespace MarketMaker
#endif // MOMENTUM_TAKER_H
