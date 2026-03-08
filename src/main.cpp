#include "trading/market_maker.h"
#include "core/config_loader.h"
#include "core/app_logger.h"
#include <iostream>
#include <signal.h>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <filesystem>

using namespace MarketMaker;

std::atomic<bool> should_exit(false);
std::unique_ptr<MarketMakerBot> bot;

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    should_exit = true;
    if (bot) {
        bot->stop();
    }
    // Force exit after 1 second if still running
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "Force exiting..." << std::endl;
    std::exit(0);
}

void print_usage() {
    std::cout << "Market Maker Bot for Cryptocurrency Trading\n"
              << "===========================================\n"
              << "Usage: ./market_maker [config_file]\n\n"
              << "Arguments:\n"
              << "  config_file         - Path to JSON config file (default: config.json)\n\n"
              << "Examples:\n"
              << "  ./market_maker                  # Use default config.json\n"
              << "  ./market_maker config.json      # Use specific config file\n"
              << "  ./market_maker config.testnet.json  # Use testnet config\n\n"
              << "Config file can be overridden with environment variables:\n"
              << "  BINANCE_API_KEY     - Override API key from config\n"
              << "  BINANCE_API_SECRET  - Override API secret from config\n"
              << "  SYMBOL              - Override trading pair\n"
              << "  ORDER_SIZE          - Override order size\n"
              << "  SPREAD_PERCENTAGE   - Override spread percentage\n"
              << std::endl;
}

Config load_config_from_env() {
    Config config;

    // Load optional parameters from environment
    const char* symbol = std::getenv("SYMBOL");
    if (symbol) {
        config.symbol = symbol;
    }

    const char* order_size = std::getenv("ORDER_SIZE");
    if (order_size) {
        config.order_size = std::stod(order_size);
    }

    const char* spread = std::getenv("SPREAD_PERCENTAGE");
    if (spread) {
        config.spread_percentage = std::stod(spread);
    }

    const char* log_file = std::getenv("LOG_FILE");
    if (log_file) {
        config.log_file = log_file;
    }

    const char* verbose = std::getenv("VERBOSE");
    if (verbose && std::string(verbose) == "false") {
        config.enable_verbose_logging = false;
    }

    return config;
}

int main(int argc, char* argv[]) {
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Check for help flag
    if (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        print_usage();
        return 0;
    }

    // Initialize logger early
    AppLogger::init();
    auto* logger = AppLogger::get("core");

    std::cout << "===========================================\n"
              << "    Market Maker Bot - High Frequency Trading\n"
              << "===========================================\n" << std::endl;

    // Determine config file path
    std::string config_file = "config.json";
    if (argc > 1) {
        config_file = argv[1];
    }

    // Check if config file exists
    if (!std::filesystem::exists(config_file)) {
        LOG_ERROR(logger, "Config file not found: {}", config_file);

        // Create a default config file if it doesn't exist
        if (config_file == "config.json") {
            LOG_INFO(logger, "{}", "Creating default config file: config.json");
            Config default_config;
            ConfigLoader::save_to_file(default_config, "config.json");
            LOG_INFO(logger, "{}", "Please edit config.json and add your API credentials, then run again.");
        } else {
            LOG_INFO(logger, "{}", "Please create the config file or specify a valid path.");
        }
        print_usage();
        AppLogger::shutdown();
        return 1;
    }

    try {
        // Load configuration from file
        LOG_INFO(logger, "Loading configuration from: {}", config_file);
        auto config_opt = ConfigLoader::load_from_file(config_file);

        if (!config_opt) {
            LOG_ERROR(logger, "{}", "Failed to load configuration!");
            AppLogger::shutdown();
            return 1;
        }

        Config config = *config_opt;

        LOG_INFO(logger, "Configuration: symbol={} order_size={} spread={:.2f}%",
                 config.symbol, config.order_size, config.spread_percentage * 100);

        // Create and initialize bot
        bot = std::make_unique<MarketMakerBot>(config);

        LOG_INFO(logger, "{}", "Initializing bot...");
        if (!bot->initialize()) {
            LOG_ERROR(logger, "{}", "Failed to initialize bot!");
            AppLogger::shutdown();
            return 1;
        }

        // Run bot
        LOG_INFO(logger, "{}", "Starting market maker bot... Press Ctrl+C to stop");

        bot->run();

        // Main loop - wait for exit signal
        while (!should_exit && bot->is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Ensure bot is stopped
        if (should_exit && bot) {
            LOG_INFO(logger, "{}", "Shutting down bot gracefully...");
            bot->stop();
            // Give time for cleanup
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // Print final metrics
        auto metrics = bot->get_metrics();
        LOG_INFO(logger,
                 "[FINAL] orders(total={} ok={} fail={} rate={:.1f}% opm={:.1f}) "
                 "latency(avg={:.3f} min={:.3f} max={:.3f}ms) "
                 "reconnects={} uptime={:.2f}%",
                 metrics.total_orders, metrics.successful_orders, metrics.failed_orders,
                 metrics.get_success_rate(), metrics.get_orders_per_minute(),
                 metrics.avg_order_latency_ms, metrics.min_order_latency_ms,
                 metrics.max_order_latency_ms, metrics.reconnect_count,
                 metrics.get_uptime_percentage());

    } catch (const std::exception& e) {
        LOG_CRITICAL(logger, "Fatal error: {}", e.what());
        AppLogger::shutdown();
        return 1;
    }

    LOG_INFO(logger, "{}", "Bot stopped successfully.");
    AppLogger::shutdown();
    return 0;
}