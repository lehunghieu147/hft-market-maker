#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>
#include <iomanip>
#include <sstream>
#include <fstream>

// Phase B Headers
#include "trading/avellaneda-stoikov-model.h"
#include "trading/orderbook-imbalance-tracker.h"
#include "trading/vwap-tracker.h"
#include "trading/volatility_tracker.h"
#include "trading/risk_manager.h"
#include "backtesting/simulated-exchange.h"
#include "backtesting/data-loader.h"
#include "backtesting/performance-metrics.h"
#include "core/types.h"

using namespace MarketMaker;

// ============================================================================
// Test Utilities
// ============================================================================

struct TestResult {
    std::string name;
    bool passed;
    std::string error_msg;
};

std::vector<TestResult> test_results;

void test_assert(const std::string& name, bool condition, const std::string& msg = "") {
    TestResult result{name, condition, msg};
    test_results.push_back(result);
    if (!condition) {
        std::cerr << "FAIL: " << name << " - " << msg << std::endl;
    } else {
        std::cout << "PASS: " << name << std::endl;
    }
}

bool approx_equal(double a, double b, double tol = 1e-6) {
    return std::abs(a - b) < tol;
}

void print_summary() {
    int passed = 0, failed = 0;
    for (const auto& r : test_results) {
        if (r.passed) passed++;
        else failed++;
    }

    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "TEST SUMMARY" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    std::cout << "Total: " << test_results.size() << " | Passed: " << passed
              << " | Failed: " << failed << std::endl;

    if (failed > 0) {
        std::cout << "\nFailed Tests:\n";
        for (const auto& r : test_results) {
            if (!r.passed) {
                std::cout << "  - " << r.name << ": " << r.error_msg << std::endl;
            }
        }
    }
    std::cout << std::string(70, '=') << std::endl;
}

// ============================================================================
// B1: Avellaneda-Stoikov Model Tests
// ============================================================================

void test_as_model() {
    std::cout << "\n[B1] Avellaneda-Stoikov Model Tests" << std::endl;
    std::cout << std::string(70, '-') << std::endl;

    AvellanedaStoikovModel model(0.001, 1.5, 300.0);

    // Test 1: Neutral position (inventory = 0)
    {
        auto result = model.compute(100.0, 0.0, 0.01, 300.0);
        test_assert(
            "AS: Neutral inventory - symmetric quotes",
            approx_equal(result.bid_price + result.ask_price, 2.0 * result.reservation_price),
            "Bid+Ask != 2*Reservation"
        );
        test_assert(
            "AS: Neutral inventory - reservation = mid",
            approx_equal(result.reservation_price, 100.0),
            "Reservation != mid price"
        );
    }

    // Test 2: Long inventory (positive) - should lower reservation price
    {
        auto result_long = model.compute(100.0, 1.0, 0.01, 300.0);
        auto result_neutral = model.compute(100.0, 0.0, 0.01, 300.0);
        test_assert(
            "AS: Long position - lowers reservation price",
            result_long.reservation_price < result_neutral.reservation_price,
            "Long reservation >= neutral"
        );
        test_assert(
            "AS: Long position - wants to sell",
            result_long.bid_price < result_neutral.bid_price &&
            result_long.ask_price < result_neutral.ask_price,
            "Long quotes not shifted lower"
        );
    }

    // Test 3: Short inventory (negative) - should raise reservation price
    {
        auto result_short = model.compute(100.0, -1.0, 0.01, 300.0);
        auto result_neutral = model.compute(100.0, 0.0, 0.01, 300.0);
        test_assert(
            "AS: Short position - raises reservation price",
            result_short.reservation_price > result_neutral.reservation_price,
            "Short reservation <= neutral"
        );
    }

    // Test 4: Higher volatility - wider spread
    {
        auto result_low_vol = model.compute(100.0, 0.0, 0.005, 300.0);
        auto result_high_vol = model.compute(100.0, 0.0, 0.020, 300.0);
        test_assert(
            "AS: Higher volatility - wider spread",
            result_high_vol.optimal_spread > result_low_vol.optimal_spread,
            "High vol spread not wider than low vol"
        );
    }

    // Test 5: Spread narrows as time_remaining decreases (approaching horizon)
    {
        auto result_far = model.compute(100.0, 0.0, 0.01, 300.0);
        auto result_near = model.compute(100.0, 0.0, 0.01, 10.0);
        test_assert(
            "AS: Time horizon - spread narrows near end",
            result_near.optimal_spread < result_far.optimal_spread,
            "Spread doesn't narrow as horizon approaches"
        );
    }

    // Test 6: No crossed orders (bid < ask always)
    {
        auto result = model.compute(100.0, 5.0, 0.05, 300.0);
        test_assert(
            "AS: Valid quote - bid < ask",
            result.bid_price < result.ask_price,
            std::string("Crossed order: bid=") + std::to_string(result.bid_price) +
            ", ask=" + std::to_string(result.ask_price)
        );
    }
}

