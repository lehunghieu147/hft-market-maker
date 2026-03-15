#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <chrono>
#include <map>
#include <vector>

namespace MarketMaker {

// Exchange-specific configuration
struct ExchangeEndpoints {
    std::string ws_url;
    std::string rest_url;
    std::string testnet_ws_url;
    std::string testnet_rest_url;
};

struct MomentumConfig {
    double epsilon = 0.0002;          // Signal threshold
    int ema_window = 400;             // EMA period
    int cooldown_ms = 500;            // Min ms between signals
    double max_position = 10.0;       // Max position size
    double order_size = 0.001;        // Order quantity (independent from MM)
    std::string order_type = "ioc";   // "ioc" or "market"
    double min_profit_bps = 0.0;      // Min profit threshold (basis points) above spread+fees
    bool tick_recording = false;      // Enable tick recording
    std::string tick_log_path = "logs/ticks.bin";

    // Multi-timeframe signal mode
    bool use_multi_timeframe = false;      // Enable dual-confirmation signals
    int fast_ema_window = 8;               // Fast EMA period
    int slow_ema_window = 50;              // Slow EMA period
    double volume_expansion_threshold = 1.2;  // Volume must be 1.2x avg to confirm
};

struct Config {
    // Exchange selection
    std::string exchange_type = "binance";  // "binance", "coinbase", "kraken", etc.
    bool use_testnet = false;               // Use testnet/sandbox endpoints

    // Symbol configuration
    std::string symbol = "BTCUSDT";         // Exchange-specific symbol format
    std::string base_asset = "BTC";         // Base currency (e.g., BTC)
    std::string quote_asset = "USDT";       // Quote currency (e.g., USDT)

    // Asset configuration for display and conversion
    std::vector<std::string> display_assets = {"USDT", "BTC"};  // Assets to display in account info
    std::vector<std::string> supported_quote_currencies = {"USDT", "BUSD", "ETH", "BNB"};  // For symbol conversion

    // Exchange endpoints (will be populated based on exchange_type)
    std::string ws_base_url = "wss://stream.binance.com:9443/ws";
    std::string rest_base_url = "https://api.binance.com";

    // WebSocket Trading API endpoint (for order management via WebSocket)
    std::string ws_trading_url = "wss://ws-api.binance.com:443";
    bool use_websocket_trading = false;  // Use WebSocket API for trading instead of REST

    // API Credentials (will be loaded from environment or config file)
    std::string api_key;
    std::string api_secret;
    std::string passphrase;  // For exchanges like Coinbase that require it

    // Trading parameters
    double spread_percentage = 0.02;  // 2% spread from mid price
    double order_size = 0.001;        // Order size in base currency
    int price_precision = 2;          // Price decimal precision
    int quantity_precision = 6;       // Quantity decimal precision

    // Performance settings
    std::chrono::milliseconds order_update_cooldown{100};  // Min time between order updates
    std::chrono::milliseconds reconnect_delay{5000};       // WebSocket reconnect delay
    int max_reconnect_attempts = 10;

    // Logging
    bool enable_verbose_logging = true;
    std::string log_file = "logs/market_maker.log";

    // Rate limiting (exchange-specific, will be overridden)
    int max_orders_per_second = 10;
    int max_requests_per_second = 10;
    int max_weight_per_minute = 1200;  // Binance-specific weight limit

    // Risk management
    double max_daily_loss = -100.0;
    double max_position_size = 0.5;
    double max_drawdown = -500.0;
    int max_consecutive_errors = 5;
    double maker_fee_rate = -0.0001;
    double taker_fee_rate = 0.001;

    // Avellaneda-Stoikov model parameters
    bool use_avellaneda_stoikov = false;    // Enable AS inventory-aware quoting
    double as_gamma = 0.001;               // Risk aversion (higher = wider spreads)
    double as_kappa = 1.5;                 // Order arrival intensity
    double as_time_horizon_sec = 300.0;    // Rolling time window (seconds)

    // Dynamic volatility-adjusted sizing parameters
    bool use_dynamic_sizing = false;        // Enable vol-adjusted order sizing
    double vol_sizing_exponent = 0.5;       // Power for size scaling (0.5 = square root)
    double min_size_multiplier = 0.25;      // Min size as fraction of base
    double max_size_multiplier = 2.0;       // Max size as fraction of base

    // Order Book Imbalance (OBI) tilt parameters
    bool use_obi_tilt = false;              // Enable OBI-based spread tilting
    int obi_levels = 5;                     // Number of orderbook levels for OBI
    double obi_tilt_factor = 0.3;           // Max tilt as fraction of spread (0.3 = 30%)
    double obi_min_volume = 50.0;           // Min total volume for OBI signal

    // Inventory skew for non-AS mode: skews bid/ask to revert position to neutral
    // 0.0 = disabled, typical: 0.1-0.5 (higher = more aggressive mean-reversion)
    double inventory_skew_factor = 0.0;

    // Multi-level quoting: place N levels per side at staggered prices
    int num_quote_levels = 1;              // 1 = single bid/ask (current behavior)
    double level_spacing_multiplier = 1.5; // Each level spread *= this^level
    double level_size_decay = 0.5;         // Each level size *= this^level

    // Drawdown-based spread widening: at max_drawdown, spread *= (1 + this value)
    // 0.0 = disabled
    double max_drawdown_spread_multiplier = 0.0;

    // Per-side position limits (0 = use symmetric max_position_size for both)
    double max_long_position = 0.0;
    double max_short_position = 0.0;

    // Time-of-day spread multiplier rules (empty = disabled)
    struct TimeOfDayRule {
        int start_hour_utc = 0;
        int end_hour_utc = 6;
        double spread_multiplier = 1.5;
    };
    std::vector<TimeOfDayRule> time_of_day_rules;

    // Toxic flow detection: widen spread when fills are heavily one-sided
    bool use_toxic_flow_detection = false;
    int toxic_flow_window = 50;
    double toxic_flow_threshold = 0.7;
    double toxic_flow_spread_mult = 1.5;

    // GLFT extension of AS model: adds inventory penalty near position limits
    bool use_glft = false;

    // Dual-window volatility regime detection
    int vol_fast_window = 20;
    double vol_regime_threshold = 2.0;
    double vol_regime_spread_mult = 2.0;

    // Exchange-specific parameters (optional)
    std::map<std::string, std::string> extra_params;

    // GCP Cloud Integration
    struct GcpConfig {
        bool enabled = false;
        std::string project_id;
        std::string service_account_path;
        std::string pubsub_topic = "trading-events";
        std::string gcs_bucket = "trading-logs";
    };
    GcpConfig gcp;

    // gRPC control API port (0 = disabled)
    int grpc_port = 50051;

    // Prometheus metrics HTTP port (0 = disabled)
    int metrics_port = 8888;

    // Momentum strategy config
    MomentumConfig momentum;

    // Static exchange endpoints database
    static std::map<std::string, ExchangeEndpoints> EXCHANGE_ENDPOINTS;

    // Helper method to get endpoints for selected exchange
    void update_endpoints_for_exchange() {
        auto it = EXCHANGE_ENDPOINTS.find(exchange_type);
        if (it != EXCHANGE_ENDPOINTS.end()) {
            if (use_testnet) {
                ws_base_url = it->second.testnet_ws_url;
                rest_base_url = it->second.testnet_rest_url;
            } else {
                ws_base_url = it->second.ws_url;
                rest_base_url = it->second.rest_url;
            }
        }
    }
};

} // namespace MarketMaker

#endif // CONFIG_H