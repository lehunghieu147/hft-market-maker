#include "backtesting/backtest-engine.h"
#include "trading/avellaneda-stoikov-model.h"
#include "trading/volatility_tracker.h"
#include <iostream>
#include <string>
#include <cmath>

// Simple market making strategy for backtesting
void market_maker_strategy(const MarketMaker::OrderBook& book,
                           MarketMaker::SimulatedExchange& exchange,
                           MarketMaker::AvellanedaStoikovModel& model,
                           MarketMaker::VolatilityTracker& vol_tracker,
                           double& position, double order_size) {
    double mid = book.get_mid_price();
    if (mid <= 0) return;

    vol_tracker.on_price(mid);
    double vol = vol_tracker.get_volatility();

    // AS model quotes
    auto quote = model.compute(mid, position, vol, 150.0);  // 2.5 min remaining

    // Cancel all resting orders
    exchange.cancel_all_orders("BTCUSDT");

    // Place new bid/ask
    auto bid = exchange.place_limit_order("BTCUSDT", MarketMaker::OrderSide::BUY,
                                           quote.bid_price, order_size, "");
    auto ask = exchange.place_limit_order("BTCUSDT", MarketMaker::OrderSide::SELL,
                                           quote.ask_price, order_size, "");

    // Track position from fills
    if (bid && bid->status == MarketMaker::OrderStatus::FILLED)
        position += order_size;
    if (ask && ask->status == MarketMaker::OrderStatus::FILLED)
        position -= order_size;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: backtest <data_file.csv> [output.csv]\n"
                  << "\nCSV format: timestamp_ms,bid1_price,bid1_qty,...,ask1_price,ask1_qty,...\n"
                  << "\nExample:\n"
                  << "  ./build/bin/backtest data/btcusdt_depth.csv results.csv\n";
        return 1;
    }

    std::string data_file = argv[1];
    std::string output_file = (argc >= 3) ? argv[2] : "";

    // Configure simulation
    MarketMaker::SimulationConfig sim_cfg;
    sim_cfg.latency_ms = 1.0;
    sim_cfg.slippage_bps = 0.5;
    sim_cfg.maker_fee_rate = -0.0001;
    sim_cfg.taker_fee_rate = 0.001;

    MarketMaker::BacktestEngine engine(sim_cfg, 5);

    // Strategy state
    MarketMaker::AvellanedaStoikovModel model(0.001, 1.5, 300.0);
    MarketMaker::VolatilityTracker vol_tracker(100, 0.001, 0.05);
    double position = 0.0;
    double order_size = 0.001;

    auto result = engine.run(data_file,
        [&](const MarketMaker::OrderBook& book, MarketMaker::SimulatedExchange& ex) {
            market_maker_strategy(book, ex, model, vol_tracker, position, order_size);
        });

    if (!output_file.empty()) {
        if (MarketMaker::BacktestEngine::export_csv(result, output_file)) {
            std::cout << "Results exported to: " << output_file << "\n";
        }
    }

    return 0;
}
