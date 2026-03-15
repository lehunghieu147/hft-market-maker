#include "trading/order_manager.h"
#include "core/app_logger.h"
#include "core/metrics_collector.h"
#include <format>
#include <cmath>
#include <random>

namespace MarketMaker {

OrderManager::OrderManager(std::shared_ptr<IExchange> exchange, const Config& config,
                           std::shared_ptr<RiskManager> risk_manager)
    : exchange_(exchange), config_(config), risk_manager_(risk_manager)
    , price_multiplier_(std::pow(10, config.price_precision))
    , qty_multiplier_(std::pow(10, config.quantity_precision)) {
    logger_ = AppLogger::get("trading");
    metrics_.start_time = std::chrono::steady_clock::now();
}

OrderManager::~OrderManager() {
    cancel_all_active_orders();
}

std::shared_ptr<Order> OrderManager::make_pooled_order(const Order& src) {
    Order* raw = order_pool_.allocate(src);
    if (!raw) {
        // Pool exhausted — fallback to heap allocation
        LOG_WARNING(logger_, "Order pool exhausted ({}/{} used), falling back to heap",
                    order_pool_.allocated(), order_pool_.capacity());
        return std::make_shared<Order>(src);
    }
    // Custom deleter returns the slot back to the pool
    return std::shared_ptr<Order>(raw, [this](Order* p) {
        order_pool_.deallocate(p);
    });
}

bool OrderManager::place_market_maker_orders(double mid_price) {
    return place_market_maker_orders(mid_price, std::chrono::steady_clock::now());
}

bool OrderManager::place_market_maker_orders(double mid_price, const std::chrono::steady_clock::time_point& orderbook_time) {
    if (mid_price <= 0) {
        LOG_ERROR(logger_, "Invalid mid price: {}", mid_price);
        return false;
    }

    // Calculate spread-based bid/ask prices
    double spread_multiplier = config_.spread_percentage;
    double bid_price = format_price(mid_price * (1.0 - spread_multiplier));
    double ask_price = format_price(mid_price * (1.0 + spread_multiplier));

    return place_market_maker_orders_with_prices(mid_price, bid_price, ask_price, orderbook_time);
}

bool OrderManager::place_market_maker_orders_with_prices(double mid_price, double bid_price, double ask_price,
                                                          const std::chrono::steady_clock::time_point& orderbook_time) {
    if (mid_price <= 0) {
        LOG_ERROR(logger_, "Invalid mid price: {}", mid_price);
        return false;
    }

    auto start_time = std::chrono::steady_clock::now();

    bid_price = format_price(bid_price);
    ask_price = format_price(ask_price);

    // Compute effective order size (dynamic if risk manager supports it)
    double order_size = config_.order_size;
    if (risk_manager_ && config_.use_dynamic_sizing) {
        order_size = format_quantity(risk_manager_->adjusted_order_size(
            config_.order_size, config_.vol_sizing_exponent,
            config_.min_size_multiplier, config_.max_size_multiplier));
        LOG_DEBUG(logger_, "Dynamic sizing: base={} adjusted={}", config_.order_size, order_size);
    }

    // Check if price change is significant enough to update
    const double PRICE_CHANGE_THRESHOLD = 0.0001; // 0.01% minimum change
    bool need_update = false;

    {
        std::lock_guard<std::mutex> lock(orders_mutex_);

        if (active_bid_orders_.empty() || active_ask_orders_.empty()) {
            need_update = true;
            LOG_DEBUG(logger_, "{}", "No active orders, placing new ones");
        } else {
            double price_change_ratio = std::abs(mid_price - last_mid_price_) / last_mid_price_;
            if (price_change_ratio > PRICE_CHANGE_THRESHOLD) {
                need_update = true;
            } else {
                return true; // Skip update
            }
        }
    }

    if (!need_update) return true;

    // Risk management gate
    if (risk_manager_ && !risk_manager_->should_trade()) {
        LOG_ERROR(logger_, "{}", "Trading blocked by risk manager");
        return false;
    }

    // Order validation
    auto validation = order_validator_.validate_market_maker_orders(
        bid_price, ask_price, order_size, mid_price);
    if (!validation.is_valid) {
        LOG_ERROR(logger_, "Validation failed: {}", validation.error_message);
        return false;
    }

    // Position limit check
    if (risk_manager_) {
        if (!risk_manager_->position_tracker().can_place_pair(order_size, order_size)) {
            LOG_ERROR(logger_, "{}", "Position limit would be exceeded");
            return false;
        }
    }

    LOG_INFO(logger_, "[ORDER]      BID ${:.2f} x {} | ASK ${:.2f} x {} (levels={})",
             bid_price, order_size, ask_price, order_size, config_.num_quote_levels);

    // Snapshot and cancel all existing orders via thread pool
    std::vector<std::shared_ptr<Order>> orders_to_cancel;
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        orders_to_cancel.insert(orders_to_cancel.end(), active_bid_orders_.begin(), active_bid_orders_.end());
        orders_to_cancel.insert(orders_to_cancel.end(), active_ask_orders_.begin(), active_ask_orders_.end());
    }

