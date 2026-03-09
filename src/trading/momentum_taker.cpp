#include "trading/momentum_taker.h"
#include "exchange/exchange_factory.h"
#include "core/app_logger.h"
#include <chrono>

namespace MarketMaker {

MomentumTakerBot::MomentumTakerBot(const Config& config) : config_(config) {
    quill_logger_ = AppLogger::get("trading");
}

MomentumTakerBot::~MomentumTakerBot() {
    stop();
}

bool MomentumTakerBot::initialize() {
    LOG_INFO(quill_logger_, "{}", "Initializing Momentum Taker Bot...");

    if (!validate_config()) {
        LOG_ERROR(quill_logger_, "{}", "Invalid configuration");
        return false;
    }

    if (!setup_exchange()) {
        LOG_ERROR(quill_logger_, "{}", "Failed to setup exchange");
        return false;
    }

    // Initialize risk manager
    RiskConfig risk_config;
    risk_config.max_daily_loss = config_.max_daily_loss;
    risk_config.max_position_size = config_.max_position_size;
    risk_config.max_drawdown = config_.max_drawdown;
    risk_config.max_consecutive_errors = config_.max_consecutive_errors;
    risk_config.maker_fee_rate = config_.maker_fee_rate;
    risk_config.taker_fee_rate = config_.taker_fee_rate;
    risk_manager_ = std::make_shared<RiskManager>(risk_config);

    // Initialize order manager
    order_manager_ = std::make_shared<OrderManager>(exchange_, config_, risk_manager_);

    // Initialize signal engine
    signal_engine_ = std::make_unique<SignalEngine>(config_.momentum);

    // Initialize latency tracker
    latency_tracker_ = std::make_unique<LatencyTracker>();

    // Wire fill callback through exchange's WS trading connection
    exchange_->set_fill_callback(
        [this](const std::string& order_id, const std::string& client_order_id,
               OrderSide side, OrderStatus status,
               double price, double qty, double cum_qty) {
            order_manager_->on_fill_event(order_id, client_order_id,
                                          side, status, price, qty, cum_qty);
        });
    LOG_INFO(quill_logger_, "{}", "User data stream callbacks wired via WS trading connection");

    initialized_ = true;
    LOG_INFO(quill_logger_, "Momentum Taker Bot initialized: symbol={} epsilon={} ema_window={} order_size={}",
             config_.symbol, config_.momentum.epsilon, config_.momentum.ema_window,
             config_.momentum.order_size);
    return true;
}

bool MomentumTakerBot::setup_exchange() {
    LOG_INFO(quill_logger_, "Setting up exchange: {}", config_.exchange_type);

    config_.update_endpoints_for_exchange();

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

    exchange_ = ExchangeFactory::create(exchange_config);
    if (!exchange_) {
        LOG_ERROR(quill_logger_, "Failed to create exchange: {}", config_.exchange_type);
        return false;
    }

    exchange_->set_orderbook_handler([this](const OrderBook& orderbook) {
        handle_orderbook_update(orderbook);
    });

    exchange_->set_connection_handler([this](bool connected) {
        handle_connection_status(connected);
    });

    if (!exchange_->connect()) {
        LOG_ERROR(quill_logger_, "{}", "Failed to connect to exchange");
        return false;
    }

    // Subscribe with depth=5 (taker only needs top of book)
    std::string formatted_symbol = format_symbol_for_exchange();
    if (!exchange_->subscribe_orderbook(formatted_symbol, 5)) {
        LOG_ERROR(quill_logger_, "Failed to subscribe orderbook: {}", formatted_symbol);
        return false;
    }

    LOG_INFO(quill_logger_, "{}", "Exchange setup completed");
    return true;
}

void MomentumTakerBot::run() {
    if (!initialized_) {
        LOG_ERROR(quill_logger_, "{}", "Bot not initialized. Call initialize() first.");
        return;
    }

    running_ = true;
    LOG_INFO(quill_logger_, "{}", "Starting Momentum Taker Bot...");

    main_thread_ = std::thread([this]() {
        main_loop();
    });

    LOG_INFO(quill_logger_, "Momentum Taker Bot running on {}", config_.exchange_type);
}

void MomentumTakerBot::stop() {
    std::call_once(stop_flag_, [this]() {
        if (!running_) return;

        LOG_INFO(quill_logger_, "{}", "Stopping Momentum Taker Bot...");
        running_ = false;

        signal_cv_.notify_all();

        if (exchange_) {
            exchange_->disconnect();
        }

        if (main_thread_.joinable()) {
            main_thread_.join();
        }

        LOG_INFO(quill_logger_, "{}", "Momentum Taker Bot stopped");
    });
}

void MomentumTakerBot::main_loop() {
    auto last_status = std::chrono::steady_clock::now();

    while (running_) {
        {
            std::unique_lock<std::mutex> lock(signal_cv_mutex_);
            signal_cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                return signal_fired_.load() || !running_;
            });
        }