// ============================================================================
// B3: Order Book Imbalance Tests
// ============================================================================

void test_obi_tracker() {
    std::cout << "\n[B3] Order Book Imbalance Tracker Tests" << std::endl;
    std::cout << std::string(70, '-') << std::endl;

    OrderBookImbalanceTracker obi(5, 0.3, 50.0);

    // Test 1: Perfect balance = OBI near 0
    {
        OrderBook book;
        book.bids = {{100.0, 10.0}, {99.5, 10.0}, {99.0, 10.0}, {98.5, 10.0}, {98.0, 10.0}};
        book.asks = {{100.5, 10.0}, {101.0, 10.0}, {101.5, 10.0}, {102.0, 10.0}, {102.5, 10.0}};

        double obi_val = obi.compute(book);
        test_assert(
            "OBI: Balanced book - OBI near 0",
            std::abs(obi_val) < 0.1,
            "OBI = " + std::to_string(obi_val)
        );
    }

    // Test 2: Buy pressure (more bid volume) = positive OBI
    {
        OrderBook book;
        book.bids = {{100.0, 20.0}, {99.5, 20.0}, {99.0, 20.0}, {98.5, 20.0}, {98.0, 20.0}};
        book.asks = {{100.5, 5.0}, {101.0, 5.0}, {101.5, 5.0}, {102.0, 5.0}, {102.5, 5.0}};

        double obi_val = obi.compute(book);
        test_assert(
            "OBI: Buy pressure - positive",
            obi_val > 0.5,
            "OBI = " + std::to_string(obi_val)
        );
    }

    // Test 3: Sell pressure (more ask volume) = negative OBI
    {
        OrderBook book;
        book.bids = {{100.0, 5.0}, {99.5, 5.0}, {99.0, 5.0}, {98.5, 5.0}, {98.0, 5.0}};
        book.asks = {{100.5, 20.0}, {101.0, 20.0}, {101.5, 20.0}, {102.0, 20.0}, {102.5, 20.0}};

        double obi_val = obi.compute(book);
        test_assert(
            "OBI: Sell pressure - negative",
            obi_val < -0.5,
            "OBI = " + std::to_string(obi_val)
        );
    }

    // Test 4: OBI range [-1, 1]
    {
        for (int i = 1; i <= 10; i++) {
            OrderBook book;
            double bid_vol = i * 5.0;
            double ask_vol = (11 - i) * 5.0;
            for (int j = 0; j < 5; j++) {
                book.bids.push_back({100.0 - j * 0.5, bid_vol / 5.0});
                book.asks.push_back({100.5 + j * 0.5, ask_vol / 5.0});
            }

            double obi_val = obi.compute(book);
            test_assert(
                "OBI: Range check [" + std::to_string(i) + "]",
                obi_val >= -1.0 && obi_val <= 1.0,
                "OBI = " + std::to_string(obi_val)
            );
        }
    }

    // Test 5: EMA smoothing reduces volatility
    {
        OrderBook book1;
        book1.bids = {{100.0, 50.0}};
        book1.asks = {{100.5, 5.0}};

        OrderBook book2;
        book2.bids = {{100.0, 5.0}};
        book2.asks = {{100.5, 50.0}};

        obi.reset();
        double smooth1 = obi.update(book1);  // Big swing first
        double smooth2 = obi.update(book2);  // Big swing opposite

        test_assert(
            "OBI: EMA smoothing - second value closer to previous",
            std::abs(smooth2 - smooth1) < std::abs(obi.raw_obi() - smooth1),
            "Smoothing not effective"
        );
    }

    // Test 6: Significance check (min volume threshold)
    {
        OrderBook book;
        book.bids = {{100.0, 30.0}};
        book.asks = {{100.5, 20.0}};
        obi.update(book);
        test_assert(
            "OBI: Significance - above threshold",
            obi.is_significant(),
            "Should be significant (volume=50)"
        );
    }
}