    if (!orders_to_cancel.empty()) {
        std::vector<std::future<bool>> cancel_futures;
        for (auto& order : orders_to_cancel) {
            cancel_futures.push_back(thread_pool_.submit([this, order]() {
                return cancel_order(order);
            }));
        }
        constexpr auto timeout = std::chrono::milliseconds(100);
        for (auto& f : cancel_futures) {
            if (f.wait_for(timeout) == std::future_status::ready) {
                f.get();
            } else {
                LOG_WARNING(logger_, "{}", "Cancel order timeout after 100ms");
            }
        }
        std::lock_guard<std::mutex> lock(orders_mutex_);
        active_bid_orders_.clear();
        active_ask_orders_.clear();
    }

    // Place multi-level orders via thread pool
    int levels = std::max(1, config_.num_quote_levels);
    double bid_distance = mid_price - bid_price;  // positive distance from mid
    double ask_distance = ask_price - mid_price;

    std::vector<std::future<bool>> place_futures;
    for (int i = 0; i < levels; ++i) {
        double level_mult = (i == 0) ? 1.0 : std::pow(config_.level_spacing_multiplier, i);
        double level_size = format_quantity(order_size * std::pow(config_.level_size_decay, i));
        if (level_size < 1e-8) break;

        double level_bid = format_price(mid_price - bid_distance * level_mult);
        double level_ask = format_price(mid_price + ask_distance * level_mult);

        place_futures.push_back(thread_pool_.submit([this, level_bid, level_size]() {
            return place_order(OrderSide::BUY, level_bid, level_size);
        }));
        place_futures.push_back(thread_pool_.submit([this, level_ask, level_size]() {
            return place_order(OrderSide::SELL, level_ask, level_size);
        }));
    }

    int successes = 0;
    int total = static_cast<int>(place_futures.size());
    for (auto& f : place_futures) {
        if (f.get()) successes++;
    }

    last_mid_price_ = mid_price;
    last_order_update_ = std::chrono::steady_clock::now();

    bool bid_success = successes > 0;
    bool ask_success = successes > 0;

    if (successes == total) {
        LOG_INFO(logger_, "[ORDER]      All {} orders placed OK", total);
    } else if (successes > 0) {
        LOG_WARNING(logger_, "Partial: {}/{} orders placed", successes, total);
    } else {
        LOG_ERROR(logger_, "{}", "FAILED: No orders were placed");
    }

    update_metrics(start_time, orderbook_time, bid_success, ask_success);

    return successes == total;
}

bool OrderManager::place_taker_order(OrderSide side, double price, double quantity,
                                     const std::string& order_type,
                                     const std::chrono::steady_clock::time_point& /*orderbook_time*/) {
    // Risk management gate
    if (risk_manager_ && !risk_manager_->should_trade()) {
        LOG_ERROR(logger_, "{}", "Taker order blocked by risk manager");
        return false;
    }

    std::string client_order_id = generate_client_order_id(side);
    const char* side_str = (side == OrderSide::BUY) ? "BUY" : "SELL";

    std::optional<Order> result;
    if (order_type == "market") {
        result = exchange_->place_market_order(config_.symbol, side, quantity, client_order_id);
    } else {
        // Default: IOC limit order
        result = exchange_->place_ioc_order(config_.symbol, side, price, quantity, client_order_id);
    }

    if (!result) {
        LOG_ERROR(logger_, "Failed to place taker {} order at {}", side_str, price);
        if (risk_manager_) risk_manager_->on_error();
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.failed_orders++;
        metrics_.total_orders++;
        return false;
    }

    if (risk_manager_) risk_manager_->on_success();

    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.successful_orders++;
        metrics_.total_orders++;
    }

    LOG_INFO(logger_, "TAKER_{} {} price={:.2f} qty={:.6f} type={} id={}",
             side_str, result->symbol, price, quantity, order_type, result->order_id);
    return true;
}