        if (!running_) break;

        if (signal_fired_.exchange(false)) {
            SignalState state;
            {
                std::lock_guard<std::mutex> lock(signal_mutex_);
                state = cached_signal_;
            }
            execute_signal(state);
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_status).count() >= 30) {
            print_status();
            last_status = now;
        }
    }
}

void MomentumTakerBot::handle_orderbook_update(const OrderBook& ob) {
    auto ob_time = std::chrono::steady_clock::now();
    double best_bid = ob.get_best_bid();
    double best_ask = ob.get_best_ask();

    if (best_bid <= 0 || best_ask <= 0) return;

    Signal sig = signal_engine_->on_tick(best_bid, best_ask);

    if (sig != Signal::NONE) {
        {
            std::lock_guard<std::mutex> lock(signal_mutex_);
            cached_signal_ = {sig, best_bid, best_ask, ob_time};
        }
        signal_fired_.store(true);
        signal_cv_.notify_one();
    }
}

void MomentumTakerBot::handle_connection_status(bool connected) {
    if (connected) {
        LOG_INFO(quill_logger_, "Connected to {} exchange", config_.exchange_type);
    } else {
        LOG_WARNING(quill_logger_, "Disconnected from {} exchange", config_.exchange_type);
    }
}

void MomentumTakerBot::execute_signal(const SignalState& state) {
    // Position check
    double current_pos = risk_manager_->position_tracker().get_position();
    double max_pos = config_.momentum.max_position;

    if (state.signal == Signal::BUY && current_pos >= max_pos) {
        LOG_DEBUG(quill_logger_, "[MOMENTUM] BUY suppressed: position {:.4f} >= max {:.4f}",
                  current_pos, max_pos);
        return;
    }
    if (state.signal == Signal::SELL && current_pos <= -max_pos) {
        LOG_DEBUG(quill_logger_, "[MOMENTUM] SELL suppressed: position {:.4f} <= -max {:.4f}",
                  current_pos, max_pos);
        return;
    }

    // Cost gate: reject signals where edge < spread + taker_fee + min_profit
    if (state.best_ask <= state.best_bid) return;  // crossed market guard
    double spread = state.best_ask - state.best_bid;
    double mid = (state.best_ask + state.best_bid) / 2.0;
    double ema = signal_engine_->ema_value();
    double signal_edge = std::abs(mid - ema);
    double cost = spread + (config_.taker_fee_rate * mid);
    double min_profit = config_.momentum.min_profit_bps * mid;

    if (signal_edge < cost + min_profit) {
        LOG_DEBUG(quill_logger_,
                  "[MOMENTUM] {} rejected: edge={:.4f} < cost={:.4f}+minprofit={:.4f}",
                  state.signal == Signal::BUY ? "BUY" : "SELL",
                  signal_edge, cost, min_profit);
        return;
    }

    OrderSide side = (state.signal == Signal::BUY) ? OrderSide::BUY : OrderSide::SELL;
    double price = (state.signal == Signal::BUY) ? state.best_ask : state.best_bid;

    auto start = std::chrono::steady_clock::now();
    orders_attempted_++;

    bool success = order_manager_->place_taker_order(
        side, price, config_.momentum.order_size,
        config_.momentum.order_type, state.orderbook_time);

    auto end = std::chrono::steady_clock::now();
    auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    latency_tracker_->record(latency_us);

    if (success) {
        orders_filled_++;
        signals_fired_++;
        LOG_INFO(quill_logger_,
                 "[MOMENTUM] {} at {:.2f} qty={:.6f} latency={}us ema={:.2f}",
                 state.signal == Signal::BUY ? "BUY" : "SELL",
                 price, config_.momentum.order_size, latency_us,
                 signal_engine_->ema_value());
    } else {
        LOG_WARNING(quill_logger_,
                    "[MOMENTUM] {} FAILED at {:.2f} latency={}us",
                    state.signal == Signal::BUY ? "BUY" : "SELL",
                    price, latency_us);
    }
}

