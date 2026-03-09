#include "core/config_loader.h"
#include "core/app_logger.h"
#include <json/json.h>
#include <fstream>
#include <cstdlib>

namespace {
    quill::Logger* get_logger() {
        static quill::Logger* logger = MarketMaker::AppLogger::get("core");
        return logger;
    }
}

namespace MarketMaker {

std::optional<Config> ConfigLoader::load_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        LOG_ERROR(get_logger(), "Cannot open config file: {}", filename);
        return std::nullopt;
    }

    Json::Value root;
    Json::Reader reader;

    if (!reader.parse(file, root)) {
        LOG_ERROR(get_logger(), "Failed to parse config file: {}", filename);
        LOG_ERROR(get_logger(), "Parser error: {}", reader.getFormattedErrorMessages());
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

            // Precision settings
            if (root["trading"].isMember("price_precision"))
                config.price_precision = root["trading"]["price_precision"].asInt();
            if (root["trading"].isMember("quantity_precision"))
                config.quantity_precision = root["trading"]["quantity_precision"].asInt();

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

        // Risk management settings
        if (root.isMember("risk")) {
            if (root["risk"].isMember("max_daily_loss"))
                config.max_daily_loss = root["risk"]["max_daily_loss"].asDouble();
            if (root["risk"].isMember("max_position_size"))
                config.max_position_size = root["risk"]["max_position_size"].asDouble();
            if (root["risk"].isMember("max_drawdown"))
                config.max_drawdown = root["risk"]["max_drawdown"].asDouble();
            if (root["risk"].isMember("max_consecutive_errors"))
                config.max_consecutive_errors = root["risk"]["max_consecutive_errors"].asInt();
            if (root["risk"].isMember("maker_fee_rate"))
                config.maker_fee_rate = root["risk"]["maker_fee_rate"].asDouble();
            if (root["risk"].isMember("taker_fee_rate"))
                config.taker_fee_rate = root["risk"]["taker_fee_rate"].asDouble();
        }

        // Avellaneda-Stoikov model settings
        if (root.isMember("strategy")) {
            auto& s = root["strategy"];
            if (s.isMember("use_avellaneda_stoikov"))
                config.use_avellaneda_stoikov = s["use_avellaneda_stoikov"].asBool();
            if (s.isMember("as_gamma"))
                config.as_gamma = s["as_gamma"].asDouble();
            if (s.isMember("as_kappa"))
                config.as_kappa = s["as_kappa"].asDouble();
            if (s.isMember("as_time_horizon_sec"))
                config.as_time_horizon_sec = s["as_time_horizon_sec"].asDouble();
            if (s.isMember("use_dynamic_sizing"))
                config.use_dynamic_sizing = s["use_dynamic_sizing"].asBool();
            if (s.isMember("vol_sizing_exponent"))
                config.vol_sizing_exponent = s["vol_sizing_exponent"].asDouble();
            if (s.isMember("min_size_multiplier"))
                config.min_size_multiplier = s["min_size_multiplier"].asDouble();
            if (s.isMember("max_size_multiplier"))
                config.max_size_multiplier = s["max_size_multiplier"].asDouble();
            if (s.isMember("use_obi_tilt"))
                config.use_obi_tilt = s["use_obi_tilt"].asBool();
            if (s.isMember("obi_levels"))
                config.obi_levels = s["obi_levels"].asInt();
            if (s.isMember("obi_tilt_factor"))
                config.obi_tilt_factor = s["obi_tilt_factor"].asDouble();
            if (s.isMember("obi_min_volume"))
                config.obi_min_volume = s["obi_min_volume"].asDouble();
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

        // Momentum strategy settings
        if (root.isMember("momentum")) {
            auto& m = root["momentum"];
            if (m.isMember("epsilon"))
                config.momentum.epsilon = m["epsilon"].asDouble();
            if (m.isMember("ema_window"))
                config.momentum.ema_window = m["ema_window"].asInt();
            if (m.isMember("cooldown_ms"))
                config.momentum.cooldown_ms = m["cooldown_ms"].asInt();
            if (m.isMember("max_position"))
                config.momentum.max_position = m["max_position"].asDouble();
            if (m.isMember("order_size"))
                config.momentum.order_size = m["order_size"].asDouble();
            if (m.isMember("order_type"))
                config.momentum.order_type = m["order_type"].asString();
            if (m.isMember("min_profit_bps"))
                config.momentum.min_profit_bps = m["min_profit_bps"].asDouble();
            if (m.isMember("tick_recording"))
                config.momentum.tick_recording = m["tick_recording"].asBool();
            if (m.isMember("tick_log_path"))
                config.momentum.tick_log_path = m["tick_log_path"].asString();
            if (m.isMember("use_multi_timeframe"))
                config.momentum.use_multi_timeframe = m["use_multi_timeframe"].asBool();
            if (m.isMember("fast_ema_window"))
                config.momentum.fast_ema_window = m["fast_ema_window"].asInt();
            if (m.isMember("slow_ema_window"))
                config.momentum.slow_ema_window = m["slow_ema_window"].asInt();
            if (m.isMember("volume_expansion_threshold"))
                config.momentum.volume_expansion_threshold = m["volume_expansion_threshold"].asDouble();
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
        LOG_ERROR(get_logger(), "Error loading config: {}", e.what());
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
        LOG_ERROR(get_logger(), "Cannot write to config file: {}", filename);
        return false;
    }

    Json::StyledWriter writer;
    file << writer.write(root);
    file.close();

    LOG_INFO(get_logger(), "Configuration saved to: {}", filename);
    return true;
}

bool ConfigLoader::validate(const Config& config) {
    bool valid = true;

    // Check API credentials
    if (config.api_key.empty() || config.api_key == "YOUR_BINANCE_API_KEY_HERE" ||
        config.api_key == "YOUR_TESTNET_API_KEY_HERE") {
        LOG_ERROR(get_logger(), "{}", "API key is not configured");
        LOG_ERROR(get_logger(), "{}", "Please edit the config file and add your Binance API key");
        valid = false;
    }

    if (config.api_secret.empty() || config.api_secret == "YOUR_BINANCE_API_SECRET_HERE" ||
        config.api_secret == "YOUR_TESTNET_API_SECRET_HERE") {
        LOG_ERROR(get_logger(), "{}", "API secret is not configured");
        LOG_ERROR(get_logger(), "{}", "Please edit the config file and add your Binance API secret");
        valid = false;
    }

    // Check trading parameters
    if (config.symbol.empty()) {
        LOG_ERROR(get_logger(), "{}", "Trading symbol is not configured");
        valid = false;
    }

    if (config.order_size <= 0) {
        LOG_ERROR(get_logger(), "Invalid order size: {}", config.order_size);
        valid = false;
    }

    if (config.spread_percentage <= 0 || config.spread_percentage > 0.1) {
        LOG_ERROR(get_logger(), "Invalid spread percentage: {}", config.spread_percentage);
        LOG_ERROR(get_logger(), "{}", "Spread should be between 0 and 0.1 (10%)");
        valid = false;
    }

    // Check URLs
    if (config.ws_base_url.empty() || config.rest_base_url.empty()) {
        LOG_ERROR(get_logger(), "{}", "Exchange URLs are not configured");
        valid = false;
    }

    return valid;
}

void ConfigLoader::merge_with_env(Config& config) {
    // Environment variables override config file values
    const char* env_api_key = std::getenv("BINANCE_API_KEY");
    if (env_api_key) {
        config.api_key = env_api_key;
        LOG_INFO(get_logger(), "{}", "Using API key from environment variable");
    }

    const char* env_api_secret = std::getenv("BINANCE_API_SECRET");
    if (env_api_secret) {
        config.api_secret = env_api_secret;
        LOG_INFO(get_logger(), "{}", "Using API secret from environment variable");
    }

    const char* env_symbol = std::getenv("SYMBOL");
    if (env_symbol) {
        config.symbol = env_symbol;
        LOG_INFO(get_logger(), "Using symbol from environment: {}", env_symbol);
    }

    const char* env_order_size = std::getenv("ORDER_SIZE");
    if (env_order_size) {
        config.order_size = std::stod(env_order_size);
        LOG_INFO(get_logger(), "Using order size from environment: {}", config.order_size);
    }

    const char* env_spread = std::getenv("SPREAD_PERCENTAGE");
    if (env_spread) {
        config.spread_percentage = std::stod(env_spread);
        LOG_INFO(get_logger(), "Using spread from environment: {}", config.spread_percentage);
    }

    const char* env_log_file = std::getenv("LOG_FILE");
    if (env_log_file) {
        config.log_file = env_log_file;
        LOG_INFO(get_logger(), "Using log file from environment: {}", env_log_file);
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