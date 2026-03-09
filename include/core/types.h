#ifndef TYPES_H
#define TYPES_H

#include <string>
#include <chrono>
#include <vector>
#include <memory>
#include <functional>

namespace MarketMaker {

enum class OrderSide {
    BUY,
    SELL
};

enum class OrderStatus {
    NEW,
    PARTIALLY_FILLED,
    FILLED,
    CANCELED,
    REJECTED,
    EXPIRED
};

struct PriceLevel {
    double price;
    double quantity;

    PriceLevel(double p = 0.0, double q = 0.0) : price(p), quantity(q) {}
};

struct OrderBook {
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    std::chrono::steady_clock::time_point timestamp;

    [[nodiscard]] double get_mid_price() const {
        if (bids.empty() || asks.empty()) {
            return 0.0;
        }
        return (bids[0].price + asks[0].price) / 2.0;
    }

    [[nodiscard]] double get_best_bid() const {
        return bids.empty() ? 0.0 : bids[0].price;
    }

    [[nodiscard]] double get_best_ask() const {
        return asks.empty() ? 0.0 : asks[0].price;
    }

    // Volume-weighted mid price using top N levels
    [[nodiscard]] double get_vwap_mid(size_t depth = 5) const {
        if (bids.empty() || asks.empty()) return 0.0;

        double bid_value = 0.0, bid_volume = 0.0;
        double ask_value = 0.0, ask_volume = 0.0;

        size_t bid_depth = std::min(depth, bids.size());
        size_t ask_depth = std::min(depth, asks.size());

        for (size_t i = 0; i < bid_depth; ++i) {
            bid_value += bids[i].price * bids[i].quantity;
            bid_volume += bids[i].quantity;
        }
        for (size_t i = 0; i < ask_depth; ++i) {
            ask_value += asks[i].price * asks[i].quantity;
            ask_volume += asks[i].quantity;
        }

        if (bid_volume <= 0 || ask_volume <= 0) return get_mid_price();

        double vwap_bid = bid_value / bid_volume;
        double vwap_ask = ask_value / ask_volume;
        return (vwap_bid + vwap_ask) / 2.0;
    }

    // Orderbook imbalance ratio: >1 means more bid pressure, <1 more ask pressure
    [[nodiscard]] double get_imbalance_ratio(size_t depth = 5) const {
        if (bids.empty() || asks.empty()) return 1.0;

        double bid_volume = 0.0, ask_volume = 0.0;
        size_t bid_depth = std::min(depth, bids.size());
        size_t ask_depth = std::min(depth, asks.size());

        for (size_t i = 0; i < bid_depth; ++i) bid_volume += bids[i].quantity;
        for (size_t i = 0; i < ask_depth; ++i) ask_volume += asks[i].quantity;

        if (ask_volume <= 0) return 1.0;
        return bid_volume / ask_volume;
    }
};

struct Order {
    std::string order_id;
    std::string client_order_id;
    std::string symbol;
    OrderSide side;
    double price;
    double quantity;
    double executed_quantity;
    OrderStatus status;
    std::chrono::steady_clock::time_point created_time;
    std::chrono::steady_clock::time_point updated_time;
};

// Callback types for user data stream events (fills, balance updates)
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

struct MarketData {
    std::string symbol;
    double last_price;
    double volume_24h;
    std::chrono::steady_clock::time_point timestamp;
};

struct LatencyMetrics {
    // Execution latency (time to execute place_market_maker_orders function)
    double avg_order_latency_ms = 0.0;
    double max_order_latency_ms = 0.0;
    double min_order_latency_ms = 999999.0;

    // Reaction latency (time from orderbook update received to order placed)
    double avg_reaction_latency_ms = 0.0;
    double max_reaction_latency_ms = 0.0;
    double min_reaction_latency_ms = 999999.0;

    long total_orders = 0;
    long successful_orders = 0;
    long failed_orders = 0;
    long reconnect_count = 0;
    std::chrono::steady_clock::time_point start_time;

    void update_latency(double latency_ms) {
        avg_order_latency_ms = (avg_order_latency_ms * total_orders + latency_ms) / (total_orders + 1);
        max_order_latency_ms = std::max(max_order_latency_ms, latency_ms);
        min_order_latency_ms = std::min(min_order_latency_ms, latency_ms);
        total_orders++;
    }

    void update_reaction_latency(double latency_ms) {
        if (total_orders == 0) {
            avg_reaction_latency_ms = latency_ms;
        } else {
            avg_reaction_latency_ms = (avg_reaction_latency_ms * total_orders + latency_ms) / (total_orders + 1);
        }
        max_reaction_latency_ms = std::max(max_reaction_latency_ms, latency_ms);
        min_reaction_latency_ms = std::min(min_reaction_latency_ms, latency_ms);
    }

    [[nodiscard]] double get_uptime_percentage() const {
        auto now = std::chrono::steady_clock::now();
        auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        return uptime_seconds > 0 ? 100.0 : 0.0;  // Simplified, should track actual downtime
    }

    [[nodiscard]] double get_success_rate() const {
        return total_orders > 0 ? (static_cast<double>(successful_orders) / total_orders * 100.0) : 0.0;
    }

    [[nodiscard]] double get_orders_per_minute() const {
        auto now = std::chrono::steady_clock::now();
        auto minutes = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() / 60.0;
        return minutes > 0 ? (total_orders / minutes) : 0.0;
    }
};

} // namespace MarketMaker

#endif // TYPES_H