void MomentumTakerBot::print_status() {
    LOG_INFO(quill_logger_,
             "[STATUS] symbol={} ema={:.2f} signals={} orders(attempt={} filled={}) "
             "lat_us(p50={:.0f} p90={:.0f} p99={:.0f})",
             config_.symbol, signal_engine_->ema_value(),
             signals_fired_.load(), orders_attempted_.load(), orders_filled_.load(),
             latency_tracker_->percentile(0.50),
             latency_tracker_->percentile(0.90),
             latency_tracker_->percentile(0.99));

    if (risk_manager_) {
        LOG_INFO(quill_logger_,
                 "[RISK] position={:.6f} daily_pnl={:.4f} total_pnl={:.4f} "
                 "fees={:.4f} kill_switch={}",
                 risk_manager_->position_tracker().get_position(),
                 risk_manager_->pnl_tracker().get_daily_pnl(),
                 risk_manager_->pnl_tracker().get_realized_pnl(),
                 risk_manager_->pnl_tracker().get_total_fees(),
                 risk_manager_->is_kill_switch_active() ? "ACTIVE" : "off");
    }
}

bool MomentumTakerBot::validate_config() {
    if (!ExchangeFactory::is_supported(config_.exchange_type)) {
        LOG_ERROR(quill_logger_, "Unsupported exchange: {}", config_.exchange_type);
        return false;
    }

    if (config_.api_key.empty() || config_.api_secret.empty()) {
        LOG_ERROR(quill_logger_, "{}", "API credentials not set");
        return false;
    }

    if (config_.momentum.order_size <= 0) {
        LOG_ERROR(quill_logger_, "Invalid momentum order_size: {}", config_.momentum.order_size);
        return false;
    }

    if (config_.momentum.epsilon <= 0) {
        LOG_ERROR(quill_logger_, "Invalid momentum epsilon: {}", config_.momentum.epsilon);
        return false;
    }

    if (config_.momentum.min_profit_bps < 0) {
        LOG_ERROR(quill_logger_, "Invalid momentum min_profit_bps: {} (must be >= 0)", config_.momentum.min_profit_bps);
        return false;
    }

    if (config_.momentum.ema_window <= 0) {
        LOG_ERROR(quill_logger_, "Invalid momentum ema_window: {}", config_.momentum.ema_window);
        return false;
    }

    if (config_.momentum.order_type != "ioc" && config_.momentum.order_type != "market") {
        LOG_ERROR(quill_logger_, "Invalid momentum order_type: {} (must be 'ioc' or 'market')",
                  config_.momentum.order_type);
        return false;
    }

    return true;
}

std::string MomentumTakerBot::format_symbol_for_exchange() {
    return config_.symbol;
}

LatencyMetrics MomentumTakerBot::get_metrics() const {
    if (order_manager_) {
        return order_manager_->get_metrics();
    }
    return LatencyMetrics();
}

} // namespace MarketMaker
