#include "trading/market_maker.h"
#include "exchange/exchange_factory.h"
#include "exchange/exchange_interface.h"
#include "core/app_logger.h"
#include "core/metrics_collector.h"
#include <json/json.h>
#include <chrono>
#include <iostream>
#include <thread>

namespace MarketMaker {

MarketMakerBot::MarketMakerBot(const Config& config) : config_(config) {
    logger_ = std::make_shared<Logger>(config.log_file);
    quill_logger_ = AppLogger::get("trading");
    // Init hot-path atomics from config
    atomic_spread_pct_.store(config.spread_percentage, std::memory_order_relaxed);
    atomic_obi_tilt_factor_.store(config.obi_tilt_factor, std::memory_order_relaxed);
}

MarketMakerBot::~MarketMakerBot() {
    stop();
    if (main_thread_.joinable()) {
        main_thread_.join();
    }
}

bool MarketMakerBot::initialize() {
    // Validate configuration
    if (!validate_config()) {
        logger_->log(LogLevel::ERROR,"Invalid configuration");
        return false;
    }
    LOG_INFO(quill_logger_, "{}", "[2/8] Validating config... OK");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::cout << "\n▸ PHASE 2: Exchange Setup" << std::endl;

    // Setup exchange using factory pattern
    if (!setup_exchange()) {
        logger_->log(LogLevel::ERROR,"Failed to setup exchange");
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::cout << "\n▸ PHASE 3: Risk & Order Management" << std::endl;

    // Initialize risk manager
    RiskConfig risk_config;
    risk_config.max_daily_loss = config_.max_daily_loss;
    risk_config.max_position_size = config_.max_position_size;
    risk_config.max_drawdown = config_.max_drawdown;
    risk_config.max_consecutive_errors = config_.max_consecutive_errors;
    risk_config.maker_fee_rate = config_.maker_fee_rate;
    risk_config.taker_fee_rate = config_.taker_fee_rate;
    risk_config.max_drawdown_spread_multiplier = config_.max_drawdown_spread_multiplier;
    risk_manager_ = std::make_shared<RiskManager>(risk_config);

    // Per-side position limits (0 = use symmetric max_position_size)
    if (config_.max_long_position > 0 || config_.max_short_position > 0) {
        risk_manager_->position_tracker().set_asymmetric_limits(
            config_.max_long_position > 0 ? config_.max_long_position : config_.max_position_size,
            config_.max_short_position > 0 ? config_.max_short_position : config_.max_position_size);
    }
    LOG_INFO(quill_logger_, "[4/8] Risk manager: max_pos={:.2f} max_loss=${:.0f} max_drawdown=${:.0f} OK",
             config_.max_position_size, config_.max_daily_loss, config_.max_drawdown);

    // Initialize order manager with exchange interface and risk manager
    order_manager_ = std::make_shared<OrderManager>(exchange_, config_, risk_manager_);
    LOG_INFO(quill_logger_, "{}", "[5/8] Order manager: ObjectPool<Order>(128) pre-allocated OK");

    // Initialize volatility tracker (with fast window for regime detection)
    volatility_tracker_ = std::make_shared<VolatilityTracker>(
        100, 0.001, 0.05,
        config_.vol_fast_window, config_.vol_regime_threshold, config_.vol_regime_spread_mult);
    LOG_INFO(quill_logger_, "[6/8] Volatility tracker: window=100 fast={} range=[0.1%, 5.0%] OK",
             config_.vol_fast_window);

    // Wire volatility tracker into risk manager for dynamic sizing
    if (config_.use_dynamic_sizing && risk_manager_) {
        risk_manager_->set_volatility_tracker(volatility_tracker_);
        logger_->log(LogLevel::INFO, "Dynamic volatility-adjusted sizing enabled");
    }

    // Initialize Avellaneda-Stoikov model if enabled
    if (config_.use_avellaneda_stoikov) {
        as_model_ = std::make_unique<AvellanedaStoikovModel>(
            config_.as_gamma, config_.as_kappa, config_.as_time_horizon_sec);
        as_horizon_start_ = std::chrono::steady_clock::now();
        logger_->log(LogLevel::INFO, "Avellaneda-Stoikov model enabled (gamma=" +
                     std::to_string(config_.as_gamma) + " kappa=" +
                     std::to_string(config_.as_kappa) + " horizon=" +
                     std::to_string(config_.as_time_horizon_sec) + "s)");
    }

    // Initialize OBI tracker if enabled
    if (config_.use_obi_tilt) {
        obi_tracker_ = std::make_unique<OrderBookImbalanceTracker>(
            config_.obi_levels, 0.3, config_.obi_min_volume);
        logger_->log(LogLevel::INFO, "OBI tilt enabled (levels=" +
                     std::to_string(config_.obi_levels) + " tilt_factor=" +
                     std::to_string(config_.obi_tilt_factor) + ")");
    }

    // Initialize toxic flow tracker if enabled
    if (config_.use_toxic_flow_detection) {
        trade_flow_tracker_ = std::make_unique<TradeFlowTracker>(
            config_.toxic_flow_window, config_.toxic_flow_threshold,
            config_.toxic_flow_spread_mult);
        logger_->log(LogLevel::INFO, "Toxic flow detection enabled (window=" +
                     std::to_string(config_.toxic_flow_window) + " threshold=" +
                     std::to_string(config_.toxic_flow_threshold) + ")");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::cout << "\n▸ PHASE 4: Event Wiring & Services" << std::endl;

    // Wire fill callback through exchange's WS trading connection
    if (exchange_->supports_websocket_trading()) {
        exchange_->set_fill_callback(
            [this](const std::string& order_id, const std::string& client_order_id,
                   OrderSide side, OrderStatus status,
                   double price, double qty, double cum_qty) {
                order_manager_->on_fill_event(order_id, client_order_id,
                                              side, status, price, qty, cum_qty);
                // Feed toxic flow tracker
                if (trade_flow_tracker_ && qty > 0) {
                    trade_flow_tracker_->on_fill(side == OrderSide::BUY);
                }
                // Publish fill event to GCP Pub/Sub
                Json::Value d;
                d["order_id"] = order_id;
                d["side"] = (side == OrderSide::BUY) ? "BUY" : "SELL";
                d["status"] = static_cast<int>(status);
                d["price"] = price;
                d["qty"] = qty;
                d["cum_qty"] = cum_qty;
                Json::FastWriter w;
                publish_event("fill", w.write(d));
            });
        LOG_INFO(quill_logger_, "{}", "[7/8] Fill callback wired -> position + PnL tracking enabled OK");
    } else {
        logger_->log(LogLevel::WARNING, "Fill tracking unavailable: set use_websocket_trading=true to enable");
    }

    initialized_ = true;
    LOG_INFO(quill_logger_, "{}", "Market Maker Bot initialized OK");

    return true;
}

bool MarketMakerBot::setup_exchange() {
    LOG_INFO(quill_logger_, "[3/8] Creating exchange via Factory ({} + {})",
             config_.exchange_type, config_.use_websocket_trading ? "websocket" : "REST");

    // Update config endpoints based on exchange type
    config_.update_endpoints_for_exchange();

    // Create exchange configuration
    ExchangeConfig exchange_config;
    exchange_config.exchange_type = config_.exchange_type;
    exchange_config.api_url = config_.rest_base_url;
    exchange_config.ws_url = config_.ws_base_url;
    exchange_config.ws_trading_url = config_.ws_trading_url;
    exchange_config.use_websocket_trading = config_.use_websocket_trading;
    exchange_config.api_key = config_.api_key;
    exchange_config.api_secret = config_.api_secret;
    exchange_config.use_testnet = config_.use_testnet;
    exchange_config.price_precision = config_.price_precision;
    exchange_config.quantity_precision = config_.quantity_precision;
    exchange_config.max_requests_per_second = config_.max_requests_per_second;
    exchange_config.max_orders_per_second = config_.max_orders_per_second;
    exchange_config.display_assets = config_.display_assets;
    exchange_config.supported_quote_currencies = config_.supported_quote_currencies;

    // Create exchange instance using factory
    exchange_ = ExchangeFactory::create(exchange_config);

    if (!exchange_) {
        logger_->log(LogLevel::ERROR,"Failed to create exchange instance for: " + config_.exchange_type);
        return false;
    }

    // Set up event handlers
    exchange_->set_orderbook_handler([this](const OrderBook& orderbook) {
        handle_orderbook_update(orderbook);
    });

    exchange_->set_connection_handler([this](bool connected) {
        handle_connection_status(connected);
    });

    // Connect to exchange
    if (!exchange_->connect()) {
        logger_->log(LogLevel::ERROR,"Failed to connect to exchange");
        return false;
    }

    // Subscribe to market data
    std::string formatted_symbol = format_symbol_for_exchange();
    if (!exchange_->subscribe_orderbook(formatted_symbol, 20)) {
        logger_->log(LogLevel::ERROR,"Failed to subscribe to orderbook for: " + formatted_symbol);
        return false;
    }

    LOG_INFO(quill_logger_, "{}", "[3/8] Callbacks wired: orderbook, connection OK");
    return true;
}

void MarketMakerBot::run() {
    if (!initialized_) {
        logger_->log(LogLevel::ERROR,"Bot not initialized. Call initialize() first.");
        return;
    }

    // Prevent double-start: if running_ was already true, return early
    if (running_.exchange(true)) {
        logger_->log(LogLevel::WARNING, "run() called while already running - ignoring");
        return;
    }
    // Start main trading loop in separate thread
    main_thread_ = std::thread([this]() {
        main_loop();
    });
}

void MarketMakerBot::stop() {
    if (!running_.exchange(false)) return;  // Already stopped, avoid double-join
    logger_->log(LogLevel::INFO, "Stopping Market Maker Bot V2...");

    // Notify condition variable to wake up main loop
    price_change_cv_.notify_all();

    // Disconnect from exchange
    if (exchange_) {
        exchange_->disconnect();
    }

    // Wait for main thread to finish
    if (main_thread_.joinable()) {
        main_thread_.join();
    }

    // Print session P&L summary
    if (risk_manager_) {
        double pos = risk_manager_->position_tracker().get_position();
        double avg_entry = risk_manager_->position_tracker().get_average_entry_price();
        double mid = current_mid_price_.load();
        risk_manager_->pnl_tracker().print_session_summary(mid, pos, avg_entry);
    }

    logger_->log(LogLevel::INFO, "Market Maker Bot V2 stopped");
}

void MarketMakerBot::main_loop() {
    auto last_status_print = std::chrono::steady_clock::now();

    while (running_) {
        // Wait for price change notification or timeout
        {
            std::unique_lock<std::mutex> lock(price_change_mutex_);
            price_change_cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                return price_changed_.load() || !running_;
            });
        }

        if (!running_) break;

        // Get current mid price
        double mid_price = current_mid_price_.load();

        if (mid_price > 0 && price_changed_.exchange(false)) {
            // Check and update orders using exchange interface
            check_and_update_orders();
        }

        // Print status every 30 seconds
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_status_print).count() >= 30) {
            print_status();
            last_status_print = now;
        }
    }
}

