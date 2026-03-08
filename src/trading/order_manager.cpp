#include "trading/order_manager.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <random>

namespace MarketMaker {

OrderManager::OrderManager(std::shared_ptr<IExchange> exchange, const Config& config,
                           std::shared_ptr<RiskManager> risk_manager)
    : exchange_(exchange), config_(config), risk_manager_(risk_manager) {
    metrics_.start_time = std::chrono::steady_clock::now();
}

OrderManager::~OrderManager() {
    cancel_all_active_orders();
}

bool OrderManager::place_market_maker_orders(double mid_price) {
    return place_market_maker_orders(mid_price, std::chrono::steady_clock::now());
}

bool OrderManager::place_market_maker_orders(double mid_price, const std::chrono::steady_clock::time_point& orderbook_time) {
    if (mid_price <= 0) {
        std::cerr << "Invalid mid price: " << mid_price << std::endl;
        return false;
    }

    auto start_time = std::chrono::steady_clock::now();
    auto t1 = start_time;

    // Pre-calculate prices before any network I/O
    double spread_multiplier = config_.spread_percentage;
    double bid_multiplier = 1.0 - spread_multiplier;
    double ask_multiplier = 1.0 + spread_multiplier;

    double bid_price_raw = mid_price * bid_multiplier;
    double ask_price_raw = mid_price * ask_multiplier;

    double bid_price = format_price(bid_price_raw);
    double ask_price = format_price(ask_price_raw);

    auto t2 = std::chrono::steady_clock::now();
    auto calc_time = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();

    // Price calculation logging
    std::cout << "\n=====================================================" << std::endl;
    std::cout << "  PRICE CALCULATION" << std::endl;
    std::cout << "=====================================================" << std::endl;
    std::cout << "  Mid Price:       $" << std::fixed << std::setprecision(5)
              << mid_price << " (from OrderBook)" << std::endl;
    std::cout << "  Spread Config:    " << std::fixed << std::setprecision(1)
              << (spread_multiplier * 100) << "%" << std::endl;
    std::cout << "-----------------------------------------------------" << std::endl;
    std::cout << "  BUY Order (BID):" << std::endl;
    std::cout << "    Formula: MidPrice × (1 - Spread)" << std::endl;
    std::cout << "    Calc: " << std::fixed << std::setprecision(5) << mid_price << " × " << std::setprecision(4)
              << bid_multiplier << " = " << std::setprecision(7) << bid_price_raw
              << " -> $" << std::setprecision(5) << bid_price << std::endl;
    std::cout << "-----------------------------------------------------" << std::endl;
    std::cout << "  SELL Order (ASK):" << std::endl;
    std::cout << "    Formula: MidPrice × (1 + Spread)" << std::endl;
    std::cout << "    Calc: " << std::fixed << std::setprecision(5) << mid_price << " × " << std::setprecision(4)
              << ask_multiplier << " = " << std::setprecision(7) << ask_price_raw
              << " -> $" << std::setprecision(5) << ask_price << std::endl;
    std::cout << "-----------------------------------------------------" << std::endl;
    std::cout << "  Calc Time: " << calc_time << " us" << std::endl;
    std::cout << "=====================================================" << std::endl;

    // OPTIMIZATION: Check if price change is significant enough
    const double PRICE_CHANGE_THRESHOLD = 0.0001; // 0.01% minimum change
    bool need_update = false;

    {
        std::lock_guard<std::mutex> lock(orders_mutex_);

        // Check if we need to update orders
        if (!active_bid_order_ || !active_ask_order_) {
            // No active orders, must place new ones
            need_update = true;
            std::cout << "[UPDATE] No active orders, placing new ones" << std::endl;
        } else {
            // Check if price changed significantly
            double price_change_ratio = std::abs(mid_price - last_mid_price_) / last_mid_price_;
            if (price_change_ratio > PRICE_CHANGE_THRESHOLD) {
                need_update = true;
                std::cout << "[UPDATE] Price change " << std::fixed << std::setprecision(5)
                          << (price_change_ratio * 100)
                          << "% exceeds threshold, updating orders" << std::endl;
            } else {
                std::cout << "[SKIP] Price change " << std::fixed << std::setprecision(5)
                          << (price_change_ratio * 100)
                          << "% below threshold, skipping update" << std::endl;
                return true; // Skip update
            }
        }
    }

    if (!need_update) {
        return true;
    }

    // Risk management gate
    if (risk_manager_ && !risk_manager_->should_trade()) {
        std::cerr << "[ORDER] Trading blocked by risk manager" << std::endl;
        return false;
    }

    // Order validation
    auto validation = order_validator_.validate_market_maker_orders(
        bid_price, ask_price, config_.order_size, mid_price);
    if (!validation.is_valid) {
        std::cerr << "[ORDER] Validation failed: " << validation.error_message << std::endl;
        return false;
    }

    // Position limit check (atomic pair check under single lock)
    if (risk_manager_) {
        if (!risk_manager_->position_tracker().can_place_pair(config_.order_size, config_.order_size)) {
            std::cerr << "[ORDER] Position limit would be exceeded" << std::endl;
            return false;
        }
    }

    std::cout << "\n=========== PLACING NEW ORDERS ===========" << std::endl;
    std::cout << "  Mid Price: $" << std::fixed << std::setprecision(5) << mid_price << std::endl;
    std::cout << "  BID (Buy):  $" << bid_price << " [Qty: " << config_.order_size << "]" << std::endl;
    std::cout << "  ASK (Sell): $" << ask_price << " [Qty: " << config_.order_size << "]" << std::endl;
    std::cout << "==========================================" << std::endl;

    // OPTIMIZATION: Try to modify existing orders first if they exist
    bool bid_success = false;
    bool ask_success = false;

    auto t3 = std::chrono::steady_clock::now();

    // Copy orders outside of lock to minimize critical section
    std::shared_ptr<Order> bid_order_to_cancel;
    std::shared_ptr<Order> ask_order_to_cancel;
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        bid_order_to_cancel = active_bid_order_;
        ask_order_to_cancel = active_ask_order_;
    }

