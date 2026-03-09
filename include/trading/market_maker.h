#ifndef MARKET_MAKER_H
#define MARKET_MAKER_H

#include "core/config.h"
#include "core/types.h"
#include "core/spsc-ring-buffer.h"
#include "exchange/exchange_interface.h"
#include "trading/order_manager.h"
#include "trading/risk_manager.h"
#include "trading/volatility_tracker.h"
#include "trading/avellaneda-stoikov-model.h"
#include "trading/orderbook-imbalance-tracker.h"
#include "cloud/gcp_publisher.h"
#include "core/logger.h"
#include "quill/Logger.h"
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace MarketMaker {

// Refactored Market Maker Bot that works with any exchange
class MarketMakerBot {
public:
    explicit MarketMakerBot(const Config& config);
    ~MarketMakerBot();

    // Main control methods
    bool initialize();
    void run();
    void stop();

    // Status
    bool is_running() const { return running_; }
    LatencyMetrics get_metrics() const;

    // Getters for gRPC service and metrics
    double get_mid_price() const { return current_mid_price_.load(); }
    double get_position() const;
    double get_daily_pnl() const;
    double get_total_pnl() const;
    double get_fees_paid() const;
    bool is_kill_switch_active() const;
    const std::string& get_symbol() const { return config_.symbol; }
    double get_spread_percentage() const {
        std::lock_guard<std::mutex> lk(config_mutex_);
        return config_.spread_percentage;
    }
    std::pair<std::shared_ptr<Order>, std::shared_ptr<Order>> get_active_orders() const;

    // Setters for gRPC remote config (thread-safe: called from gRPC thread)
    void set_spread_percentage(double val) {
        std::lock_guard<std::mutex> lk(config_mutex_);
        config_.spread_percentage = val;
    }
    void set_order_size(double val) {
        std::lock_guard<std::mutex> lk(config_mutex_);
        config_.order_size = val;
    }
    void activate_kill_switch(const std::string& reason);

    // GCP event publishing (optional, non-owning)
    void set_publisher(GcpPublisher* pub) { publisher_ = pub; }

private:
    // Publish JSON event to GCP Pub/Sub (no-op if publisher not set)
    void publish_event(const std::string& event_type, const std::string& payload);

    Config config_;

    // Core components - now using exchange interface
    std::shared_ptr<IExchange> exchange_;  // Generic exchange interface
    std::shared_ptr<OrderManager> order_manager_;
    std::shared_ptr<RiskManager> risk_manager_;
    std::shared_ptr<VolatilityTracker> volatility_tracker_;
    std::unique_ptr<AvellanedaStoikovModel> as_model_;  // Inventory-aware quoting
    std::chrono::steady_clock::time_point as_horizon_start_;  // Rolling window start
    std::unique_ptr<OrderBookImbalanceTracker> obi_tracker_;  // OBI spread tilting
    std::shared_ptr<Logger> logger_;
    quill::Logger* quill_logger_ = nullptr;
    GcpPublisher* publisher_ = nullptr;  // non-owning, optional

    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};

    // Protects config_ fields written by gRPC thread and read by trading thread
    mutable std::mutex config_mutex_;

    // Market data - lock-free SPSC ring buffer replaces mutex on hot path
    // Timestamped orderbook snapshot pushed from WS thread, drained by strategy thread
    struct TimestampedOrderBook {
        OrderBook book;
        std::chrono::steady_clock::time_point received_time;
    };
    SPSCRingBuffer<TimestampedOrderBook, 64> orderbook_ring_;

    std::atomic<double> current_mid_price_{0.0};
    std::atomic<bool> price_changed_{false};
    // Condition variable kept as idle-CPU fallback (hybrid: spin briefly, then block)
    std::condition_variable price_change_cv_;
    std::mutex price_change_mutex_;

    // Threads
    std::thread main_thread_;

    // Event handlers
    void handle_orderbook_update(const OrderBook& orderbook);
    void handle_connection_status(bool connected);

    // Core logic
    void main_loop();
    void check_and_update_orders();

    // Utilities
    bool validate_config();
    bool setup_exchange();
    void print_status();
    std::string format_symbol_for_exchange();
};

} // namespace MarketMaker

#endif // MARKET_MAKER_H