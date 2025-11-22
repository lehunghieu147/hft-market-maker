#include "config_loader.h"
#include <json/json.h>
#include <fstream>
#include <iostream>
#include <cstdlib>

namespace MarketMaker {

std::optional<Config> ConfigLoader::load_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open config file: " << filename << std::endl;
        return std::nullopt;
    }

    Json::Value root;
    Json::Reader reader;

    if (!reader.parse(file, root)) {
        std::cerr << "Error: Failed to parse config file: " << filename << std::endl;
        std::cerr << "Parser error: " << reader.getFormattedErrorMessages() << std::endl;
        return std::nullopt;
    }

    Config config;

    try {
        // API credentials
        if (root.isMember("api")) {
            config.api_key = root["api"]["key"].asString();
            config.api_secret = root["api"]["secret"].asString();
        }

        // Trading parameters
        if (root.isMember("trading")) {
            config.symbol = root["trading"]["symbol"].asString();
            config.order_size = root["trading"]["order_size"].asDouble();
            config.spread_percentage = root["trading"]["spread_percentage"].asDouble();

            // Load base and quote assets
            if (root["trading"].isMember("base_asset")) {
                config.base_asset = root["trading"]["base_asset"].asString();
            }
            if (root["trading"].isMember("quote_asset")) {
                config.quote_asset = root["trading"]["quote_asset"].asString();
            }

            // Load display assets array
            if (root["trading"].isMember("display_assets") && root["trading"]["display_assets"].isArray()) {
                config.display_assets.clear();
                for (const auto& asset : root["trading"]["display_assets"]) {
                    config.display_assets.push_back(asset.asString());
                }
            }

            // Load supported quote currencies array
            if (root["trading"].isMember("supported_quote_currencies") &&
                root["trading"]["supported_quote_currencies"].isArray()) {
                config.supported_quote_currencies.clear();
                for (const auto& currency : root["trading"]["supported_quote_currencies"]) {
                    config.supported_quote_currencies.push_back(currency.asString());
                }
            }
        }

        // Exchange settings
        if (root.isMember("exchange")) {
            config.exchange_type = root["exchange"]["name"].asString();
            config.ws_base_url = root["exchange"]["ws_url"].asString();
            config.rest_base_url = root["exchange"]["rest_url"].asString();

            // WebSocket Trading API settings
            if (root["exchange"].isMember("ws_trading_url")) {
                config.ws_trading_url = root["exchange"]["ws_trading_url"].asString();
            }
            if (root["exchange"].isMember("use_websocket_trading")) {
                config.use_websocket_trading = root["exchange"]["use_websocket_trading"].asBool();
            }

            // Check for testnet setting
            if (root["exchange"].isMember("testnet")) {
                config.use_testnet = root["exchange"]["testnet"].asBool();
            }
        }

        // Performance settings
        if (root.isMember("performance")) {
            config.order_update_cooldown = std::chrono::milliseconds(
                root["performance"]["order_update_cooldown_ms"].asInt()
            );
            config.reconnect_delay = std::chrono::milliseconds(
                root["performance"]["reconnect_delay_ms"].asInt()
            );
            config.max_reconnect_attempts = root["performance"]["max_reconnect_attempts"].asInt();
            config.max_orders_per_second = root["performance"]["max_orders_per_second"].asInt();
        }

        // Logging settings
        if (root.isMember("logging")) {
            config.enable_verbose_logging = root["logging"]["verbose"].asBool();
            config.log_file = root["logging"]["file"].asString();
        }

        // Merge with environment variables (env vars take priority)
        merge_with_env(config);

        // Validate configuration
        if (!validate(config)) {
            return std::nullopt;
        }

        return config;

    } catch (const std::exception& e) {
        std::cerr << "Error loading config: " << e.what() << std::endl;
        return std::nullopt;
    }
}