    // Cancel orders in parallel - handle each order independently
    std::vector<std::future<bool>> cancel_futures;

    if (bid_order_to_cancel) {
        cancel_futures.push_back(std::async(std::launch::async, [this, bid_order_to_cancel]() {
            return cancel_order(bid_order_to_cancel);
        }));
    }

    if (ask_order_to_cancel) {
        cancel_futures.push_back(std::async(std::launch::async, [this, ask_order_to_cancel]() {
            return cancel_order(ask_order_to_cancel);
        }));
    }

    // Wait for all cancellations with timeout (100ms max per cancel)
    constexpr auto timeout = std::chrono::milliseconds(100);

    for (size_t i = 0; i < cancel_futures.size(); ++i) {
        if (cancel_futures[i].wait_for(timeout) == std::future_status::ready) {
            cancel_futures[i].get();
        } else {
            std::cerr << "[WARNING] Cancel order timeout after 100ms" << std::endl;
        }
    }

    // Clear active orders after cancellation attempts
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        if (bid_order_to_cancel) {
            active_bid_order_.reset();
        }
        if (ask_order_to_cancel) {
            active_ask_order_.reset();
        }
    }

    auto t4 = std::chrono::steady_clock::now();
    auto cancel_time = std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3).count();
    std::cout << "[LATENCY] Cancel orders: " << cancel_time << " μs" << std::endl;

    // OPTIMIZATION: Use threads instead of async to avoid overhead
    auto t5 = std::chrono::steady_clock::now();
    std::thread bid_thread([this, bid_price, &bid_success]() {
        auto thread_start = std::chrono::steady_clock::now();
        bid_success = place_order(OrderSide::BUY, bid_price, config_.order_size);
        auto thread_end = std::chrono::steady_clock::now();
        auto thread_time = std::chrono::duration_cast<std::chrono::microseconds>(thread_end - thread_start).count();
        std::cout << "[LATENCY] BID order placement: " << thread_time << " μs" << std::endl;
    });

    std::thread ask_thread([this, ask_price, &ask_success]() {
        auto thread_start = std::chrono::steady_clock::now();
        ask_success = place_order(OrderSide::SELL, ask_price, config_.order_size);
        auto thread_end = std::chrono::steady_clock::now();
        auto thread_time = std::chrono::duration_cast<std::chrono::microseconds>(thread_end - thread_start).count();
        std::cout << "[LATENCY] ASK order placement: " << thread_time << " μs" << std::endl;
    });

    // Wait for both threads
    bid_thread.join();
    ask_thread.join();
    auto t6 = std::chrono::steady_clock::now();
    auto thread_time = std::chrono::duration_cast<std::chrono::microseconds>(t6 - t5).count();
    std::cout << "[LATENCY] Total thread execution: " << thread_time << " μs" << std::endl;

    last_mid_price_ = mid_price;
    last_order_update_ = std::chrono::steady_clock::now();

    // Display order placement summary
    if (bid_success && ask_success) {
        std::cout << "\n=============================================" << std::endl;
        std::cout << "  BOTH ORDERS PLACED SUCCESSFULLY" << std::endl;
        std::cout << "=============================================" << std::endl;
    } else if (bid_success || ask_success) {
        std::cout << "\n=============================================" << std::endl;
        std::cout << "  PARTIAL SUCCESS: Only " << (bid_success ? "BID" : "ASK") << " order placed" << std::endl;
        std::cout << "=============================================" << std::endl;
    } else {
        std::cout << "\n=============================================" << std::endl;
        std::cout << "  FAILED: No orders were placed" << std::endl;
        std::cout << "=============================================" << std::endl;
    }

    update_metrics(start_time, orderbook_time, bid_success, ask_success);

    return bid_success && ask_success;
}