void MarketMakerBot::check_and_update_orders() {
    double mid_price = current_mid_price_.load();

    if (mid_price <= 0) {
        return;
    }

    // Read hot-path config via atomics (no mutex on tick path)
    double spread_pct = atomic_spread_pct_.load(std::memory_order_relaxed);
    double obi_tilt_factor = atomic_obi_tilt_factor_.load(std::memory_order_relaxed);

    // Stack all spread multipliers (cap at 5x to prevent runaway widening)
    double drawdown_mult = (risk_manager_ && config_.max_drawdown_spread_multiplier > 0.0)
        ? risk_manager_->get_drawdown_spread_multiplier(mid_price) : 1.0;
    double tod_mult = get_time_of_day_multiplier();
    double toxic_mult = trade_flow_tracker_ ? trade_flow_tracker_->get_spread_multiplier() : 1.0;
    double regime_mult = volatility_tracker_ ? volatility_tracker_->get_regime_spread_multiplier() : 1.0;

    double total_mult = std::min(drawdown_mult * tod_mult * toxic_mult * regime_mult, 5.0);
    spread_pct *= total_mult;

    // Drain ring buffer, use the latest orderbook timestamp for latency tracking
    auto latest = orderbook_ring_.drain_latest();
    auto orderbook_time = latest ? latest->received_time : std::chrono::steady_clock::now();

    // Update OBI tracker if enabled and we have orderbook data
    double obi_tilt = 0.0;
    if (obi_tracker_ && latest) {
        obi_tracker_->update(latest->book);
        if (obi_tracker_->is_significant()) {
            obi_tilt = obi_tracker_->smoothed_obi() * obi_tilt_factor;
            LOG_INFO(quill_logger_, "[OBI] raw={:.3f} smoothed={:.3f} tilt={:.4f}",
                     obi_tracker_->raw_obi(), obi_tracker_->smoothed_obi(), obi_tilt);
        }
    }

    if (as_model_) {
        // Avellaneda-Stoikov: compute inventory-aware bid/ask prices
        double inventory = risk_manager_ ? risk_manager_->position_tracker().get_position() : 0.0;
        double volatility = volatility_tracker_ ? volatility_tracker_->get_volatility() : 0.0;

        // Rolling time horizon: seconds remaining until next reset
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - as_horizon_start_).count();
        double time_remaining = config_.as_time_horizon_sec - std::fmod(elapsed, config_.as_time_horizon_sec);

        if (elapsed >= config_.as_time_horizon_sec) {
            as_horizon_start_ = now;
        }

        auto quote = config_.use_glft
            ? as_model_->compute_glft(mid_price, inventory, volatility, time_remaining, config_.max_position_size)
            : as_model_->compute(mid_price, inventory, volatility, time_remaining);

        // Apply OBI tilt: positive OBI (buy pressure) -> tighten bid, widen ask
        double bid_price = quote.bid_price * (1.0 + obi_tilt);
        double ask_price = quote.ask_price * (1.0 - obi_tilt);

        // Guard against crossed orders from aggressive tilt
        if (bid_price >= ask_price) {
            double center = (bid_price + ask_price) / 2.0;
            bid_price = center - center * 1e-6;
            ask_price = center + center * 1e-6;
        }

        LOG_INFO(quill_logger_,
                 "[AS] mid={:.2f} inv={:.6f} vol={:.6f} tau={:.1f}s "
                 "rprice={:.2f} spread={:.4f} bid={:.2f} ask={:.2f}",
                 mid_price, inventory, volatility, time_remaining,
                 quote.reservation_price, quote.optimal_spread, bid_price, ask_price);

        [[maybe_unused]] bool ok = order_manager_->place_market_maker_orders_with_prices(
            mid_price, bid_price, ask_price, orderbook_time);
    } else {
        // Non-AS mode: fixed spread with optional OBI tilt and inventory skew
        double spread = spread_pct;

        // Inventory skew: positive inventory -> skew quotes to sell more aggressively
        double skew = 0.0;
        double inventory = risk_manager_ ? risk_manager_->position_tracker().get_position() : 0.0;
        if (config_.inventory_skew_factor > 0.0 && std::abs(inventory) > 1e-9) {
            skew = -inventory * config_.inventory_skew_factor;
        }

        double bid_offset = spread * (1.0 - obi_tilt) - skew;
        double ask_offset = spread * (1.0 + obi_tilt) + skew;
        double bid_price = mid_price * (1.0 - bid_offset);
        double ask_price = mid_price * (1.0 + ask_offset);

        // Guard against crossed orders from aggressive tilt/skew
        if (bid_price >= ask_price) {
            bid_price = mid_price * (1.0 - spread);
            ask_price = mid_price * (1.0 + spread);
        }

        [[maybe_unused]] bool ok = order_manager_->place_market_maker_orders_with_prices(
            mid_price, bid_price, ask_price, orderbook_time);
    }
}