// ============================================================================
// VWAP Tracker Tests
// ============================================================================

void test_vwap_tracker() {
    std::cout << "\n[B2] VWAP Tracker Tests" << std::endl;
    std::cout << std::string(70, '-') << std::endl;

    VwapTracker vwap;

    // Test 1: Single observation = VWAP equals price
    {
        vwap.reset();
        vwap.update(100.0, 10.0);
        test_assert(
            "VWAP: Single sample = price",
            approx_equal(vwap.value(), 100.0),
            "VWAP = " + std::to_string(vwap.value())
        );
    }

    // Test 2: Weighted average calculation
    {
        vwap.reset();
        vwap.update(100.0, 10.0);  // 100*10 = 1000
        vwap.update(110.0, 10.0);  // 110*10 = 1100, total = 2100, vol = 20
        // Expected: 2100/20 = 105
        test_assert(
            "VWAP: Two samples - correct weighted average",
            approx_equal(vwap.value(), 105.0),
            "VWAP = " + std::to_string(vwap.value())
        );
    }

    // Test 3: Volume weighting
    {
        vwap.reset();
        vwap.update(100.0, 1.0);   // Small volume
        vwap.update(110.0, 9.0);   // Large volume
        // Expected: (100*1 + 110*9) / 10 = 1090 / 10 = 109
        test_assert(
            "VWAP: Volume weighting",
            approx_equal(vwap.value(), 109.0),
            "VWAP = " + std::to_string(vwap.value())
        );
    }

    // Test 4: Standard deviation bands
    {
        vwap.reset();
        // Add samples around a price
        for (int i = 0; i < 15; i++) {
            vwap.update(100.0 + i * 0.5, 10.0);
        }
        double val = vwap.value();
        double std = vwap.stddev();
        test_assert(
            "VWAP: Stddev >= 0",
            std >= 0.0,
            "Stddev = " + std::to_string(std)
        );
        test_assert(
            "VWAP: Upper band > value",
            vwap.upper_band() > val,
            "Upper = " + std::to_string(vwap.upper_band())
        );
        test_assert(
            "VWAP: Lower band < value",
            vwap.lower_band() < val,
            "Lower = " + std::to_string(vwap.lower_band())
        );
    }

    // Test 5: Ready flag (needs >= 10 samples)
    {
        vwap.reset();
        for (int i = 0; i < 9; i++) {
            vwap.update(100.0, 10.0);
        }
        test_assert(
            "VWAP: Not ready with 9 samples",
            !vwap.ready(),
            "Ready with only 9 samples"
        );

        vwap.update(100.0, 10.0);  // 10th sample
        test_assert(
            "VWAP: Ready with 10 samples",
            vwap.ready(),
            "Not ready with 10 samples"
        );
    }

    // Test 6: Ignore zero/negative volume
    {
        vwap.reset();
        vwap.update(100.0, 10.0);
        int count_before = vwap.sample_count();
        vwap.update(110.0, 0.0);
        vwap.update(120.0, -5.0);
        int count_after = vwap.sample_count();
        test_assert(
            "VWAP: Ignore invalid volumes",
            count_before == count_after,
            "Count changed from " + std::to_string(count_before) +
            " to " + std::to_string(count_after)
        );
    }
}

