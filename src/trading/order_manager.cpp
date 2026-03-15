#include "trading/order_manager.h"
#include "core/app_logger.h"
#include "core/metrics_collector.h"
#include <format>
#include <cmath>
#include <random>

namespace MarketMaker {

OrderManager::OrderManager(std::shared_ptr<IExchange> exchange, const Config& config,
                           std::shared_ptr<RiskManager> risk_manager)
    : exchange_(exchange), config_(config), risk_manager_(risk_manager) {
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

        if (!active_bid_order_ || !active_ask_order_) {
            need_update = true;
            LOG_DEBUG(logger_, "{}", "No active orders, placing new ones");
        } else {
            double price_change_ratio = std::abs(mid_price - last_mid_price_) / last_mid_price_;
            if (price_change_ratio > PRICE_CHANGE_THRESHOLD) {
                need_update = true;
                LOG_DEBUG(logger_, "Price change {:.5f}% exceeds threshold, updating orders",
                          price_change_ratio * 100);
            } else {
                LOG_DEBUG(logger_, "Price change {:.5f}% below threshold, skipping update",
                          price_change_ratio * 100);
                return true; // Skip update
            }
        }
    }

    if (!need_update) {
        return true;
    }

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

    // Position limit check (atomic pair check under single lock)
    if (risk_manager_) {
        if (!risk_manager_->position_tracker().can_place_pair(order_size, order_size)) {
            LOG_ERROR(logger_, "{}", "Position limit would be exceeded");
            return false;
        }
    }

    LOG_INFO(logger_, "[ORDER]      BID ${:.2f} x {} | ASK ${:.2f} x {}",
             bid_price, order_size, ask_price, order_size);

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

    // Cancel orders in parallel
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

    constexpr auto timeout = std::chrono::milliseconds(100);
    for (size_t i = 0; i < cancel_futures.size(); ++i) {
        if (cancel_futures[i].wait_for(timeout) == std::future_status::ready) {
            cancel_futures[i].get();
        } else {
            LOG_WARNING(logger_, "{}", "Cancel order timeout after 100ms");
        }
    }

    // Clear active orders after cancellation
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        if (bid_order_to_cancel) active_bid_order_.reset();
        if (ask_order_to_cancel) active_ask_order_.reset();
    }

    auto t4 = std::chrono::steady_clock::now();
    LOG_DEBUG(logger_, "Cancel orders latency: {}us",
              std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3).count());

    // Place new orders in parallel threads
    auto t5 = std::chrono::steady_clock::now();
    std::thread bid_thread([this, bid_price, order_size, &bid_success]() {
        bid_success = place_order(OrderSide::BUY, bid_price, order_size);
    });

    std::thread ask_thread([this, ask_price, order_size, &ask_success]() {
        ask_success = place_order(OrderSide::SELL, ask_price, order_size);
    });

    bid_thread.join();
    ask_thread.join();
    auto t6 = std::chrono::steady_clock::now();
    LOG_DEBUG(logger_, "Total thread execution: {}us",
              std::chrono::duration_cast<std::chrono::microseconds>(t6 - t5).count());

    last_mid_price_ = mid_price;
    last_order_update_ = std::chrono::steady_clock::now();

    if (bid_success && ask_success) {
        LOG_INFO(logger_, "{}", "[ORDER]      Both orders placed OK");
    } else if (bid_success || ask_success) {
        LOG_WARNING(logger_, "Partial: Only {} order placed", bid_success ? "BID" : "ASK");
    } else {
        LOG_ERROR(logger_, "{}", "FAILED: No orders were placed");
    }

    update_metrics(start_time, orderbook_time, bid_success, ask_success);

    return bid_success && ask_success;
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
        active_bid_order_ = make_pooled_order(*order_result);
    } else {
        active_ask_order_ = make_pooled_order(*order_result);
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

    std::lock_guard<std::mutex> lock(metrics_mutex_);
    metrics_.update_latency(execution_latency_ms);
    metrics_.update_reaction_latency(reaction_latency_ms);
    metrics_.successful_orders += (bid_success ? 1 : 0) + (ask_success ? 1 : 0);

    LOG_DEBUG(logger_, "LATENCY reaction={:.3f}ms exec={:.3f}ms",
              reaction_latency_ms, execution_latency_ms);
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
    // Always update order state, regardless of risk_manager presence
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        if (active_bid_order_ && active_bid_order_->order_id == order_id) {
            active_bid_order_->status = status;
            active_bid_order_->executed_quantity += quantity;
            if (status == OrderStatus::FILLED || status == OrderStatus::CANCELED) {
                active_bid_order_.reset();
            }
        } else if (active_ask_order_ && active_ask_order_->order_id == order_id) {
            active_ask_order_->status = status;
            active_ask_order_->executed_quantity += quantity;
            if (status == OrderStatus::FILLED || status == OrderStatus::CANCELED) {
                active_ask_order_.reset();
            }
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
