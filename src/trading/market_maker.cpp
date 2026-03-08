#include "trading/market_maker.h"
#include "exchange/exchange_factory.h"
#include "exchange/exchange_interface.h"
#include "core/app_logger.h"
#include <chrono>

namespace MarketMaker {

MarketMakerBot::MarketMakerBot(const Config& config) : config_(config) {
    logger_ = std::make_shared<Logger>(config.log_file);
    quill_logger_ = AppLogger::get("trading");
}

MarketMakerBot::~MarketMakerBot() {
    stop();
    if (main_thread_.joinable()) {
        main_thread_.join();
    }
}

bool MarketMakerBot::initialize() {
    logger_->log(LogLevel::INFO, "Initializing Market Maker Bot V2...");

    // Validate configuration
    if (!validate_config()) {
        logger_->log(LogLevel::ERROR,"Invalid configuration");
        return false;
    }

    // Setup exchange using factory pattern
    if (!setup_exchange()) {
        logger_->log(LogLevel::ERROR,"Failed to setup exchange");
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
    logger_->log(LogLevel::INFO, "Risk manager initialized (max_pos=" +
                 std::to_string(config_.max_position_size) + ", max_loss=" +
                 std::to_string(config_.max_daily_loss) + ")");

    // Initialize order manager with exchange interface and risk manager
    order_manager_ = std::make_shared<OrderManager>(exchange_, config_, risk_manager_);
    logger_->log(LogLevel::INFO, "Order manager initialized successfully");

    // Initialize volatility tracker
    volatility_tracker_ = std::make_shared<VolatilityTracker>(100, 0.001, 0.05);
    logger_->log(LogLevel::INFO, "Volatility tracker initialized");

    // Initialize User Data Stream for real fill tracking
    user_data_stream_ = std::make_unique<UserDataStream>(
        config_.api_key, config_.api_secret,
        config_.rest_base_url, config_.ws_base_url);

    // Wire fill callback to OrderManager
    user_data_stream_->set_fill_callback(
        [this](const std::string& order_id, const std::string& client_order_id,
               OrderSide side, OrderStatus status,
               double price, double qty, double cum_qty) {
            order_manager_->on_fill_event(order_id, client_order_id,
                                          side, status, price, qty, cum_qty);
        });

    if (!user_data_stream_->start()) {
        logger_->log(LogLevel::WARNING, "User Data Stream failed to start - "
                     "position tracking will be unavailable");
    } else {
        logger_->log(LogLevel::INFO, "User Data Stream connected");
    }

    initialized_ = true;
    logger_->log(LogLevel::INFO, "Market Maker Bot V2 initialized successfully");

    return true;
}

bool MarketMakerBot::setup_exchange() {
    logger_->log(LogLevel::INFO, "Setting up exchange: " + config_.exchange_type);

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

    logger_->log(LogLevel::INFO, "Exchange setup completed successfully");
    return true;
}

void MarketMakerBot::run() {
    if (!initialized_) {
        logger_->log(LogLevel::ERROR,"Bot not initialized. Call initialize() first.");
        return;
    }

    running_ = true;
    logger_->log(LogLevel::INFO, "Starting Market Maker Bot V2...");

    // Start main trading loop in separate thread
    main_thread_ = std::thread([this]() {
        main_loop();
    });

    logger_->log(LogLevel::INFO, "Market Maker Bot V2 is running on " + config_.exchange_type);
}

void MarketMakerBot::stop() {
    logger_->log(LogLevel::INFO, "Stopping Market Maker Bot V2...");
    running_ = false;

    // Stop User Data Stream first
    if (user_data_stream_) {
        user_data_stream_->stop();
    }

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

    // Get the orderbook received timestamp
    std::chrono::steady_clock::time_point orderbook_time;
    {
        std::lock_guard<std::mutex> lock(orderbook_mutex_);
        orderbook_time = last_orderbook_time_;
    }

    // Use OrderManager to handle all order logic with latency tracking
    order_manager_->update_orders_if_needed(mid_price, orderbook_time);
}

void MarketMakerBot::handle_orderbook_update(const OrderBook& orderbook) {
    // Capture timestamp immediately when orderbook update is received
    auto orderbook_received_time = std::chrono::steady_clock::now();

    // Update local orderbook
    {
        std::lock_guard<std::mutex> lock(orderbook_mutex_);
        current_orderbook_ = orderbook;
        last_orderbook_time_ = orderbook_received_time;
    }

    // Calculate and update mid price
    update_mid_price();
}

void MarketMakerBot::update_mid_price() {
    std::lock_guard<std::mutex> lock(orderbook_mutex_);

    if (!current_orderbook_.bids.empty() && !current_orderbook_.asks.empty()) {
        // Use VWAP mid price for better accuracy
        double new_mid_price = current_orderbook_.get_vwap_mid(5);
        if (new_mid_price <= 0) {
            new_mid_price = current_orderbook_.get_mid_price();
        }
        double old_mid_price = current_mid_price_.exchange(new_mid_price);

        // Feed volatility tracker
        if (volatility_tracker_) {
            volatility_tracker_->on_price(new_mid_price);
        }

        if (std::abs(old_mid_price - new_mid_price) > 0.00001) {
            LOG_INFO(quill_logger_, "[PRICE] ${:.5f} -> ${:.5f} (change: {:+.5f})",
                     old_mid_price, new_mid_price, new_mid_price - old_mid_price);

            // Signal price change for immediate reaction
            price_changed_.store(true);
            price_change_cv_.notify_one();

            if (config_.enable_verbose_logging) {
                LOG_DEBUG(quill_logger_, "Mid price updated: {:.5f} (Exchange: {})",
                          new_mid_price, config_.exchange_type);
            }
        }
    }
}

void MarketMakerBot::handle_connection_status(bool connected) {
    if (connected) {
        logger_->log(LogLevel::INFO, "Connected to " + config_.exchange_type + " exchange");
    } else {
        logger_->log(LogLevel::WARNING, "Disconnected from " + config_.exchange_type + " exchange");
    }
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
    auto [bid_order, ask_order] = order_manager_->get_active_orders();

    LOG_INFO(quill_logger_,
             "[STATUS] exchange={} symbol={} mid={:.2f} "
             "bid={:.2f} ask={:.2f} "
             "orders(total={} ok={} fail={} rate={:.1f}% opm={:.1f}) "
             "exec_lat(avg={:.3f} min={:.3f} max={:.3f}ms) "
             "react_lat(avg={:.3f} min={:.3f} max={:.3f}ms) "
             "reconnects={} uptime={:.2f}%",
             exchange_->get_exchange_name(), config_.symbol, current_mid_price_.load(),
             bid_order ? bid_order->price : 0.0,
             ask_order ? ask_order->price : 0.0,
             metrics.total_orders, metrics.successful_orders, metrics.failed_orders,
             metrics.get_success_rate(), metrics.get_orders_per_minute(),
             metrics.avg_order_latency_ms, metrics.min_order_latency_ms, metrics.max_order_latency_ms,
             metrics.avg_reaction_latency_ms, metrics.min_reaction_latency_ms, metrics.max_reaction_latency_ms,
             metrics.reconnect_count, metrics.get_uptime_percentage());

    if (risk_manager_) {
        LOG_INFO(quill_logger_,
                 "[RISK] position={:.6f} daily_pnl={:.4f} total_pnl={:.4f} "
                 "fees={:.4f} trades(win={} loss={} total={}) kill_switch={}",
                 risk_manager_->position_tracker().get_position(),
                 risk_manager_->pnl_tracker().get_daily_pnl(),
                 risk_manager_->pnl_tracker().get_realized_pnl(),
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

} // namespace MarketMaker