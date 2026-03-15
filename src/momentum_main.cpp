#include "trading/momentum_taker.h"
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
std::unique_ptr<MomentumTakerBot> bot;

void signal_handler(int /* signal */) {
    should_exit = true;
}

void print_usage() {
    std::cout << "Momentum Taker Bot for Cryptocurrency Trading\n"
              << "==============================================\n"
              << "Usage: ./momentum_taker [config_file]\n\n"
              << "Arguments:\n"
              << "  config_file  - Path to JSON config file (default: config.momentum.json)\n\n"
              << "Examples:\n"
              << "  ./momentum_taker                           # Use default config\n"
              << "  ./momentum_taker config.momentum.json      # Use specific config\n\n"
              << "Momentum-specific config keys (in \"momentum\" section):\n"
              << "  epsilon       - Signal threshold (default: 0.0002)\n"
              << "  ema_window    - EMA period (default: 400)\n"
              << "  cooldown_ms   - Min ms between signals (default: 500)\n"
              << "  max_position  - Max position size (default: 10.0)\n"
              << "  order_size    - Order quantity (default: 0.001)\n"
              << "  order_type    - \"ioc\" or \"market\" (default: ioc)\n\n"
              << "Environment variable overrides:\n"
              << "  BINANCE_API_KEY     - Override API key\n"
              << "  BINANCE_API_SECRET  - Override API secret\n"
              << "  SYMBOL              - Override trading pair\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        print_usage();
        return 0;
    }

    AppLogger::init();
    auto* logger = AppLogger::get("core");

    std::cout << "\n"
              << "══════════════════════════════════════════════\n"
              << "  Momentum Taker Bot — Startup Sequence\n"
              << "══════════════════════════════════════════════\n"
              << std::endl;

    std::cout << "\xE2\x96\xB8 PHASE 1: Configuration" << std::endl;

    std::string config_file = "config.momentum.json";
    if (argc > 1) {
        config_file = argv[1];
    }

    if (!std::filesystem::exists(config_file)) {
        LOG_ERROR(logger, "Config file not found: {}", config_file);

        if (config_file == "config.momentum.json") {
            LOG_INFO(logger, "{}", "Creating default config: config.momentum.json");
            Config default_config;
            ConfigLoader::save_to_file(default_config, "config.momentum.json");
            LOG_INFO(logger, "{}", "Please edit config.momentum.json and add your API credentials.");
        } else {
            LOG_INFO(logger, "{}", "Please create the config file or specify a valid path.");
        }
        print_usage();
        AppLogger::shutdown();
        return 1;
    }

    try {
        LOG_INFO(logger, "  [1/7] Loading {}", config_file);
        auto config_opt = ConfigLoader::load_from_file(config_file);

        if (!config_opt) {
            LOG_ERROR(logger, "{}", "Failed to load configuration!");
            AppLogger::shutdown();
            return 1;
        }

        Config config = *config_opt;

        LOG_INFO(logger, "  [2/7] Config: {} epsilon={} ema={} size={} type={}",
                 config.symbol, config.momentum.epsilon, config.momentum.ema_window,
                 config.momentum.order_size, config.momentum.order_type);

        bot = std::make_unique<MomentumTakerBot>(config);

        if (!bot->initialize()) {
            LOG_ERROR(logger, "{}", "Failed to initialize bot!");
            AppLogger::shutdown();
            return 1;
        }

        std::cout << "\n"
                  << "══════════════════════════════════════════════\n"
                  << "  READY — Momentum Taker on " << config.exchange_type << "\n"
                  << "══════════════════════════════════════════════\n"
                  << std::endl;

        std::cout << "--- Signal Detection Active ---" << std::endl;

        bot->run();

        while (!should_exit && bot->is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (should_exit && bot) {
            LOG_INFO(logger, "{}", "Shutting down bot gracefully...");
            bot->stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

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