void MarketMakerBot::handle_orderbook_update(const OrderBook& orderbook) {
    // Lock-free push into ring buffer from WS callback thread
    TimestampedOrderBook snapshot{orderbook, std::chrono::steady_clock::now()};
    if (!orderbook_ring_.try_push(std::move(snapshot))) {
        LOG_WARNING(quill_logger_, "{}", "Orderbook ring buffer full - dropping update");
    }

    // Compute mid price and signal strategy thread
    if (!orderbook.bids.empty() && !orderbook.asks.empty()) {
        double new_mid_price = orderbook.get_vwap_mid(5);
        if (new_mid_price <= 0) {
            new_mid_price = orderbook.get_mid_price();
        }
        double old_mid_price = current_mid_price_.exchange(new_mid_price);

        // Feed volatility tracker (lightweight, ok in callback)
        if (volatility_tracker_) {
            volatility_tracker_->on_price(new_mid_price);
        }

        uint64_t tick = ++tick_count_;
        double change = new_mid_price - old_mid_price;
        if (std::abs(change) > 0.00001) {
            // Only log first tick, or when price moves >= $1 (reduce noise)
            if (old_mid_price <= 0.00001) {
                LOG_INFO(quill_logger_, "[TICK #{}] mid=${:.2f} (first tick)", tick, new_mid_price);
            } else if (std::abs(change) >= 1.0 || tick % 100 == 0) {
                LOG_INFO(quill_logger_, "[TICK #{}] mid=${:.2f} ({:+.2f})", tick, new_mid_price, change);
            }

            // Signal price change for immediate reaction
            price_changed_.store(true);
            price_change_cv_.notify_one();
        }
    }
}