bool OrderManager::cancel_all_active_orders() {
    // Copy orders outside lock to avoid holding mutex during network I/O
    std::shared_ptr<Order> bid_copy;
    std::shared_ptr<Order> ask_copy;
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        bid_copy = active_bid_order_;
        ask_copy = active_ask_order_;
    }

    // Cancel both orders in parallel (no lock held)
    std::vector<std::future<bool>> cancel_futures;

    if (bid_copy) {
        cancel_futures.push_back(std::async(std::launch::async,
            [this, order = bid_copy]() {
                return cancel_order(order);
            }));
    }

    if (ask_copy) {
        cancel_futures.push_back(std::async(std::launch::async,
            [this, order = ask_copy]() {
                return cancel_order(order);
            }));
    }

    bool success = true;
    for (auto& future : cancel_futures) {
        success &= future.get();
    }

    // Clear active orders after cancellation
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        active_bid_order_.reset();
        active_ask_order_.reset();
    }

    return success;
}

bool OrderManager::update_orders_if_needed(double new_mid_price) {
    return update_orders_if_needed(new_mid_price, std::chrono::steady_clock::now());
}

bool OrderManager::update_orders_if_needed(double new_mid_price, const std::chrono::steady_clock::time_point& orderbook_time) {
    if (!should_update_orders(new_mid_price)) {
        return true;  // No update needed
    }

    std::cout << "Mid price changed from " << last_mid_price_.load()
              << " to " << new_mid_price << " - updating orders" << std::endl;

    return place_market_maker_orders(new_mid_price, orderbook_time);
}

std::pair<std::shared_ptr<Order>, std::shared_ptr<Order>> OrderManager::get_active_orders() const {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    return {active_bid_order_, active_ask_order_};
}

LatencyMetrics OrderManager::get_metrics() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return metrics_;
}

void OrderManager::reset_metrics() {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_ = LatencyMetrics();
    metrics_.start_time = std::chrono::steady_clock::now();
}

double OrderManager::format_price(double price) const {
    double multiplier = std::pow(10, config_.price_precision);
    return std::round(price * multiplier) / multiplier;
}

double OrderManager::format_quantity(double quantity) const {
    double multiplier = std::pow(10, config_.quantity_precision);
    return std::round(quantity * multiplier) / multiplier;
}

