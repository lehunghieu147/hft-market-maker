#ifndef ORDER_VALIDATOR_H
#define ORDER_VALIDATOR_H

#include "types.h"
#include <string>
#include <optional>
#include <iostream>

namespace MarketMaker {

class OrderValidator {
public:
    struct ValidationResult {
        bool is_valid;
        std::string error_message;

        // Suggested corrections
        std::optional<double> suggested_price;
        std::optional<double> suggested_quantity;
    };

    struct TradingLimits {
        double min_price;
        double max_price;
        double min_quantity;
        double max_quantity;
        double min_notional;  // Minimum order value (price * quantity)
        double max_notional;
        int price_precision;
        int quantity_precision;

        // Dynamic limits based on market conditions
        double max_spread_percentage;  // Max 10% spread
        double min_spread_percentage; // Min 0.1% spread

        // Constructor with defaults
        TradingLimits()
            : min_price(0.01),
              max_price(1000000.0),
              min_quantity(0.00001),
              max_quantity(10000.0),
              min_notional(10.0),
              max_notional(100000.0),
              price_precision(2),
              quantity_precision(5),
              max_spread_percentage(0.10),
              min_spread_percentage(0.001) {}
    };

    OrderValidator(const TradingLimits& limits = TradingLimits());

    // Validate a single order
    ValidationResult validate_order(
        double price,
        double quantity,
        OrderSide side,
        double current_mid_price = 0
    ) const;

    // Validate a pair of market maker orders
    ValidationResult validate_market_maker_orders(
        double bid_price,
        double ask_price,
        double quantity,
        double mid_price
    ) const;

    // Check if price needs adjustment for tick size
    double adjust_price_to_tick_size(double price) const;
    double adjust_quantity_to_lot_size(double quantity) const;

    // Validate against exchange rules
    bool check_self_trade_risk(
        const std::shared_ptr<Order>& bid_order,
        const std::shared_ptr<Order>& ask_order
    ) const;

    // Update limits dynamically based on market conditions
    void update_limits(const OrderBook& orderbook);

    // Get current limits
    const TradingLimits& get_limits() const { return limits_; }

private:
    TradingLimits limits_;
    double tick_size_ = 0.01;
    double lot_size_ = 0.00001;

    // Helper functions
    bool is_price_valid(double price) const;
    bool is_quantity_valid(double quantity) const;
    bool is_notional_valid(double price, double quantity) const;
    bool is_spread_valid(double bid_price, double ask_price, double mid_price) const;

    std::string format_validation_error(
        const std::string& field,
        double value,
        double min,
        double max
    ) const;
};

// Singleton for global validation
class GlobalOrderValidator {
public:
    static GlobalOrderValidator& instance() {
        static GlobalOrderValidator instance;
        return instance;
    }

    bool pre_validate_order(double price, double quantity, OrderSide side) {
        auto result = validator_.validate_order(price, quantity, side);
        if (!result.is_valid) {
            std::cerr << "[VALIDATION] Order rejected: " << result.error_message << std::endl;

            if (result.suggested_price) {
                std::cerr << "[VALIDATION] Suggested price: " << *result.suggested_price << std::endl;
            }
            if (result.suggested_quantity) {
                std::cerr << "[VALIDATION] Suggested quantity: " << *result.suggested_quantity << std::endl;
            }
        }
        return result.is_valid;
    }

    void update_from_orderbook(const OrderBook& orderbook) {
        validator_.update_limits(orderbook);
    }

private:
    GlobalOrderValidator() = default;
    OrderValidator validator_;
};

} // namespace MarketMaker

#endif // ORDER_VALIDATOR_H