bool OrderManager::cancel_all_active_orders() {
    // Snapshot orders outside lock to avoid holding mutex during network I/O
    std::vector<std::shared_ptr<Order>> all_orders;
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        all_orders.insert(all_orders.end(), active_bid_orders_.begin(), active_bid_orders_.end());
        all_orders.insert(all_orders.end(), active_ask_orders_.begin(), active_ask_orders_.end());
    }

    // Cancel all orders in parallel via thread pool
    std::vector<std::future<bool>> cancel_futures;
    for (auto& order : all_orders) {
        cancel_futures.push_back(thread_pool_.submit([this, order]() {
            return cancel_order(order);
        }));
    }

    bool success = true;
    for (auto& f : cancel_futures) {
        success &= f.get();
    }

    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        active_bid_orders_.clear();
        active_ask_orders_.clear();
    }

    return success;
}

bool OrderManager::update_orders_if_needed(double new_mid_price) {
    return update_orders_if_needed(new_mid_price, std::chrono::steady_clock::now());
}

bool OrderManager::update_orders_if_needed(double new_mid_price, const std::chrono::steady_clock::time_point& orderbook_time) {
    // Timestamp validation: reject stale orderbook data (>5 seconds old)
    auto age = std::chrono::steady_clock::now() - orderbook_time;
    if (age > std::chrono::seconds(5)) {
        auto ms_age = std::chrono::duration_cast<std::chrono::milliseconds>(age).count();
        LOG_ERROR(logger_, "Rejecting stale orderbook data ({}ms old)", ms_age);
        return false;
    }

    if (!should_update_orders(new_mid_price)) {
        return true;  // No update needed
    }

    LOG_DEBUG(logger_, "Mid price changed from {} to {} - updating orders",
              last_mid_price_.load(), new_mid_price);

    return place_market_maker_orders(new_mid_price, orderbook_time);
}

std::pair<std::shared_ptr<Order>, std::shared_ptr<Order>> OrderManager::get_active_orders() const {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    auto bid = active_bid_orders_.empty() ? nullptr : active_bid_orders_.front();
    auto ask = active_ask_orders_.empty() ? nullptr : active_ask_orders_.front();
    return {bid, ask};
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
    return std::round(price * price_multiplier_) / price_multiplier_;
}

double OrderManager::format_quantity(double quantity) const {
    return std::round(quantity * qty_multiplier_) / qty_multiplier_;
}

bool OrderManager::place_order(OrderSide side, double price, double quantity) {
    std::string client_order_id = generate_client_order_id(side);
    const char* side_str = (side == OrderSide::BUY) ? "BID" : "ASK";

    auto order_result = exchange_->place_limit_order(
        config_.symbol, side, price, quantity, client_order_id);

    if (!order_result) {
        LOG_ERROR(logger_, "Failed to place {} order at {}", side_str, price);
        if (risk_manager_) risk_manager_->on_error();
        MetricsCollector::instance().increment("orders_failed_total");
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        metrics_.failed_orders++;
        return false;
    }

    if (risk_manager_) {
        risk_manager_->on_success();
    }

    MetricsCollector::instance().increment("orders_placed_total");

    std::lock_guard<std::mutex> lock(orders_mutex_);
    if (side == OrderSide::BUY) {
        active_bid_orders_.push_back(make_pooled_order(*order_result));
    } else {
        active_ask_orders_.push_back(make_pooled_order(*order_result));
    }

    LOG_DEBUG(logger_, "Placed {} order: ID={} Price={:.2f} Qty={:.4f}",
              side_str, order_result->order_id, price, quantity);

    return true;
}