void MarketMakerBot::handle_connection_status(bool connected) {
    if (connected) {
        // Only log once when fully connected (both WS), not per-WS
        if (!initialized_) return;
        LOG_INFO(quill_logger_, "Reconnected to {} exchange", config_.exchange_type);
        publish_event("connection", R"({"status":"connected"})");
    } else {
        if (initialized_) {
            LOG_WARNING(quill_logger_, "Disconnected from {} exchange", config_.exchange_type);
            publish_event("connection", R"({"status":"disconnected"})");
        }
    }
}

double MarketMakerBot::get_time_of_day_multiplier() const {
    if (config_.time_of_day_rules.empty()) return 1.0;
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm;
    gmtime_r(&time_t, &utc_tm);
    int hour = utc_tm.tm_hour;
    for (const auto& rule : config_.time_of_day_rules) {
        if (rule.start_hour_utc <= rule.end_hour_utc) {
            // Normal range (e.g., 2-6)
            if (hour >= rule.start_hour_utc && hour < rule.end_hour_utc)
                return rule.spread_multiplier;
        } else {
            // Wraps midnight (e.g., 22-4)
            if (hour >= rule.start_hour_utc || hour < rule.end_hour_utc)
                return rule.spread_multiplier;
        }
    }
    return 1.0;
}

