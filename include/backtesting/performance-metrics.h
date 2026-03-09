#ifndef PERFORMANCE_METRICS_H
#define PERFORMANCE_METRICS_H

#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace MarketMaker {

// Trade record for backtesting analysis
struct BacktestTrade {
    double price;
    double quantity;
    bool is_buy;
    double pnl;         // Realized PnL from this trade
    double fee;
    double timestamp_ms; // Simulated time
};

// Performance metrics computed from backtest results
struct PerformanceMetrics {
    double total_pnl = 0.0;
    double total_fees = 0.0;
    double net_pnl = 0.0;          // total_pnl - total_fees
    double max_drawdown = 0.0;
    double sharpe_ratio = 0.0;
    double win_rate = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;
    int total_trades = 0;
    int winning_trades = 0;
    int losing_trades = 0;

    // Compute all metrics from a PnL curve (vector of cumulative PnL snapshots)
    static PerformanceMetrics compute(const std::vector<BacktestTrade>& trades,
                                       const std::vector<double>& pnl_curve) {
        PerformanceMetrics m;
        m.total_trades = static_cast<int>(trades.size());

        double sum_win = 0.0, sum_loss = 0.0;
        for (const auto& t : trades) {
            m.total_pnl += t.pnl;
            m.total_fees += t.fee;
            if (t.pnl > 0) {
                m.winning_trades++;
                sum_win += t.pnl;
            } else if (t.pnl < 0) {
                m.losing_trades++;
                sum_loss += t.pnl;
            }
        }

        m.net_pnl = m.total_pnl - m.total_fees;
        m.win_rate = m.total_trades > 0
            ? static_cast<double>(m.winning_trades) / m.total_trades : 0.0;
        m.avg_win = m.winning_trades > 0 ? sum_win / m.winning_trades : 0.0;
        m.avg_loss = m.losing_trades > 0 ? sum_loss / m.losing_trades : 0.0;

        // Max drawdown from PnL curve
        if (!pnl_curve.empty()) {
            double peak = pnl_curve[0];
            for (double val : pnl_curve) {
                peak = std::max(peak, val);
                m.max_drawdown = std::min(m.max_drawdown, val - peak);
            }
        }

        // Sharpe ratio (annualized, 365 days for crypto)
        if (pnl_curve.size() > 1) {
            // Compute returns from consecutive PnL values
            std::vector<double> returns;
            returns.reserve(pnl_curve.size() - 1);
            for (size_t i = 1; i < pnl_curve.size(); ++i) {
                returns.push_back(pnl_curve[i] - pnl_curve[i - 1]);
            }

            double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
            double sq_sum = 0.0;
            for (double r : returns) sq_sum += (r - mean) * (r - mean);
            double stddev = std::sqrt(sq_sum / returns.size());

            // Annualize: assume each PnL point is ~1 second of data
            // 365 * 86400 seconds per year
            m.sharpe_ratio = stddev > 0 ? (mean / stddev) * std::sqrt(365.0 * 86400.0) : 0.0;
        }

        return m;
    }
};

} // namespace MarketMaker

#endif // PERFORMANCE_METRICS_H
