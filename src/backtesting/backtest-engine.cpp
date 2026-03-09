#include "backtesting/backtest-engine.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iomanip>

namespace MarketMaker {

BacktestEngine::BacktestEngine(const SimulationConfig& sim_config, int orderbook_depth)
    : sim_config_(sim_config), orderbook_depth_(orderbook_depth) {}

BacktestResult BacktestEngine::run(const std::string& data_file, StrategyCallback strategy) {
    BacktestResult result;
    auto wall_start = std::chrono::steady_clock::now();

    DataLoader loader(orderbook_depth_);
    if (!loader.open(data_file)) {
        std::cerr << "[BACKTEST] Failed to open data file: " << data_file << "\n";
        return result;
    }

    SimulatedExchange exchange(sim_config_);
    ExchangeConfig ex_cfg;
    ex_cfg.exchange_type = "simulated";
    exchange.initialize(ex_cfg);
    exchange.connect();

    OrderBook book;
    double timestamp_ms = 0.0;
    double cum_pnl = 0.0;

    while (loader.next(book, timestamp_ms)) {
        // Feed new market data to simulated exchange (checks resting fills)
        exchange.update_orderbook(book);

        // Run strategy logic
        strategy(book, exchange);

        result.ticks_processed++;

        // Track PnL curve at each trade
        const auto& trades = exchange.get_trades();
        while (result.pnl_curve.size() < trades.size()) {
            size_t idx = result.pnl_curve.size();
            cum_pnl += trades[idx].pnl - trades[idx].fee;
            result.pnl_curve.push_back(cum_pnl);
        }
    }

    auto wall_end = std::chrono::steady_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(wall_end - wall_start).count();
    result.trades = exchange.get_trades();
    result.metrics = PerformanceMetrics::compute(result.trades, result.pnl_curve);

    // Print summary
    std::cout << "\n===== BACKTEST RESULTS =====\n"
              << "Ticks processed: " << result.ticks_processed << "\n"
              << "Wall-clock time: " << std::fixed << std::setprecision(1) << result.elapsed_ms << " ms\n"
              << "Total trades:    " << result.metrics.total_trades << "\n"
              << "Win rate:        " << std::setprecision(1) << (result.metrics.win_rate * 100) << "%\n"
              << "Total PnL:       " << std::setprecision(4) << result.metrics.total_pnl << "\n"
              << "Total fees:      " << result.metrics.total_fees << "\n"
              << "Net PnL:         " << result.metrics.net_pnl << "\n"
              << "Max drawdown:    " << result.metrics.max_drawdown << "\n"
              << "Sharpe ratio:    " << std::setprecision(2) << result.metrics.sharpe_ratio << "\n"
              << "Avg win:         " << std::setprecision(4) << result.metrics.avg_win << "\n"
              << "Avg loss:        " << result.metrics.avg_loss << "\n"
              << "============================\n";

    return result;
}

bool BacktestEngine::export_csv(const BacktestResult& result, const std::string& output_file) {
    std::ofstream out(output_file);
    if (!out.is_open()) return false;

    out << "trade_idx,price,quantity,side,pnl,fee,cum_pnl\n";
    double cum_pnl = 0.0;
    for (size_t i = 0; i < result.trades.size(); ++i) {
        const auto& t = result.trades[i];
        cum_pnl += t.pnl - t.fee;
        out << i << "," << std::fixed << std::setprecision(6)
            << t.price << "," << t.quantity << ","
            << (t.is_buy ? "BUY" : "SELL") << ","
            << t.pnl << "," << t.fee << "," << cum_pnl << "\n";
    }

    out.close();
    return true;
}

} // namespace MarketMaker