bool MarketMakerBot::validate_config() {
    // Validate exchange type
    if (!ExchangeFactory::is_supported(config_.exchange_type)) {
        logger_->log(LogLevel::ERROR,"Unsupported exchange type: " + config_.exchange_type);

        auto supported = ExchangeFactory::get_supported_exchanges();
        std::string supported_str = "Supported exchanges: ";
        for (const auto& ex : supported) {
            supported_str += ex + " ";
        }
        logger_->log(LogLevel::INFO, supported_str);

        return false;
    }

    // Validate API credentials
    if (config_.api_key.empty() || config_.api_secret.empty()) {
        logger_->log(LogLevel::ERROR,"API credentials not set");
        return false;
    }

    // Validate trading parameters
    if (config_.spread_percentage <= 0) {
        logger_->log(LogLevel::ERROR,"Invalid spread percentage: " + std::to_string(config_.spread_percentage));
        return false;
    }

    if (config_.order_size <= 0) {
        logger_->log(LogLevel::ERROR,"Invalid order size: " + std::to_string(config_.order_size));
        return false;
    }

    return true;
}

void MarketMakerBot::print_status() {
    auto metrics = order_manager_->get_metrics();

    double pos = risk_manager_ ? risk_manager_->position_tracker().get_position() : 0.0;
    double daily_pnl = risk_manager_ ? risk_manager_->pnl_tracker().get_daily_pnl() : 0.0;
    LOG_INFO(quill_logger_,
             "[STATUS 30s] pos={:.6f} pnl=${:.2f} orders={} fills={} latency={:.1f}ms",
             pos, daily_pnl,
             metrics.total_orders, metrics.successful_orders,
             metrics.avg_order_latency_ms);

    // Latency percentiles
    auto exec_pct = order_manager_->get_exec_percentiles();
    auto react_pct = order_manager_->get_reaction_percentiles();
    if (exec_pct.sample_count > 0) {
        LOG_INFO(quill_logger_,
                 "[LATENCY] exec: p50={:.2f}ms p95={:.2f}ms p99={:.2f}ms | "
                 "react: p50={:.2f}ms p95={:.2f}ms p99={:.2f}ms (n={})",
                 exec_pct.p50_ms, exec_pct.p95_ms, exec_pct.p99_ms,
                 react_pct.p50_ms, react_pct.p95_ms, react_pct.p99_ms,
                 exec_pct.sample_count);
    }

    // Publish status snapshot to GCP Pub/Sub
    {
        Json::Value d;
        d["mid_price"] = current_mid_price_.load();
        d["total_orders"] = static_cast<Json::UInt64>(metrics.total_orders);
        d["success_rate"] = metrics.get_success_rate();
        d["avg_latency_ms"] = metrics.avg_order_latency_ms;
        if (risk_manager_) {
            double pos = risk_manager_->position_tracker().get_position();
            double avg_entry = risk_manager_->position_tracker().get_average_entry_price();
            double mid = current_mid_price_.load();
            d["position"] = pos;
            d["daily_pnl"] = risk_manager_->pnl_tracker().get_daily_pnl();
            d["realized_pnl"] = risk_manager_->pnl_tracker().get_realized_pnl();
            d["unrealized_pnl"] = risk_manager_->pnl_tracker().get_unrealized_pnl(mid, pos, avg_entry);
            d["total_pnl"] = risk_manager_->pnl_tracker().get_total_pnl(mid, pos, avg_entry);
            d["kill_switch"] = risk_manager_->is_kill_switch_active();
        }
        if (exec_pct.sample_count > 0) {
            d["latency_exec_p50_ms"] = exec_pct.p50_ms;
            d["latency_exec_p95_ms"] = exec_pct.p95_ms;
            d["latency_exec_p99_ms"] = exec_pct.p99_ms;
            d["latency_react_p50_ms"] = react_pct.p50_ms;
            d["latency_react_p95_ms"] = react_pct.p95_ms;
            d["latency_react_p99_ms"] = react_pct.p99_ms;
        }
        Json::FastWriter w;
        publish_event("status", w.write(d));
    }

    // Update Prometheus gauges
    auto& mc = MetricsCollector::instance();
    mc.set_gauge("mid_price", current_mid_price_.load());
    mc.set_gauge("spread_bps", config_.spread_percentage * 10000);
    mc.set_gauge("bot_running", running_ ? 1.0 : 0.0);

    // Latency percentile gauges
    if (exec_pct.sample_count > 0) {
        mc.set_gauge("latency_exec_p50_ms", exec_pct.p50_ms);
        mc.set_gauge("latency_exec_p95_ms", exec_pct.p95_ms);
        mc.set_gauge("latency_exec_p99_ms", exec_pct.p99_ms);
        mc.set_gauge("latency_react_p50_ms", react_pct.p50_ms);
        mc.set_gauge("latency_react_p95_ms", react_pct.p95_ms);
        mc.set_gauge("latency_react_p99_ms", react_pct.p99_ms);
    }

    if (risk_manager_) {
        double pos = risk_manager_->position_tracker().get_position();
        double avg_entry = risk_manager_->position_tracker().get_average_entry_price();
        double mid = current_mid_price_.load();
        double unrealized = risk_manager_->pnl_tracker().get_unrealized_pnl(mid, pos, avg_entry);
        double total_pnl = risk_manager_->pnl_tracker().get_total_pnl(mid, pos, avg_entry);

        mc.set_gauge("position_current", pos);
        mc.set_gauge("pnl_daily_usd", risk_manager_->pnl_tracker().get_daily_pnl());
        mc.set_gauge("pnl_realized_usd", risk_manager_->pnl_tracker().get_realized_pnl());
        mc.set_gauge("pnl_unrealized_usd", unrealized);
        mc.set_gauge("pnl_total_usd", total_pnl);
        mc.set_gauge("kill_switch_active", risk_manager_->is_kill_switch_active() ? 1.0 : 0.0);

        LOG_INFO(quill_logger_,
                 "[RISK] position={:.6f} avg_entry={:.2f} realized={:.4f} unrealized={:.4f} "
                 "total_pnl={:.4f} daily_pnl={:.4f} fees={:.4f} "
                 "trades(win={} loss={} total={}) kill_switch={}",
                 pos, avg_entry,
                 risk_manager_->pnl_tracker().get_realized_pnl(),
                 unrealized, total_pnl,
                 risk_manager_->pnl_tracker().get_daily_pnl(),
                 risk_manager_->pnl_tracker().get_total_fees(),
                 risk_manager_->pnl_tracker().get_winning_trades(),
                 risk_manager_->pnl_tracker().get_losing_trades(),
                 risk_manager_->pnl_tracker().get_total_trades(),
                 risk_manager_->is_kill_switch_active() ? "ACTIVE" : "off");
    }
}

