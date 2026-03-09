#ifndef BACKTEST_ENGINE_H
#define BACKTEST_ENGINE_H

#include "backtesting/data-loader.h"
#include "backtesting/simulated-exchange.h"
#include "backtesting/performance-metrics.h"
#include "core/config.h"
#include <string>
#include <vector>
#include <functional>

namespace MarketMaker {

// Callback for per-tick strategy logic. Receives orderbook + simulated exchange.
using StrategyCallback = std::function<void(const OrderBook&, SimulatedExchange&)>;

// Backtest results container
struct BacktestResult {
    PerformanceMetrics metrics;
    std::vector<BacktestTrade> trades;
    std::vector<double> pnl_curve;  // Cumulative PnL at each trade
    int ticks_processed = 0;
    double elapsed_ms = 0.0;        // Wall-clock time for the backtest
};

// Tick-level replay backtesting engine.
// Loads historical L2 data, feeds it to a strategy callback via SimulatedExchange.
class BacktestEngine {
public:
    BacktestEngine(const SimulationConfig& sim_config = {},
                   int orderbook_depth = 5);

    // Run backtest with a strategy callback
    BacktestResult run(const std::string& data_file, StrategyCallback strategy);

    // Export PnL curve and trades to CSV for external analysis
    static bool export_csv(const BacktestResult& result, const std::string& output_file);

private:
    SimulationConfig sim_config_;
    int orderbook_depth_;
};

} // namespace MarketMaker

#endif // BACKTEST_ENGINE_H
