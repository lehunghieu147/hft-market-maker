#include "trading/market_maker.h"
#include "core/config_loader.h"
#include "core/app_logger.h"
#include "core/metrics_server.h"
#include "core/gcp_auth_provider.h"
#include "cloud/gcp_publisher.h"
#include "cloud/gcp_storage_client.h"
#ifdef BUILD_GRPC
#include "network/grpc_server.h"
#endif
#include <iostream>
#include <signal.h>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <filesystem>

using namespace MarketMaker;

std::atomic<bool> should_exit(false);
std::unique_ptr<MarketMakerBot> bot;

void signal_handler(int /* signal */) {
    should_exit = true;
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

    std::cout << "\n"
              << "══════════════════════════════════════════════\n"
              << "  Market Maker Bot v2 — Startup Sequence\n"
              << "══════════════════════════════════════════════\n"
              << std::endl;

    std::cout << "▸ PHASE 1: Configuration" << std::endl;

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
        LOG_INFO(logger, "  [1/8] Loading {}", config_file);
        auto config_opt = ConfigLoader::load_from_file(config_file);

        if (!config_opt) {
            LOG_ERROR(logger, "{}", "Failed to load configuration!");
            AppLogger::shutdown();
            return 1;
        }

        Config config = *config_opt;

        LOG_INFO(logger, "  [2/8] Config: {} spread={:.2f}% size={} mode={}",
                 config.symbol, config.spread_percentage * 100, config.order_size,
                 config.use_websocket_trading ? "websocket" : "rest");

        // Create and initialize bot
        bot = std::make_unique<MarketMakerBot>(config);

        if (!bot->initialize()) {
            LOG_ERROR(logger, "{}", "Failed to initialize bot!");
            AppLogger::shutdown();
            return 1;
        }

        // Start metrics server
        std::unique_ptr<MetricsServer> metrics_server;
        if (config.metrics_port > 0) {
            metrics_server = std::make_unique<MetricsServer>(config.metrics_port);
            metrics_server->start();
            LOG_INFO(logger, "  [8/8] Prometheus :{}/metrics ... listening OK", config.metrics_port);
        }

        // Start gRPC server
#ifdef BUILD_GRPC
        std::unique_ptr<GrpcServer> grpc_server;
        if (config.grpc_port > 0) {
            grpc_server = std::make_unique<GrpcServer>(*bot, config.grpc_port);
            grpc_server->start();
        }
#endif

        // Start GCP publisher
        std::shared_ptr<GcpAuthProvider> gcp_auth;
        std::unique_ptr<GcpPublisher> gcp_publisher;
        std::unique_ptr<GcpStorageClient> gcp_storage;
        if (config.gcp.enabled && !config.gcp.service_account_path.empty()) {
            gcp_auth = std::make_shared<GcpAuthProvider>(config.gcp.service_account_path);
            if (gcp_auth->is_loaded()) {
                gcp_publisher = std::make_unique<GcpPublisher>(
                    *gcp_auth, config.gcp.project_id, config.gcp.pubsub_topic);
                gcp_publisher->start();
                gcp_storage = std::make_unique<GcpStorageClient>(
                    *gcp_auth, config.gcp.gcs_bucket);
                LOG_INFO(logger, "GCP integration enabled (project: {})", config.gcp.project_id);
                // Wire publisher into bot for trading event streaming
                bot->set_publisher(gcp_publisher.get());
            }
        }

        std::cout << "\n"
                  << "══════════════════════════════════════════════\n"
                  << "  READY — 2 WS connections active\n"
                  << "══════════════════════════════════════════════\n"
                  << std::endl;

        std::cout << "--- Trading Loop Started ---" << std::endl;

        bot->run();

        // Main loop - wait for exit signal
        while (!should_exit && bot->is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Ensure bot is stopped
        if (should_exit && bot) {
            LOG_INFO(logger, "{}", "Shutting down bot gracefully...");

            // Upload log file to GCS on shutdown
            if (gcp_storage) {
                gcp_storage->upload_file("logs/shutdown-" + config.symbol + ".log", config.log_file);
            }

#ifdef BUILD_GRPC
            if (grpc_server) grpc_server->stop();
#endif
            if (gcp_publisher) gcp_publisher->stop();
            if (metrics_server) metrics_server->stop();

            bot->stop();
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