std::string MarketMakerBot::format_symbol_for_exchange() {
    // Different exchanges use different symbol formats
    // This is a simplified version - in production, each exchange
    // would handle its own symbol formatting

    if (config_.exchange_type == "binance") {
        // Binance uses the symbol directly from config (e.g., SEIUSDT, BTCUSDT)
        return config_.symbol;
    }else if (config_.exchange_type == "kraken") {
        // Kraken uses different format - would need proper parsing
        // For now, just use the symbol from config
        return config_.symbol;
    }

    // Default format
    return config_.symbol;
}

LatencyMetrics MarketMakerBot::get_metrics() const {
    if (order_manager_) {
        return order_manager_->get_metrics();
    }
    return LatencyMetrics();
}

double MarketMakerBot::get_position() const {
    return risk_manager_ ? risk_manager_->position_tracker().get_position() : 0.0;
}

double MarketMakerBot::get_daily_pnl() const {
    return risk_manager_ ? risk_manager_->pnl_tracker().get_daily_pnl() : 0.0;
}

double MarketMakerBot::get_total_pnl() const {
    return risk_manager_ ? risk_manager_->pnl_tracker().get_realized_pnl() : 0.0;
}

double MarketMakerBot::get_fees_paid() const {
    return risk_manager_ ? risk_manager_->pnl_tracker().get_total_fees() : 0.0;
}

bool MarketMakerBot::is_kill_switch_active() const {
    return risk_manager_ ? risk_manager_->is_kill_switch_active() : false;
}

std::pair<std::shared_ptr<Order>, std::shared_ptr<Order>> MarketMakerBot::get_active_orders() const {
    return order_manager_ ? order_manager_->get_active_orders()
                          : std::pair<std::shared_ptr<Order>, std::shared_ptr<Order>>{nullptr, nullptr};
}

void MarketMakerBot::activate_kill_switch(const std::string& reason) {
    if (risk_manager_) risk_manager_->activate_kill_switch(reason);
}

void MarketMakerBot::publish_event(const std::string& event_type, const std::string& payload) {
    if (!publisher_) return;

    Json::Value root;
    root["type"] = event_type;
    root["symbol"] = config_.symbol;
    root["timestamp_ms"] = static_cast<Json::Int64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Parse payload into the event
    Json::Value data;
    Json::Reader reader;
    if (reader.parse(payload, data)) {
        root["data"] = data;
    } else {
        root["data"] = payload;
    }

    Json::FastWriter writer;
    std::string json = writer.write(root);
    if (!json.empty() && json.back() == '\n') json.pop_back();
    publisher_->publish(json);
}

} // namespace MarketMaker