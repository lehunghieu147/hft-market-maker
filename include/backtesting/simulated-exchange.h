#ifndef SIMULATED_EXCHANGE_H
#define SIMULATED_EXCHANGE_H

#include "exchange/exchange_interface.h"
#include "backtesting/performance-metrics.h"
#include <unordered_map>
#include <vector>
#include <atomic>

namespace MarketMaker {

// Configuration for simulated exchange behavior
struct SimulationConfig {
    double latency_ms = 1.0;         // Simulated order latency
    double slippage_bps = 1.0;       // Slippage in basis points
    double maker_fee_rate = -0.0001; // Maker rebate
    double taker_fee_rate = 0.001;   // Taker fee
};

// Simulated exchange for backtesting. Maintains a virtual orderbook
// and fills limit orders when price crosses, IOC immediately at best.
class SimulatedExchange : public IExchange {
public:
    explicit SimulatedExchange(const SimulationConfig& sim_config = {});

    // Update the simulated orderbook with new market data
    void update_orderbook(const OrderBook& book);

    // Get all executed trades for analysis
    const std::vector<BacktestTrade>& get_trades() const { return trades_; }

    // IExchange interface implementation
    bool initialize(const ExchangeConfig& config) override;
    bool connect() override { connected_ = true; return true; }
    void disconnect() override { connected_ = false; }
    bool is_connected() const override { return connected_; }

    bool subscribe_orderbook(const std::string&, int) override { return true; }
    bool subscribe_trades(const std::string&) override { return true; }
    bool unsubscribe(const std::string&) override { return true; }

    std::optional<OrderBook> get_orderbook(const std::string&, int) override;
    std::optional<double> get_current_price(const std::string&) override;
    std::optional<std::string> get_exchange_info() override { return "simulated"; }

    [[nodiscard]] std::optional<Order> place_limit_order(
        const std::string& symbol, OrderSide side,
        double price, double quantity, const std::string& client_order_id) override;

    [[nodiscard]] std::optional<Order> place_market_order(
        const std::string& symbol, OrderSide side,
        double quantity, const std::string& client_order_id) override;

    [[nodiscard]] std::optional<Order> place_ioc_order(
        const std::string& symbol, OrderSide side,
        double price, double quantity, const std::string& client_order_id) override;

    [[nodiscard]] std::optional<bool> cancel_order(const std::string&, const std::string& order_id) override;
    [[nodiscard]] std::optional<bool> cancel_all_orders(const std::string&) override;

    std::optional<Order> modify_order(const std::string&, const std::string&,
                                       OrderSide, double, double) override { return std::nullopt; }

    std::optional<std::vector<Order>> get_open_orders(const std::string&) override;
    std::optional<Order> get_order_status(const std::string&, const std::string& order_id) override;

    std::optional<std::string> get_account_info() override { return "simulated"; }
    std::optional<double> get_balance(const std::string&) override { return 10000.0; }

    void set_orderbook_handler(OrderbookHandler handler) override { orderbook_handler_ = handler; }
    void set_message_handler(MessageHandler handler) override { message_handler_ = handler; }
    void set_connection_handler(ConnectionHandler handler) override { connection_handler_ = handler; }
    void set_fill_callback(FillCallback cb) override { fill_callback_ = cb; }

    std::string get_exchange_name() const override { return "simulated"; }
    bool supports_websocket_trading() const override { return true; }
    bool get_symbol_info(const std::string&, int& pp, int& qp) override { pp = 2; qp = 6; return true; }
    double format_price(double p, const std::string&) override { return p; }
    double format_quantity(double q, const std::string&) override { return q; }
    double get_min_order_size(const std::string&) override { return 0.00001; }
    double get_max_order_size(const std::string&) override { return 1000.0; }
    double get_tick_size(const std::string&) override { return 0.01; }

private:
    SimulationConfig sim_config_;
    OrderBook current_book_;
    bool connected_ = false;
    int next_order_id_ = 1;
    double simulated_time_ms_ = 0.0;

    // Open resting limit orders
    std::unordered_map<std::string, Order> resting_orders_;

    // Executed trades
    std::vector<BacktestTrade> trades_;

    // Position tracking for PnL
    double net_position_ = 0.0;
    double cost_basis_ = 0.0;

    FillCallback fill_callback_;

    // Try to fill resting orders against current book
    void check_resting_fills();

    // Record a fill
    void record_fill(const Order& order, double fill_price, double fill_qty, bool is_maker);

    // Generate unique order ID
    std::string gen_order_id();
};

} // namespace MarketMaker

#endif // SIMULATED_EXCHANGE_H
