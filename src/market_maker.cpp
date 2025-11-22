#include "market_maker_v2.h"
#include "exchange_factory.h"
#include "exchange_interface.h"
#include <iostream>
#include <iomanip>
#include <chrono>

namespace MarketMaker {

MarketMakerBotV2::MarketMakerBotV2(const Config& config) : config_(config) {
    logger_ = std::make_shared<Logger>(config.log_file);
}

MarketMakerBotV2::~MarketMakerBotV2() {
    stop();
    if (main_thread_.joinable()) {
        main_thread_.join();
    }
}

bool MarketMakerBotV2::initialize() {
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

    // Initialize order manager with exchange interface
    order_manager_ = std::make_shared<OrderManager>(exchange_, config_);
    logger_->log(LogLevel::INFO, "Order manager initialized successfully");

    initialized_ = true;
    logger_->log(LogLevel::INFO, "Market Maker Bot V2 initialized successfully");

    return true;
}

bool MarketMakerBotV2::setup_exchange() {
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

void MarketMakerBotV2::run() {
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

void MarketMakerBotV2::stop() {
    logger_->log(LogLevel::INFO, "Stopping Market Maker Bot V2...");
    running_ = false;

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

void MarketMakerBotV2::main_loop() {
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

void MarketMakerBotV2::check_and_update_orders() {
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

void MarketMakerBotV2::handle_orderbook_update(const OrderBook& orderbook) {
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

void MarketMakerBotV2::update_mid_price() {
    std::lock_guard<std::mutex> lock(orderbook_mutex_);

    if (!current_orderbook_.bids.empty() && !current_orderbook_.asks.empty()) {
        double best_bid = current_orderbook_.bids[0].price;
        double best_ask = current_orderbook_.asks[0].price;
        double new_mid_price = (best_bid + best_ask) / 2.0;
        double old_mid_price = current_mid_price_.exchange(new_mid_price);

        if (std::abs(old_mid_price - new_mid_price) > 0.00001) {
            std::cout << "[PRICE UPDATE] Mid price: $" << std::fixed << std::setprecision(5)
                      << old_mid_price << " -> $" << std::setprecision(5) << new_mid_price
                      << " (Change: " << std::showpos << std::setprecision(5) << (new_mid_price - old_mid_price)
                      << std::noshowpos << ")" << std::endl;

            // Signal price change for immediate reaction
            price_changed_.store(true);
            price_change_cv_.notify_one();

            if (config_.enable_verbose_logging) {
                logger_->log(LogLevel::INFO, "Mid price updated: " + std::to_string(new_mid_price) +
                            " (Exchange: " + config_.exchange_type + ")");
            }
        }
    }
}

void MarketMakerBotV2::handle_connection_status(bool connected) {
    if (connected) {
        logger_->log(LogLevel::INFO, "Connected to " + config_.exchange_type + " exchange");
    } else {
        logger_->log(LogLevel::WARNING, "Disconnected from " + config_.exchange_type + " exchange");
    }
}

bool MarketMakerBotV2::validate_config() {
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

void MarketMakerBotV2::print_status() {
    auto metrics = order_manager_->get_metrics();

    std::cout << "\n========== Market Maker Status ==========" << std::endl;
    std::cout << "Exchange: " << exchange_->get_exchange_name() << std::endl;
    std::cout << "Symbol: " << config_.symbol << std::endl;
    std::cout << "Current Mid Price: " << std::fixed << std::setprecision(2)
              << current_mid_price_.load() << std::endl;

    auto [bid_order, ask_order] = order_manager_->get_active_orders();
    if (bid_order) {
        std::cout << "Active Bid: " << bid_order->price
                  << " (ID: " << bid_order->order_id << ")" << std::endl;
    }
    if (ask_order) {
        std::cout << "Active Ask: " << ask_order->price
                  << " (ID: " << ask_order->order_id << ")" << std::endl;
    }

    std::cout << "\nMetrics:" << std::endl;
    std::cout << "  Total Orders: " << metrics.total_orders << std::endl;
    std::cout << "  Successful: " << metrics.successful_orders << std::endl;
    std::cout << "  Failed: " << metrics.failed_orders << std::endl;
    std::cout << "\n  Execution Latency (function time):" << std::endl;
    std::cout << "    Avg: " << std::fixed << std::setprecision(3)
              << metrics.avg_order_latency_ms << " ms" << std::endl;
    std::cout << "    Min: " << metrics.min_order_latency_ms << " ms" << std::endl;
    std::cout << "    Max: " << metrics.max_order_latency_ms << " ms" << std::endl;
    std::cout << "\n  Reaction Latency (price change → order):" << std::endl;
    std::cout << "    Avg: " << std::fixed << std::setprecision(3)
              << metrics.avg_reaction_latency_ms << " ms" << std::endl;
    std::cout << "    Min: " << metrics.min_reaction_latency_ms << " ms" << std::endl;
    std::cout << "    Max: " << metrics.max_reaction_latency_ms << " ms" << std::endl;
    std::cout << "\n  Reconnects: " << metrics.reconnect_count << std::endl;
    std::cout << "  Uptime: " << std::fixed << std::setprecision(2)
              << metrics.get_uptime_percentage() << "%" << std::endl;
    std::cout << "=========================================" << std::endl;
}

std::string MarketMakerBotV2::format_symbol_for_exchange() {
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

LatencyMetrics MarketMakerBotV2::get_metrics() const {
    // This would return metrics from the order manager
    // For now, return empty metrics
    return LatencyMetrics();
}

} // namespace MarketMaker