#include "backtesting/simulated-exchange.h"
#include <cmath>
#include <algorithm>

namespace MarketMaker {

SimulatedExchange::SimulatedExchange(const SimulationConfig& sim_config)
    : sim_config_(sim_config) {}

bool SimulatedExchange::initialize(const ExchangeConfig& config) {
    config_ = config;
    return true;
}

void SimulatedExchange::update_orderbook(const OrderBook& book) {
    current_book_ = book;
    check_resting_fills();

    if (orderbook_handler_) {
        orderbook_handler_(book);
    }
}

std::optional<OrderBook> SimulatedExchange::get_orderbook(const std::string&, int) {
    return current_book_;
}

std::optional<double> SimulatedExchange::get_current_price(const std::string&) {
    return current_book_.get_mid_price();
}

std::optional<Order> SimulatedExchange::place_limit_order(
    const std::string& symbol, OrderSide side,
    double price, double quantity, const std::string& client_order_id) {

    Order order;
    order.order_id = gen_order_id();
    order.client_order_id = client_order_id;
    order.symbol = symbol;
    order.side = side;
    order.price = price;
    order.quantity = quantity;
    order.executed_quantity = 0.0;
    order.status = OrderStatus::NEW;
    order.created_time = std::chrono::steady_clock::now();

    // Check immediate fill: buy above best ask or sell below best bid
    bool immediate_fill = false;
    if (side == OrderSide::BUY && !current_book_.asks.empty() &&
        price >= current_book_.asks[0].price) {
        immediate_fill = true;
    }
    if (side == OrderSide::SELL && !current_book_.bids.empty() &&
        price <= current_book_.bids[0].price) {
        immediate_fill = true;
    }

    if (immediate_fill) {
        double fill_price = (side == OrderSide::BUY)
            ? current_book_.asks[0].price : current_book_.bids[0].price;
        // Apply slippage
        double slippage = fill_price * sim_config_.slippage_bps * 1e-4;
        fill_price += (side == OrderSide::BUY) ? slippage : -slippage;

        order.status = OrderStatus::FILLED;
        order.executed_quantity = quantity;
        record_fill(order, fill_price, quantity, false);  // taker
    } else {
        // Resting order
        resting_orders_[order.order_id] = order;
    }

    return order;
}

std::optional<Order> SimulatedExchange::place_market_order(
    const std::string& symbol, OrderSide side,
    double quantity, const std::string& client_order_id) {

    double fill_price = 0.0;
    if (side == OrderSide::BUY && !current_book_.asks.empty()) {
        fill_price = current_book_.asks[0].price;
    } else if (side == OrderSide::SELL && !current_book_.bids.empty()) {
        fill_price = current_book_.bids[0].price;
    } else {
        return std::nullopt;
    }

    // Apply slippage
    double slippage = fill_price * sim_config_.slippage_bps * 1e-4;
    fill_price += (side == OrderSide::BUY) ? slippage : -slippage;

    Order order;
    order.order_id = gen_order_id();
    order.client_order_id = client_order_id;
    order.symbol = symbol;
    order.side = side;
    order.price = fill_price;
    order.quantity = quantity;
    order.executed_quantity = quantity;
    order.status = OrderStatus::FILLED;
    order.created_time = std::chrono::steady_clock::now();

    record_fill(order, fill_price, quantity, false);
    return order;
}

std::optional<Order> SimulatedExchange::place_ioc_order(
    const std::string& symbol, OrderSide side,
    double price, double quantity, const std::string& client_order_id) {

    // IOC: fill immediately if price is marketable, otherwise cancel
    double fill_price = 0.0;
    bool can_fill = false;

    if (side == OrderSide::BUY && !current_book_.asks.empty() &&
        price >= current_book_.asks[0].price) {
        fill_price = current_book_.asks[0].price;
        can_fill = true;
    } else if (side == OrderSide::SELL && !current_book_.bids.empty() &&
               price <= current_book_.bids[0].price) {
        fill_price = current_book_.bids[0].price;
        can_fill = true;
    }

    Order order;
    order.order_id = gen_order_id();
    order.client_order_id = client_order_id;
    order.symbol = symbol;
    order.side = side;
    order.price = price;
    order.quantity = quantity;
    order.created_time = std::chrono::steady_clock::now();

    if (can_fill) {
        double slippage = fill_price * sim_config_.slippage_bps * 1e-4;
        fill_price += (side == OrderSide::BUY) ? slippage : -slippage;
        order.executed_quantity = quantity;
        order.status = OrderStatus::FILLED;
        record_fill(order, fill_price, quantity, false);
    } else {
        order.executed_quantity = 0.0;
        order.status = OrderStatus::EXPIRED;
    }

    return order;
}

std::optional<bool> SimulatedExchange::cancel_order(const std::string&, const std::string& order_id) {
    auto it = resting_orders_.find(order_id);
    if (it != resting_orders_.end()) {
        it->second.status = OrderStatus::CANCELED;
        resting_orders_.erase(it);
        return true;
    }
    return false;
}

std::optional<bool> SimulatedExchange::cancel_all_orders(const std::string&) {
    resting_orders_.clear();
    return true;
}

std::optional<std::vector<Order>> SimulatedExchange::get_open_orders(const std::string&) {
    std::vector<Order> orders;
    for (const auto& [id, order] : resting_orders_) {
        orders.push_back(order);
    }
    return orders;
}

std::optional<Order> SimulatedExchange::get_order_status(const std::string&, const std::string& order_id) {
    auto it = resting_orders_.find(order_id);
    if (it != resting_orders_.end()) return it->second;
    return std::nullopt;
}

void SimulatedExchange::check_resting_fills() {
    if (current_book_.bids.empty() || current_book_.asks.empty()) return;

    std::vector<std::string> filled_ids;

    for (auto& [id, order] : resting_orders_) {
        bool should_fill = false;
        double fill_price = order.price;

        // Buy order fills when ask drops to or below order price
        if (order.side == OrderSide::BUY && current_book_.asks[0].price <= order.price) {
            should_fill = true;
            fill_price = order.price;  // Maker fill at limit price
        }
        // Sell order fills when bid rises to or above order price
        if (order.side == OrderSide::SELL && current_book_.bids[0].price >= order.price) {
            should_fill = true;
            fill_price = order.price;
        }

        if (should_fill) {
            order.status = OrderStatus::FILLED;
            order.executed_quantity = order.quantity;
            record_fill(order, fill_price, order.quantity, true);  // maker
            filled_ids.push_back(id);
        }
    }

    for (const auto& id : filled_ids) {
        resting_orders_.erase(id);
    }
}

void SimulatedExchange::record_fill(const Order& order, double fill_price,
                                     double fill_qty, bool is_maker) {
    double fee_rate = is_maker ? sim_config_.maker_fee_rate : sim_config_.taker_fee_rate;
    double fee = std::abs(fill_price * fill_qty * fee_rate);

    // PnL tracking: compute realized PnL when closing position
    double pnl = 0.0;
    double signed_qty = (order.side == OrderSide::BUY) ? fill_qty : -fill_qty;
    double closing_qty = 0.0;

    // Check if this trade reduces position (realizes PnL)
    if ((net_position_ > 0 && order.side == OrderSide::SELL) ||
        (net_position_ < 0 && order.side == OrderSide::BUY)) {
        closing_qty = std::min(std::abs(signed_qty), std::abs(net_position_));
        double avg_entry = (net_position_ != 0.0) ? cost_basis_ / net_position_ : 0.0;

        if (order.side == OrderSide::SELL) {
            pnl = closing_qty * (fill_price - avg_entry);
        } else {
            pnl = closing_qty * (avg_entry - fill_price);
        }

        // Reduce cost_basis proportionally for the closed portion
        double proportion = closing_qty / std::abs(net_position_);
        cost_basis_ *= (1.0 - proportion);
    }

    net_position_ += signed_qty;
    // Add cost basis only for the opening portion
    double opening_qty = std::abs(signed_qty) - closing_qty;
    if (opening_qty > 0) {
        cost_basis_ += opening_qty * fill_price * (signed_qty > 0 ? 1.0 : -1.0);
    }

    // Negative fee = rebate (maker)
    double actual_fee = (fee_rate < 0) ? -fee : fee;

    trades_.push_back(BacktestTrade{
        fill_price, fill_qty, order.side == OrderSide::BUY,
        pnl, actual_fee, simulated_time_ms_
    });

    // Fire fill callback if set
    if (fill_callback_) {
        fill_callback_(order.order_id, order.client_order_id,
                       order.side, OrderStatus::FILLED,
                       fill_price, fill_qty, fill_qty);
    }
}

std::string SimulatedExchange::gen_order_id() {
    return "SIM_" + std::to_string(next_order_id_++);
}

} // namespace MarketMaker