// ============================================================================
// Volatility Tracker Tests
// ============================================================================

void test_volatility_tracker() {
    std::cout << "\n[B4] Volatility Tracker Tests" << std::endl;
    std::cout << std::string(70, '-') << std::endl;

    VolatilityTracker vol(100, 0.001, 0.05);

    // Test 1: Constant prices = zero volatility
    {
        vol.reset();
        for (int i = 0; i < 20; i++) {
            vol.on_price(100.0);
        }
        double volatility = vol.get_volatility();
        test_assert(
            "Volatility: Constant prices = 0",
            volatility < 0.01,
            "Volatility = " + std::to_string(volatility)
        );
    }

    // Test 2: Volatile prices = higher volatility
    {
        VolatilityTracker vol_stable(100, 0.001, 0.05);
        VolatilityTracker vol_volatile(100, 0.001, 0.05);

        // Stable: small moves
        for (int i = 0; i < 20; i++) {
            vol_stable.on_price(100.0 + i * 0.001);
        }

        // Volatile: large moves
        for (int i = 0; i < 20; i++) {
            vol_volatile.on_price(100.0 + i * 0.1);
        }

        test_assert(
            "Volatility: Larger moves = higher volatility",
            vol_volatile.get_volatility() > vol_stable.get_volatility(),
            "Stable=" + std::to_string(vol_stable.get_volatility()) +
            " vs Volatile=" + std::to_string(vol_volatile.get_volatility())
        );
    }

    // Test 3: get_adjusted_spread clamps to min/max
    {
        vol.reset();
        for (int i = 0; i < 20; i++) vol.on_price(100.0);

        double adjusted = vol.get_adjusted_spread(0.02);
        test_assert(
            "Volatility: Adjusted spread >= min_spread",
            adjusted >= 0.001,
            "Adjusted = " + std::to_string(adjusted)
        );
        test_assert(
            "Volatility: Adjusted spread <= max_spread",
            adjusted <= 0.05,
            "Adjusted = " + std::to_string(adjusted)
        );
    }

    // Test 4: baseline_volatility and volatility_ratio
    {
        vol.reset();
        for (int i = 0; i < 50; i++) {
            vol.on_price(100.0 + std::sin(i * 0.1) * 0.01);
        }

        double baseline = vol.get_baseline_volatility();
        double ratio = vol.get_volatility_ratio();

        test_assert(
            "Volatility: Baseline >= 0",
            baseline >= 0.0,
            "Baseline = " + std::to_string(baseline)
        );
        test_assert(
            "Volatility: Ratio >= 0.5 (lower bound)",
            ratio >= 0.5,
            "Ratio = " + std::to_string(ratio)
        );
        test_assert(
            "Volatility: Ratio <= 2.0 (upper bound)",
            ratio <= 2.0,
            "Ratio = " + std::to_string(ratio)
        );
    }
}

// ============================================================================
// Risk Manager Dynamic Sizing Tests
// ============================================================================

