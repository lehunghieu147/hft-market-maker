#include "order_validator.h"
#include <cmath>
#include <sstream>
#include <iostream>
#include <iomanip>

namespace MarketMaker {

OrderValidator::OrderValidator(const TradingLimits& limits)
    : limits_(limits) {
    tick_size_ = std::pow(10, -limits_.price_precision);
    lot_size_ = std::pow(10, -limits_.quantity_precision);
}

OrderValidator::ValidationResult OrderValidator::validate_order(
    double price,
    double quantity,
    OrderSide side,
    double current_mid_price) const {

    ValidationResult result{true, "", std::nullopt, std::nullopt};

    // Price validation
    if (!is_price_valid(price)) {
        result.is_valid = false;
        result.error_message = format_validation_error(
            "Price", price, limits_.min_price, limits_.max_price);

        // Suggest adjusted price
        if (price < limits_.min_price) {
            result.suggested_price = limits_.min_price;
        } else if (price > limits_.max_price) {
            result.suggested_price = limits_.max_price;
        } else {
            result.suggested_price = adjust_price_to_tick_size(price);
        }
        return result;
    }

    // Quantity validation
    if (!is_quantity_valid(quantity)) {
        result.is_valid = false;
        result.error_message = format_validation_error(
            "Quantity", quantity, limits_.min_quantity, limits_.max_quantity);

        // Suggest adjusted quantity
        if (quantity < limits_.min_quantity) {
            result.suggested_quantity = limits_.min_quantity;
        } else if (quantity > limits_.max_quantity) {
            result.suggested_quantity = limits_.max_quantity;
        } else {
            result.suggested_quantity = adjust_quantity_to_lot_size(quantity);
        }
        return result;
    }

    // Notional validation
    if (!is_notional_valid(price, quantity)) {
        double notional = price * quantity;
        result.is_valid = false;
        result.error_message = format_validation_error(
            "Notional (price*qty)", notional,
            limits_.min_notional, limits_.max_notional);

        // Suggest adjusted quantity to meet notional requirements
        if (notional < limits_.min_notional) {
            result.suggested_quantity = limits_.min_notional / price;
        } else if (notional > limits_.max_notional) {
            result.suggested_quantity = limits_.max_notional / price;
        }
        return result;
    }

    // Check price sanity against current market
    if (current_mid_price > 0) {
        double price_deviation = std::abs(price - current_mid_price) / current_mid_price;

        // Warn if order is too far from market (potential fat finger)
        const double MAX_DEVIATION = 0.10;  // 10% max deviation
        if (price_deviation > MAX_DEVIATION) {
            result.is_valid = false;
            result.error_message = "Price deviates more than 10% from current market ("
                + std::to_string(current_mid_price) + ")";

            // Suggest price within acceptable range
            if (side == OrderSide::BUY) {
                result.suggested_price = current_mid_price * (1.0 - MAX_DEVIATION);
            } else {
                result.suggested_price = current_mid_price * (1.0 + MAX_DEVIATION);
            }
        }
    }

    return result;
}

OrderValidator::ValidationResult OrderValidator::validate_market_maker_orders(
    double bid_price,
    double ask_price,
    double quantity,
    double mid_price) const {

    ValidationResult result{true, "", std::nullopt, std::nullopt};

    // Validate individual orders
    auto bid_result = validate_order(bid_price, quantity, OrderSide::BUY, mid_price);
    if (!bid_result.is_valid) {
        return bid_result;
    }

    auto ask_result = validate_order(ask_price, quantity, OrderSide::SELL, mid_price);
    if (!ask_result.is_valid) {
        return ask_result;
    }

    // Validate spread
    if (!is_spread_valid(bid_price, ask_price, mid_price)) {
        result.is_valid = false;
        double spread = (ask_price - bid_price) / mid_price;
        result.error_message = "Spread " + std::to_string(spread * 100) + "% is outside valid range ["
            + std::to_string(limits_.min_spread_percentage * 100) + "%, "
            + std::to_string(limits_.max_spread_percentage * 100) + "%]";

        // Suggest adjusted prices
        double target_spread = 0.02;  // Default 2% spread
        result.suggested_price = mid_price * (1.0 - target_spread / 2);  // Bid
        result.suggested_quantity = quantity;  // Keep quantity same
    }

    // Check for crossed orders (bid >= ask)
    if (bid_price >= ask_price) {
        result.is_valid = false;
        result.error_message = "Orders are crossed! Bid price >= Ask price";
        return result;
    }

    return result;
}

double OrderValidator::adjust_price_to_tick_size(double price) const {
    return std::round(price / tick_size_) * tick_size_;
}

double OrderValidator::adjust_quantity_to_lot_size(double quantity) const {
    return std::round(quantity / lot_size_) * lot_size_;
}

bool OrderValidator::check_self_trade_risk(
    const std::shared_ptr<Order>& bid_order,
    const std::shared_ptr<Order>& ask_order) const {

    if (!bid_order || !ask_order) {
        return false;
    }

    // Check if orders could match against each other
    return bid_order->price >= ask_order->price;
}

void OrderValidator::update_limits(const OrderBook& orderbook) {
    // Dynamically adjust limits based on current market conditions
    if (orderbook.bids.empty() || orderbook.asks.empty()) {
        return;
    }

    double best_bid = orderbook.bids[0].price;
    double best_ask = orderbook.asks[0].price;
    double spread = (best_ask - best_bid) / ((best_bid + best_ask) / 2);

    // Adjust spread limits based on current market spread
    if (spread > 0) {
        limits_.min_spread_percentage = std::max(0.0001, spread * 0.5);  // At least 50% of current spread
        limits_.max_spread_percentage = std::min(0.10, spread * 5.0);    // At most 5x current spread
    }

    // Adjust price limits based on current market
    double mid_price = (best_bid + best_ask) / 2;
    limits_.min_price = mid_price * 0.5;   // 50% of current price
    limits_.max_price = mid_price * 2.0;   // 200% of current price
}

bool OrderValidator::is_price_valid(double price) const {
    return price >= limits_.min_price && price <= limits_.max_price && !std::isnan(price) && !std::isinf(price);
}

bool OrderValidator::is_quantity_valid(double quantity) const {
    return quantity >= limits_.min_quantity && quantity <= limits_.max_quantity &&
           !std::isnan(quantity) && !std::isinf(quantity) && quantity > 0;
}

bool OrderValidator::is_notional_valid(double price, double quantity) const {
    double notional = price * quantity;
    return notional >= limits_.min_notional && notional <= limits_.max_notional;
}

bool OrderValidator::is_spread_valid(double bid_price, double ask_price, double mid_price) const {
    if (mid_price <= 0) return true;  // Skip validation if no mid price

    double spread = (ask_price - bid_price) / mid_price;
    return spread >= limits_.min_spread_percentage && spread <= limits_.max_spread_percentage;
}

std::string OrderValidator::format_validation_error(
    const std::string& field,
    double value,
    double min,
    double max) const {

    std::stringstream ss;
    ss << std::fixed << std::setprecision(8);
    ss << field << " " << value << " is outside valid range ["
       << min << ", " << max << "]";
    return ss.str();
}

} // namespace MarketMaker