bool ConfigLoader::save_to_file(const Config& config, const std::string& filename) {
    Json::Value root;

    // API section (mask the secret for security)
    root["api"]["key"] = config.api_key.empty() ? "YOUR_API_KEY_HERE" : mask_secret(config.api_key);
    root["api"]["secret"] = config.api_secret.empty() ? "YOUR_API_SECRET_HERE" : mask_secret(config.api_secret);

    // Trading section
    root["trading"]["symbol"] = config.symbol;
    root["trading"]["order_size"] = config.order_size;
    root["trading"]["spread_percentage"] = config.spread_percentage;

    // Exchange section
    root["exchange"]["name"] = config.exchange_type;
    root["exchange"]["ws_url"] = config.ws_base_url;
    root["exchange"]["rest_url"] = config.rest_base_url;
    root["exchange"]["ws_trading_url"] = config.ws_trading_url;
    root["exchange"]["use_websocket_trading"] = config.use_websocket_trading;
    root["exchange"]["testnet"] = config.use_testnet;

    // Performance section
    root["performance"]["order_update_cooldown_ms"] = static_cast<int>(config.order_update_cooldown.count());
    root["performance"]["reconnect_delay_ms"] = static_cast<int>(config.reconnect_delay.count());
    root["performance"]["max_reconnect_attempts"] = config.max_reconnect_attempts;
    root["performance"]["max_orders_per_second"] = config.max_orders_per_second;

    // Logging section
    root["logging"]["enabled"] = true;
    root["logging"]["verbose"] = config.enable_verbose_logging;
    root["logging"]["file"] = config.log_file;
    root["logging"]["level"] = "INFO";

    // Write to file
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot write to config file: " << filename << std::endl;
        return false;
    }

    Json::StyledWriter writer;
    file << writer.write(root);
    file.close();

    std::cout << "Configuration saved to: " << filename << std::endl;
    return true;
}

bool ConfigLoader::validate(const Config& config) {
    bool valid = true;

    // Check API credentials
    if (config.api_key.empty() || config.api_key == "YOUR_BINANCE_API_KEY_HERE" ||
        config.api_key == "YOUR_TESTNET_API_KEY_HERE") {
        std::cerr << "Error: API key is not configured" << std::endl;
        std::cerr << "Please edit the config file and add your Binance API key" << std::endl;
        valid = false;
    }

    if (config.api_secret.empty() || config.api_secret == "YOUR_BINANCE_API_SECRET_HERE" ||
        config.api_secret == "YOUR_TESTNET_API_SECRET_HERE") {
        std::cerr << "Error: API secret is not configured" << std::endl;
        std::cerr << "Please edit the config file and add your Binance API secret" << std::endl;
        valid = false;
    }

    // Check trading parameters
    if (config.symbol.empty()) {
        std::cerr << "Error: Trading symbol is not configured" << std::endl;
        valid = false;
    }

    if (config.order_size <= 0) {
        std::cerr << "Error: Invalid order size: " << config.order_size << std::endl;
        valid = false;
    }

    if (config.spread_percentage <= 0 || config.spread_percentage > 0.1) {
        std::cerr << "Error: Invalid spread percentage: " << config.spread_percentage << std::endl;
        std::cerr << "Spread should be between 0 and 0.1 (10%)" << std::endl;
        valid = false;
    }

    // Check URLs
    if (config.ws_base_url.empty() || config.rest_base_url.empty()) {
        std::cerr << "Error: Exchange URLs are not configured" << std::endl;
        valid = false;
    }

    return valid;
}

void ConfigLoader::merge_with_env(Config& config) {
    // Environment variables override config file values
    const char* env_api_key = std::getenv("BINANCE_API_KEY");
    if (env_api_key) {
        config.api_key = env_api_key;
        std::cout << "Using API key from environment variable" << std::endl;
    }

    const char* env_api_secret = std::getenv("BINANCE_API_SECRET");
    if (env_api_secret) {
        config.api_secret = env_api_secret;
        std::cout << "Using API secret from environment variable" << std::endl;
    }

    const char* env_symbol = std::getenv("SYMBOL");
    if (env_symbol) {
        config.symbol = env_symbol;
        std::cout << "Using symbol from environment: " << env_symbol << std::endl;
    }

    const char* env_order_size = std::getenv("ORDER_SIZE");
    if (env_order_size) {
        config.order_size = std::stod(env_order_size);
        std::cout << "Using order size from environment: " << config.order_size << std::endl;
    }

    const char* env_spread = std::getenv("SPREAD_PERCENTAGE");
    if (env_spread) {
        config.spread_percentage = std::stod(env_spread);
        std::cout << "Using spread from environment: " << config.spread_percentage << std::endl;
    }

    const char* env_log_file = std::getenv("LOG_FILE");
    if (env_log_file) {
        config.log_file = env_log_file;
        std::cout << "Using log file from environment: " << env_log_file << std::endl;
    }
}

std::string ConfigLoader::mask_secret(const std::string& secret) {
    if (secret.length() <= 8) {
        return std::string(secret.length(), '*');
    }
    // Show first 4 and last 4 characters
    return secret.substr(0, 4) + std::string(secret.length() - 8, '*') + secret.substr(secret.length() - 4);
}

} // namespace MarketMaker