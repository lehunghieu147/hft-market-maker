#include "exchange/exchange_factory.h"
#include "exchange/binance_exchange.h"
#include "network/websocket_trading_adapter.h"

#include "core/app_logger.h"
#include <algorithm>
#include <cctype>

namespace MarketMaker {

namespace {
    quill::Logger* get_logger() {
        static quill::Logger* logger = AppLogger::get("core");
        return logger;
    }
}

// Static registration of built-in exchanges
namespace {
    struct ExchangeInitializer {
        ExchangeInitializer() {
            ExchangeFactory::instance().register_exchange(
                "binance",
                []() { return std::make_shared<BinanceExchange>(); }
            );
        }
    };

    static ExchangeInitializer initializer;
}

std::shared_ptr<IExchange> ExchangeFactory::create(const ExchangeConfig& config) {
    std::string normalized_name = normalize_exchange_name(config.exchange_type);

    // Check if WebSocket trading is requested for Binance
    if (normalized_name == "binance" && config.use_websocket_trading) {
        auto ws_adapter = std::make_shared<WebSocketTradingAdapter>(
            config.api_key,
            config.api_secret,
            config.ws_url,
            config.ws_trading_url,
            config.price_precision,
            config.quantity_precision
        );
        return ws_adapter;
    }

    auto& factory = instance();
    auto it = factory.exchange_registry_.find(normalized_name);

    if (it != factory.exchange_registry_.end()) {
        auto exchange = it->second();
        if (exchange) {
            if (exchange->initialize(config)) {
                return exchange;
            } else {
                LOG_ERROR(get_logger(), "Failed to initialize {} exchange", normalized_name);
                return nullptr;
            }
        }
    }

    LOG_ERROR(get_logger(), "Exchange type '{}' not supported", config.exchange_type);
    return nullptr;
}

std::shared_ptr<IExchange> ExchangeFactory::create(
    ExchangeType type,
    const ExchangeConfig& config
) {
    ExchangeConfig mutable_config = config;
    mutable_config.exchange_type = get_exchange_name(type);
    return create(mutable_config);
}

void ExchangeFactory::register_exchange(
    const std::string& name,
    ExchangeCreator creator
) {
    std::string normalized = normalize_exchange_name(name);
    exchange_registry_[normalized] = creator;
}

ExchangeType ExchangeFactory::get_exchange_type(const std::string& name) {
    std::string normalized = normalize_exchange_name(name);

    if (normalized == "binance") return ExchangeType::BINANCE;
    return ExchangeType::UNKNOWN;
}

std::string ExchangeFactory::get_exchange_name(ExchangeType type) {
    switch (type) {
        case ExchangeType::BINANCE: return "binance";
        default:                    return "unknown";
    }
}

std::vector<std::string> ExchangeFactory::get_supported_exchanges() {
    std::vector<std::string> exchanges;
    auto& factory = instance();

    for (const auto& pair : factory.exchange_registry_) {
        exchanges.push_back(pair.first);
    }

    std::sort(exchanges.begin(), exchanges.end());
    return exchanges;
}

bool ExchangeFactory::is_supported(const std::string& exchange_name) {
    std::string normalized = normalize_exchange_name(exchange_name);
    auto& factory = instance();
    return factory.exchange_registry_.find(normalized) != factory.exchange_registry_.end();
}

std::string ExchangeFactory::normalize_exchange_name(const std::string& name) {
    std::string normalized = name;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (normalized == "binance.com" || normalized == "binance.us") {
        normalized = "binance";
    }

    return normalized;
}

} // namespace MarketMaker