void test_risk_manager_dynamic_sizing() {
    std::cout << "\n[B4] Risk Manager - Dynamic Sizing Tests" << std::endl;
    std::cout << std::string(70, '-') << std::endl;

    RiskConfig config;
    config.max_position_size = 1.0;
    RiskManager rm(config);

    auto vol_tracker = std::make_shared<VolatilityTracker>(100, 0.001, 0.05);
    rm.set_volatility_tracker(vol_tracker);

    // Test 1: High volatility shrinks order size
    {
        vol_tracker->reset();
        // High volatility
        for (int i = 0; i < 30; i++) {
            vol_tracker->on_price(100.0 + std::sin(i * 0.2) * 0.05);
        }
        double high_vol_size = rm.adjusted_order_size(1.0, 0.5, 0.25, 2.0);

        // Low volatility
        vol_tracker->reset();
        for (int i = 0; i < 30; i++) {
            vol_tracker->on_price(100.0 + std::sin(i * 0.2) * 0.005);
        }
        double low_vol_size = rm.adjusted_order_size(1.0, 0.5, 0.25, 2.0);

        test_assert(
            "Risk: High vol = smaller orders",
            high_vol_size <= low_vol_size,
            "High vol=" + std::to_string(high_vol_size) +
            " vs Low vol=" + std::to_string(low_vol_size)
        );
    }

    // Test 2: Adjusted position limit respects bounds
    {
        vol_tracker->reset();
        for (int i = 0; i < 30; i++) {
            vol_tracker->on_price(100.0 + i * 0.01);
        }
        double adjusted_limit = rm.adjusted_position_limit(1.0);
        test_assert(
            "Risk: Adjusted limit <= base",
            adjusted_limit <= 1.0,
            "Adjusted = " + std::to_string(adjusted_limit)
        );
    }
}

// ============================================================================
// Simulated Exchange Tests
// ============================================================================

void test_simulated_exchange() {
    std::cout << "\n[B5] Simulated Exchange Tests" << std::endl;
    std::cout << std::string(70, '-') << std::endl;

    SimulationConfig sim_cfg;
    sim_cfg.latency_ms = 1.0;
    sim_cfg.slippage_bps = 1.0;
    sim_cfg.maker_fee_rate = -0.0001;
    sim_cfg.taker_fee_rate = 0.001;

    SimulatedExchange exchange(sim_cfg);
    ExchangeConfig ex_cfg;
    exchange.initialize(ex_cfg);
    exchange.connect();

    // Test 1: Can place limit order
    {
        OrderBook book;
        book.bids = {{100.0, 10.0}};
        book.asks = {{100.5, 10.0}};
        exchange.update_orderbook(book);

        auto order = exchange.place_limit_order("BTC/USDT", OrderSide::BUY, 99.5, 0.1, "order1");
        test_assert(
            "Exchange: Place limit order",
            order.has_value(),
            "Order placement failed"
        );
        test_assert(
            "Exchange: Resting order status",
            order->status == OrderStatus::NEW,
            "Status should be NEW"
        );
    }

    // Test 2: Immediate fill when price crosses
    {
        OrderBook book;
        book.bids = {{100.0, 10.0}};
        book.asks = {{100.5, 10.0}};
        exchange.update_orderbook(book);

        // Buy above ask (should fill immediately)
        auto order = exchange.place_limit_order("BTC/USDT", OrderSide::BUY, 101.0, 0.1, "order2");
        test_assert(
            "Exchange: Immediate fill on market cross",
            order->status == OrderStatus::FILLED,
            "Status should be FILLED"
        );
    }

    // Test 3: Market order fills immediately
    {
        OrderBook book;
        book.bids = {{100.0, 10.0}};
        book.asks = {{100.5, 10.0}};
        exchange.update_orderbook(book);

        auto order = exchange.place_market_order("BTC/USDT", OrderSide::BUY, 0.1, "order3");
        test_assert(
            "Exchange: Market order fills",
            order.has_value(),
            "Market order failed"
        );
        test_assert(
            "Exchange: Market order status",
            order->status == OrderStatus::FILLED,
            "Status not FILLED"
        );
    }

    // Test 4: IOC order fills immediately
    {
        OrderBook book;
        book.bids = {{100.0, 10.0}};
        book.asks = {{100.5, 10.0}};
        exchange.update_orderbook(book);

        auto order = exchange.place_ioc_order("BTC/USDT", OrderSide::SELL, 99.5, 0.1, "order4");
        test_assert(
            "Exchange: IOC order execution",
            order.has_value(),
            "IOC order failed"
        );
    }

    // Test 5: Cancel order
    {
        OrderBook book;
        book.bids = {{100.0, 10.0}};
        book.asks = {{100.5, 10.0}};
        exchange.update_orderbook(book);

        auto order = exchange.place_limit_order("BTC/USDT", OrderSide::BUY, 99.5, 0.1, "order5");
        auto result = exchange.cancel_order("BTC/USDT", order->order_id);
        test_assert(
            "Exchange: Cancel order",
            result.has_value(),
            "Cancel failed"
        );
    }
}

