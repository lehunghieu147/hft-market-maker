#ifndef EXCHANGE_CRTP_ADAPTER_H
#define EXCHANGE_CRTP_ADAPTER_H

#include "core/types.h"
#include <optional>
#include <string>

namespace MarketMaker {

/// CRTP base for zero-cost exchange dispatch on the hot path.
///
/// Virtual dispatch via IExchange is fine for non-hot-path operations
/// (connect, subscribe, account queries). But for order submission in
/// tight trading loops, the vtable indirection is measurable overhead.
///
/// This CRTP base provides compile-time polymorphism for hot-path
/// operations: place_limit_order, cancel_order, get_order_status.
/// Derived classes implement the `*_impl` methods.
///
/// Usage:
///   class MyExchange : public IExchange, public CRTPExchange<MyExchange> {
///       // IExchange overrides for runtime dispatch (factory, config)
///       // CRTPExchange _impl methods for compile-time dispatch (hot path)
///   };
///
///   // Hot path: zero-cost dispatch via CRTP
///   template<typename ExchangeT>
///   void hot_path(CRTPExchange<ExchangeT>& exchange) {
///       auto order = exchange.place_limit_order_fast(symbol, side, price, qty, id);
///   }
template<typename Derived>
class CRTPExchange {
public:
    /// Place limit order with compile-time dispatch (no vtable lookup)
    [[nodiscard]] std::optional<Order> place_limit_order_fast(
        const std::string& symbol,
        OrderSide side,
        double price,
        double quantity,
        const std::string& client_order_id = "")
    {
        return derived().place_limit_order_impl(symbol, side, price, quantity, client_order_id);
    }

    /// Cancel order with compile-time dispatch
    [[nodiscard]] std::optional<bool> cancel_order_fast(
        const std::string& symbol,
        const std::string& order_id)
    {
        return derived().cancel_order_impl(symbol, order_id);
    }

    /// Get order status with compile-time dispatch
    [[nodiscard]] std::optional<Order> get_order_status_fast(
        const std::string& symbol,
        const std::string& order_id)
    {
        return derived().get_order_status_impl(symbol, order_id);
    }

    /// Place IOC order with compile-time dispatch
    [[nodiscard]] std::optional<Order> place_ioc_order_fast(
        const std::string& symbol,
        OrderSide side,
        double price,
        double quantity,
        const std::string& client_order_id = "")
    {
        return derived().place_ioc_order_impl(symbol, side, price, quantity, client_order_id);
    }

    /// Place market order with compile-time dispatch
    [[nodiscard]] std::optional<Order> place_market_order_fast(
        const std::string& symbol,
        OrderSide side,
        double quantity,
        const std::string& client_order_id = "")
    {
        return derived().place_market_order_impl(symbol, side, quantity, client_order_id);
    }

protected:
    ~CRTPExchange() = default; // prevent deletion through base pointer

private:
    Derived& derived() noexcept { return static_cast<Derived&>(*this); }
    const Derived& derived() const noexcept { return static_cast<const Derived&>(*this); }
};

} // namespace MarketMaker

#endif // EXCHANGE_CRTP_ADAPTER_H