bool OrderManager::cancel_order(const std::shared_ptr<Order>& order) {
    if (!order) {
        return true;  // Nothing to cancel
    }

    auto result = exchange_->cancel_order(config_.symbol, order->order_id);

    if (!result || !*result) {
        // Cancel failed — check if order was already filled
        auto status = exchange_->get_order_status(config_.symbol, order->order_id);
        if (status && (status->status == OrderStatus::FILLED ||
                       status->status == OrderStatus::CANCELED)) {
            LOG_INFO(logger_, "Cancel failed but order already {}: {}",
                     (status->status == OrderStatus::FILLED ? "FILLED" : "CANCELED"),
                     order->order_id);
            return true;  // Not an error
        }
        LOG_ERROR(logger_, "Failed to cancel order: {}", order->order_id);
        return false;
    }

    MetricsCollector::instance().increment("orders_cancelled_total");
    LOG_DEBUG(logger_, "Canceled order: {}", order->order_id);
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

    MetricsCollector::instance().observe("order_latency_ms", execution_latency_ms);

    // Record into percentile trackers
    exec_latency_tracker_.record(execution_latency_us);
    reaction_latency_tracker_.record(reaction_latency_us);

    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_.update_latency(execution_latency_ms);
    metrics_.update_reaction_latency(reaction_latency_ms);
    metrics_.successful_orders += (bid_success ? 1 : 0) + (ask_success ? 1 : 0);

    LOG_DEBUG(logger_, "LATENCY reaction={:.3f}ms exec={:.3f}ms",
              reaction_latency_ms, execution_latency_ms);
}

OrderManager::PercentileMetrics OrderManager::get_exec_percentiles() const {
    return {
        exec_latency_tracker_.percentile(0.50) / 1000.0,
        exec_latency_tracker_.percentile(0.95) / 1000.0,
        exec_latency_tracker_.percentile(0.99) / 1000.0,
        exec_latency_tracker_.max() / 1000.0,
        exec_latency_tracker_.count()
    };
}

OrderManager::PercentileMetrics OrderManager::get_reaction_percentiles() const {
    return {
        reaction_latency_tracker_.percentile(0.50) / 1000.0,
        reaction_latency_tracker_.percentile(0.95) / 1000.0,
        reaction_latency_tracker_.percentile(0.99) / 1000.0,
        reaction_latency_tracker_.max() / 1000.0,
        reaction_latency_tracker_.count()
    };
}

std::string OrderManager::generate_client_order_id(OrderSide side) {
    thread_local std::random_device rd;
    thread_local std::mt19937 gen(rd());
    thread_local std::uniform_int_distribution<> dis(100000, 999999);

    return std::format("MM_{}_{}_{}",
        side == OrderSide::BUY ? "BID" : "ASK",
        std::chrono::system_clock::now().time_since_epoch().count(),
        dis(gen));
}

void OrderManager::on_fill_event(const std::string& order_id,
                                 const std::string& /*client_order_id*/,
                                 OrderSide side,
                                 OrderStatus status,
                                 double price,
                                 double quantity,
                                 double /*cumulative_quantity*/) {
    // Update order state in active order vectors
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        auto update_vec = [&](std::vector<std::shared_ptr<Order>>& vec) -> bool {
            for (auto it = vec.begin(); it != vec.end(); ++it) {
                if ((*it)->order_id == order_id) {
                    (*it)->status = status;
                    (*it)->executed_quantity += quantity;
                    if (status == OrderStatus::FILLED || status == OrderStatus::CANCELED) {
                        vec.erase(it);
                    }
                    return true;
                }
            }
            return false;
        };
        if (!update_vec(active_bid_orders_)) {
            update_vec(active_ask_orders_);
        }
    }

    // Track position and P&L from real fills
    if (risk_manager_ && quantity > 0 &&
        (status == OrderStatus::FILLED || status == OrderStatus::PARTIALLY_FILLED)) {
        // Snapshot position state before updating
        double position_before = risk_manager_->position_tracker().get_position();
        double avg_entry = risk_manager_->position_tracker().get_average_entry_price();

        // Update position
        risk_manager_->position_tracker().on_fill(side, price, quantity);

        // Compute realized P&L if position reduced (market maker = maker, else taker)
        // Default: assume maker (limit orders). Taker IOC/market fills use same rate
        // for now. Exact maker/taker could be parsed from Binance executionReport "m" field.
        bool is_maker = true;
        risk_manager_->pnl_tracker().on_fill(side, price, quantity,
                                              position_before, avg_entry, is_maker);

        MetricsCollector::instance().increment("orders_filled_total");

        LOG_INFO(logger_, "[FILL]       {} {:.6f} @ ${:.2f} | pos: {:.6f} -> {:+.6f}",
                 (side == OrderSide::BUY ? "BUY" : "SELL"), quantity, price,
                 position_before, risk_manager_->position_tracker().get_position());
    }
}

} // namespace MarketMaker