// ============================================================================
// Data Loader Tests
// ============================================================================

void test_data_loader() {
    std::cout << "\n[B5] Data Loader Tests" << std::endl;
    std::cout << std::string(70, '-') << std::endl;

    // Create test CSV file with depth=5 (5 bid levels + 5 ask levels)
    // Format: timestamp_ms, bid1_price, bid1_qty, bid2_price, bid2_qty, ..., ask1_price, ask1_qty, ...
    std::string test_file = "/tmp/test_orderbook.csv";
    std::ofstream csv(test_file);
    csv << "timestamp_ms,bid1p,bid1q,bid2p,bid2q,bid3p,bid3q,bid4p,bid4q,bid5p,bid5q,"
        << "ask1p,ask1q,ask2p,ask2q,ask3p,ask3q,ask4p,ask4q,ask5p,ask5q\n";
    csv << "1000,100.0,10.0,99.9,10.0,99.8,10.0,99.7,10.0,99.6,10.0,"
        << "100.5,10.0,100.6,10.0,100.7,10.0,100.8,10.0,100.9,10.0\n";
    csv << "2000,100.1,10.5,100.0,10.0,99.9,10.0,99.8,10.0,99.7,10.0,"
        << "100.6,9.5,100.7,9.5,100.8,9.5,100.9,9.5,101.0,9.5\n";
    csv << "3000,99.9,11.0,99.8,11.0,99.7,11.0,99.6,11.0,99.5,11.0,"
        << "100.4,10.5,100.5,10.5,100.6,10.5,100.7,10.5,100.8,10.5\n";
    csv.close();

    DataLoader loader(5);  // depth = 5 levels
    test_assert(
        "DataLoader: Open file",
        loader.open(test_file),
        "Failed to open test CSV"
    );

    // Test 2: Read first line
    {
        OrderBook book;
        double ts;
        bool success = loader.next(book, ts);
        test_assert(
            "DataLoader: Read line 1",
            success && !book.bids.empty() && !book.asks.empty(),
            "Failed to read first data line"
        );
        test_assert(
            "DataLoader: Timestamp 1",
            approx_equal(ts, 1000.0),
            "Timestamp = " + std::to_string(ts)
        );
    }

    // Test 3: Read second line
    {
        OrderBook book;
        double ts;
        bool success = loader.next(book, ts);
        test_assert(
            "DataLoader: Read line 2",
            success && !book.bids.empty() && !book.asks.empty(),
            "Failed to read second data line"
        );
        test_assert(
            "DataLoader: Timestamp 2",
            approx_equal(ts, 2000.0),
            "Timestamp = " + std::to_string(ts)
        );
    }

    // Test 4: EOF handling
    {
        OrderBook book;
        double ts;
        loader.next(book, ts);  // 3rd line
        bool success = loader.next(book, ts);  // Should fail (EOF)
        test_assert(
            "DataLoader: EOF detection",
            !success,
            "Should return false at EOF"
        );
    }

    // Test 5: Reset and re-read
    {
        loader.reset();
        OrderBook book;
        double ts;
        bool success = loader.next(book, ts);
        test_assert(
            "DataLoader: Reset functionality",
            success && !book.bids.empty() && !book.asks.empty() && approx_equal(ts, 1000.0),
            "Reset didn't work properly"
        );
    }
}

