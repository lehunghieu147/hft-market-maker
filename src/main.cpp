#include "market_maker_v2.h"
#include "config_loader.h"
#include <iostream>
#include <signal.h>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <filesystem>

using namespace MarketMaker;

std::atomic<bool> should_exit(false);
std::unique_ptr<MarketMakerBotV2> bot;

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
        std::cerr << "Error: Config file not found: " << config_file << std::endl;

        // Create a default config file if it doesn't exist
        if (config_file == "config.json") {
            std::cout << "\nCreating default config file: config.json" << std::endl;
            Config default_config;
            ConfigLoader::save_to_file(default_config, "config.json");
            std::cout << "\nPlease edit config.json and add your API credentials, then run again." << std::endl;
        } else {
            std::cout << "\nPlease create the config file or specify a valid path." << std::endl;
        }
        print_usage();
        return 1;
    }

    try {
        // Load configuration from file
        std::cout << "Loading configuration from: " << config_file << std::endl;
        auto config_opt = ConfigLoader::load_from_file(config_file);

        if (!config_opt) {
            std::cerr << "Failed to load configuration!" << std::endl;
            return 1;
        }

        Config config = *config_opt;

        std::cout << "Configuration:\n"
                  << "  Symbol: " << config.symbol << "\n"
                  << "  Order Size: " << config.order_size << "\n"
                  << "  Spread: " << (config.spread_percentage * 100) << "%\n";

        // Create and initialize bot
        bot = std::make_unique<MarketMakerBotV2>(config);

        std::cout << "Initializing bot..." << std::endl;
        if (!bot->initialize()) {
            std::cerr << "Failed to initialize bot!" << std::endl;
            return 1;
        }

        // Run bot
        std::cout << "Starting market maker bot...\n"
                  << "Press Ctrl+C to stop\n" << std::endl;

        bot->run();

        // Main loop - wait for exit signal
        while (!should_exit && bot->is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Ensure bot is stopped
        if (should_exit && bot) {
            std::cout << "\nShutting down bot gracefully..." << std::endl;
            bot->stop();
            // Give time for cleanup
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // Print final metrics
        auto metrics = bot->get_metrics();
        std::cout << "\n===========================================\n"
                  << "Final Statistics:\n"
                  << "  Total Orders: " << metrics.total_orders << "\n"
                  << "  Successful Orders: " << metrics.successful_orders << "\n"
                  << "  Failed Orders: " << metrics.failed_orders << "\n"
                  << "  Average Latency: " << metrics.avg_order_latency_ms << " ms\n"
                  << "  Min Latency: " << metrics.min_order_latency_ms << " ms\n"
                  << "  Max Latency: " << metrics.max_order_latency_ms << " ms\n"
                  << "  Reconnects: " << metrics.reconnect_count << "\n"
                  << "  Uptime: " << metrics.get_uptime_percentage() << "%\n"
                  << "===========================================\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Bot stopped successfully." << std::endl;
    return 0;
}