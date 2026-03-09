#ifndef CONCEPTS_H
#define CONCEPTS_H

#include <concepts>
#include <type_traits>
#include <string>
#include <optional>
#include <vector>

namespace MarketMaker {

// Forward declarations for concept constraints
enum class OrderSide;
struct Order;
struct OrderBook;

// Concept: any arithmetic type (int, double, float, etc.)
template<typename T>
concept Numeric = std::is_arithmetic_v<T>;

// Concept: type that behaves like an Order (has required fields)
template<typename T>
concept OrderLike = requires(const T& t) {
    { t.order_id } -> std::convertible_to<std::string>;
    { t.price } -> std::convertible_to<double>;
    { t.quantity } -> std::convertible_to<double>;
    { t.side } -> std::convertible_to<OrderSide>;
};

// Concept: type that implements exchange adapter interface
// Validates at compile time that a type provides the required trading operations
template<typename T>
concept ExchangeAdapter = requires(T& exchange, const std::string& symbol,
                                    OrderSide side, double price, double qty,
                                    const std::string& id) {
    { exchange.place_limit_order(symbol, side, price, qty, id) } -> std::same_as<std::optional<Order>>;
    { exchange.cancel_order(symbol, id) } -> std::same_as<std::optional<bool>>;
    { exchange.get_orderbook(symbol, 20) } -> std::same_as<std::optional<OrderBook>>;
    { exchange.is_connected() } -> std::convertible_to<bool>;
    { exchange.get_exchange_name() } -> std::convertible_to<std::string>;
};

// Concept: type that can track price data (EMA, VWAP, etc.)
template<typename T>
concept PriceTracker = requires(T& tracker, double price) {
    { tracker.update(price) } -> std::same_as<void>;
    { tracker.value() } -> std::convertible_to<double>;
    { tracker.ready() } -> std::convertible_to<bool>;
    { tracker.reset() } -> std::same_as<void>;
};

} // namespace MarketMaker

#endif // CONCEPTS_H
