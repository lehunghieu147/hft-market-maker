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

    // Exchange-specific parameters (optional)
    std::map<std::string, std::string> extra_params;

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