bool OrderManager::place_order(OrderSide side, double price, double quantity) {
    std::string client_order_id = generate_client_order_id(side);

    auto order_result = exchange_->place_limit_order(
        config_.symbol,
        side,
        price,
        quantity,
        client_order_id
    );

    if (!order_result) {
        std::cerr << "Failed to place " << (side == OrderSide::BUY ? "BID" : "ASK")
                  << " order at " << price << std::endl;

        if (risk_manager_) risk_manager_->on_error();

        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.failed_orders++;
        return false;
    }

    if (risk_manager_) {
        risk_manager_->on_success();
        // Track position (approximate until User Data Stream in Phase 04)
        risk_manager_->position_tracker().on_fill(side, price, quantity);
    }

    std::lock_guard<std::mutex> lock(orders_mutex_);
    if (side == OrderSide::BUY) {
        active_bid_order_ = std::make_shared<Order>(*order_result);
    } else {
        active_ask_order_ = std::make_shared<Order>(*order_result);
    }

    std::cout << "Placed " << (side == OrderSide::BUY ? "BID" : "ASK")
              << " order: ID=" << order_result->order_id
              << ", Price=" << std::fixed << std::setprecision(2) << price
              << ", Qty=" << std::fixed << std::setprecision(2) << quantity << std::endl;

    return true;
}

bool OrderManager::cancel_order(const std::shared_ptr<Order>& order) {
    if (!order) {
        return true;  // Nothing to cancel
    }

    auto result = exchange_->cancel_order(config_.symbol, order->order_id);

    if (!result || !*result) {
        std::cerr << "Failed to cancel order: " << order->order_id << std::endl;
        return false;
    }

    std::cout << "Canceled order: " << order->order_id << std::endl;
    return true;
}

bool OrderManager::should_update_orders(double new_mid_price) const {
    // Check if price has changed
    double current_mid = last_mid_price_.load();
    if (std::abs(new_mid_price - current_mid) < 0.00001) {
        return false;  // Price hasn't changed significantly
    }

    // Check cooldown period
    auto now = std::chrono::steady_clock::now();
    auto time_since_last_update = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_order_update_
    );

    if (time_since_last_update < config_.order_update_cooldown) {
        return false;  // Still in cooldown period
    }

    return true;
}

void OrderManager::update_metrics(const std::chrono::steady_clock::time_point& start_time,
                                  const std::chrono::steady_clock::time_point& orderbook_time,
                                  bool bid_success, bool ask_success) {
    auto end_time = std::chrono::steady_clock::now();

    auto execution_latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
        end_time - start_time
    ).count();
    double execution_latency_ms = execution_latency_us / 1000.0;

    auto reaction_latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
        end_time - orderbook_time
    ).count();
    double reaction_latency_ms = reaction_latency_us / 1000.0;

    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_.update_latency(execution_latency_ms);
    metrics_.update_reaction_latency(reaction_latency_ms);
    metrics_.successful_orders += (bid_success ? 1 : 0) + (ask_success ? 1 : 0);

    // Display reaction latency only
    std::cout << "\n================================================" << std::endl;
    std::cout << "  LATENCY METRICS" << std::endl;
    std::cout << "================================================" << std::endl;
    std::cout << "  Reaction Latency: " << std::fixed << std::setprecision(3)
              << reaction_latency_ms << " ms (" << reaction_latency_us << " us)" << std::endl;

    if (reaction_latency_ms < 50) {
        std::cout << "  Status: TARGET MET (< 50ms requirement)" << std::endl;
    } else {
        std::cout << "  Status: Above target (optimizing...)" << std::endl;
    }
    std::cout << "================================================\n" << std::endl;
}

std::string OrderManager::generate_client_order_id(OrderSide side) {
    thread_local std::random_device rd;
    thread_local std::mt19937 gen(rd());
    thread_local std::uniform_int_distribution<> dis(100000, 999999);

    std::stringstream ss;
    ss << "MM_" << (side == OrderSide::BUY ? "BID_" : "ASK_")
       << std::chrono::system_clock::now().time_since_epoch().count()
       << "_" << dis(gen);

    return ss.str();
}

} // namespace MarketMaker