// ============================================================================
// Performance Metrics Tests
// ============================================================================

void test_performance_metrics() {
    std::cout << "\n[B5] Performance Metrics Tests" << std::endl;
    std::cout << std::string(70, '-') << std::endl;

    // Create sample trades
    std::vector<BacktestTrade> trades;
    trades.push_back({100.0, 1.0, true, 10.0, 0.01, 1000.0});   // Win
    trades.push_back({100.5, 1.0, false, -5.0, 0.01, 2000.0});  // Loss
    trades.push_back({101.0, 1.0, true, 15.0, 0.01, 3000.0});   // Win
    trades.push_back({100.8, 1.0, false, -2.0, 0.01, 4000.0});  // Loss

    std::vector<double> pnl_curve{0.0, 10.0, 5.0, 20.0, 18.0};

    auto metrics = PerformanceMetrics::compute(trades, pnl_curve);

    // Test 1: Total PnL calculation
    {
        double expected_pnl = 10.0 - 5.0 + 15.0 - 2.0;  // 18.0
        test_assert(
            "Metrics: Total PnL",
            approx_equal(metrics.total_pnl, expected_pnl),
            "Total PnL = " + std::to_string(metrics.total_pnl)
        );
    }

    // Test 2: Trade count
    {
        test_assert(
            "Metrics: Trade count",
            metrics.total_trades == 4,
            "Total trades = " + std::to_string(metrics.total_trades)
        );
    }

    // Test 3: Win/loss count
    {
        test_assert(
            "Metrics: Winning trades",
            metrics.winning_trades == 2,
            "Wins = " + std::to_string(metrics.winning_trades)
        );
        test_assert(
            "Metrics: Losing trades",
            metrics.losing_trades == 2,
            "Losses = " + std::to_string(metrics.losing_trades)
        );
    }

    // Test 4: Win rate
    {
        test_assert(
            "Metrics: Win rate",
            approx_equal(metrics.win_rate, 0.5),
            "Win rate = " + std::to_string(metrics.win_rate)
        );
    }

    // Test 5: Average win/loss
    {
        test_assert(
            "Metrics: Average win",
            approx_equal(metrics.avg_win, 12.5),
            "Avg win = " + std::to_string(metrics.avg_win)
        );
        test_assert(
            "Metrics: Average loss",
            approx_equal(metrics.avg_loss, -3.5),
            "Avg loss = " + std::to_string(metrics.avg_loss)
        );
    }

    // Test 6: Net PnL (after fees)
    {
        double expected_net = 18.0 - 4 * 0.01;  // PnL - fees
        test_assert(
            "Metrics: Net PnL",
            approx_equal(metrics.net_pnl, expected_net),
            "Net PnL = " + std::to_string(metrics.net_pnl)
        );
    }

    // Test 7: Max drawdown
    {
        // From pnl_curve: peak=10, valley=5, drawdown=-5
        test_assert(
            "Metrics: Max drawdown",
            metrics.max_drawdown <= 0.0,
            "Drawdown = " + std::to_string(metrics.max_drawdown)
        );
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "PHASE B TRADING SYSTEM TEST SUITE" << std::endl;
    std::cout << std::string(70, '=') << std::endl;

    // Run all tests
    test_as_model();
    test_obi_tracker();
    test_vwap_tracker();
    test_volatility_tracker();
    test_risk_manager_dynamic_sizing();
    test_simulated_exchange();
    test_data_loader();
    test_performance_metrics();

    // Print summary
    print_summary();

    // Return exit code based on test results
    int failed = 0;
    for (const auto& r : test_results) {
        if (!r.passed) failed++;
    }

    return failed > 0 ? 1 : 0;
}
