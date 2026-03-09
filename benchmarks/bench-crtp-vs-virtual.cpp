/// Benchmark: CRTP compile-time dispatch vs virtual dispatch vs direct call.
/// Uses CRTPExchange<Derived> from include/exchange/exchange-crtp-adapter.h
/// and IExchange virtual interface from include/exchange/exchange_interface.h

#include <benchmark/benchmark.h>
#include "exchange/exchange_interface.h"
#include "exchange/exchange-crtp-adapter.h"

#include <optional>
#include <string>
#include <vector>

namespace {

// ─── MockExchange: implements both IExchange (virtual) and CRTPExchange<> ───

class MockExchange
    : public MarketMaker::IExchange
    , public MarketMaker::CRTPExchange<MockExchange>
{
public:
    // ── IExchange pure virtual overrides (non-hot-path stubs) ────────────────

    bool initialize(const MarketMaker::ExchangeConfig&) override { return true; }
    bool connect() override { return true; }
    void disconnect() override {}
    bool is_connected() const override { return true; }

    bool subscribe_orderbook(const std::string&, int) override { return true; }
    bool subscribe_trades(const std::string&) override { return true; }
    bool unsubscribe(const std::string&) override { return true; }

    std::optional<MarketMaker::OrderBook> get_orderbook(const std::string&, int) override {
        return std::nullopt;
    }
    std::optional<double> get_current_price(const std::string&) override {
        return std::nullopt;
    }
    std::optional<std::string> get_exchange_info() override { return std::nullopt; }

    // Hot-path via virtual (IExchange*)
    [[nodiscard]] std::optional<MarketMaker::Order> place_limit_order(
        const std::string& symbol,
        MarketMaker::OrderSide side,
        double price,
        double quantity,
        const std::string& client_order_id = "") override
    {
        return build_order(symbol, side, price, quantity, client_order_id);
    }

    [[nodiscard]] std::optional<MarketMaker::Order> place_market_order(
        const std::string& symbol,
        MarketMaker::OrderSide side,
        double quantity,
        const std::string& client_order_id = "") override
    {
        return build_order(symbol, side, 0.0, quantity, client_order_id);
    }

    [[nodiscard]] std::optional<bool> cancel_order(
        const std::string&, const std::string&) override { return true; }

    [[nodiscard]] std::optional<bool> cancel_all_orders(const std::string&) override {
        return true;
    }

    std::optional<MarketMaker::Order> modify_order(
        const std::string&, const std::string&,
        MarketMaker::OrderSide, double, double) override { return std::nullopt; }

    std::optional<std::vector<MarketMaker::Order>> get_open_orders(const std::string&) override {
        return std::nullopt;
    }
    std::optional<MarketMaker::Order> get_order_status(
        const std::string&, const std::string&) override { return std::nullopt; }

    std::optional<std::string> get_account_info() override { return std::nullopt; }
    std::optional<double> get_balance(const std::string&) override { return std::nullopt; }

    void set_orderbook_handler(OrderbookHandler h) override { orderbook_handler_ = std::move(h); }
    void set_message_handler(MessageHandler h) override { message_handler_ = std::move(h); }
    void set_connection_handler(ConnectionHandler h) override { connection_handler_ = std::move(h); }

    std::string get_exchange_name() const override { return "mock"; }
    bool supports_websocket_trading() const override { return false; }

    bool get_symbol_info(const std::string&, int& pp, int& qp) override {
        pp = 2; qp = 8; return true;
    }
    double format_price(double p, const std::string&) override { return p; }
    double format_quantity(double q, const std::string&) override { return q; }
    double get_min_order_size(const std::string&) override { return 0.0001; }
    double get_max_order_size(const std::string&) override { return 1000.0; }
    double get_tick_size(const std::string&) override { return 0.01; }

    // ── CRTPExchange _impl methods (hot-path, compile-time dispatch) ─────────

    [[nodiscard]] std::optional<MarketMaker::Order> place_limit_order_impl(
        const std::string& symbol,
        MarketMaker::OrderSide side,
        double price,
        double quantity,
        const std::string& client_order_id)
    {
        return build_order(symbol, side, price, quantity, client_order_id);
    }

    [[nodiscard]] std::optional<bool> cancel_order_impl(
        const std::string&, const std::string&) { return true; }

    [[nodiscard]] std::optional<MarketMaker::Order> get_order_status_impl(
        const std::string&, const std::string&) { return std::nullopt; }

    [[nodiscard]] std::optional<MarketMaker::Order> place_ioc_order_impl(
        const std::string& symbol,
        MarketMaker::OrderSide side,
        double price,
        double quantity,
        const std::string& client_order_id)
    {
        return build_order(symbol, side, price, quantity, client_order_id);
    }

    [[nodiscard]] std::optional<MarketMaker::Order> place_market_order_impl(
        const std::string& symbol,
        MarketMaker::OrderSide side,
        double quantity,
        const std::string& client_order_id)
    {
        return build_order(symbol, side, 0.0, quantity, client_order_id);
    }

private:
    // Shared builder used by all paths so the work is identical per benchmark
    [[nodiscard]] std::optional<MarketMaker::Order> build_order(
        const std::string& symbol,
        MarketMaker::OrderSide side,
        double price,
        double quantity,
        const std::string& client_order_id)
    {
        MarketMaker::Order o;
        o.order_id        = "ORD-001";
        o.client_order_id = client_order_id;
        o.symbol          = symbol;
        o.side            = side;
        o.price           = price;
        o.quantity        = quantity;
        o.executed_quantity = 0.0;
        o.status          = MarketMaker::OrderStatus::NEW;
        o.created_time    = std::chrono::steady_clock::now();
        o.updated_time    = o.created_time;
        return o;
    }
};

// ─── Fixtures ────────────────────────────────────────────────────────────────

static const std::string kSymbol  = "BTCUSDT";
static const std::string kClOrdId = "C-001";

// ─── BM_VirtualDispatch: call through IExchange* (vtable lookup) ─────────────

static void BM_VirtualDispatch(benchmark::State& state) {
    MockExchange mock;
    MarketMaker::IExchange* iface = &mock;  // Forces virtual dispatch

    for (auto _ : state) {
        auto result = iface->place_limit_order(
            kSymbol, MarketMaker::OrderSide::BUY, 45000.0, 0.01, kClOrdId);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_VirtualDispatch);

// ─── BM_CRTPDispatch: call through CRTPExchange<MockExchange>& (no vtable) ───

static void BM_CRTPDispatch(benchmark::State& state) {
    MockExchange mock;
    MarketMaker::CRTPExchange<MockExchange>& crtp = mock;  // CRTP base reference

    for (auto _ : state) {
        auto result = crtp.place_limit_order_fast(
            kSymbol, MarketMaker::OrderSide::BUY, 45000.0, 0.01, kClOrdId);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CRTPDispatch);

// ─── BM_DirectCall: call MockExchange directly (inlining baseline) ───────────

static void BM_DirectCall(benchmark::State& state) {
    MockExchange mock;

    for (auto _ : state) {
        auto result = mock.place_limit_order(
            kSymbol, MarketMaker::OrderSide::BUY, 45000.0, 0.01, kClOrdId);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DirectCall);

} // namespace

BENCHMARK_MAIN();
