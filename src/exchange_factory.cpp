#include "exchange_factory.h"
#include "binance_exchange.h"
#include "websocket_trading_adapter.h"
// Include other exchange implementations here as they're created
// #include "coinbase_exchange.h"
// #include "kraken_exchange.h"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace MarketMaker {

// Static registration of built-in exchanges
namespace {
    struct ExchangeInitializer {
        ExchangeInitializer() {
            // Register Binance
            ExchangeFactory::instance().register_exchange(
                "binance",
                []() { return std::make_shared<BinanceExchange>(); }
            );

            // Register other exchanges as they're implemented
            // ExchangeFactory::instance().register_exchange(
            //     "coinbase",
            //     []() { return std::make_shared<CoinbaseExchange>(); }
            // );

            // Placeholder for future exchanges
            auto placeholder_creator = []() -> std::shared_ptr<IExchange> {
                std::cerr << "Exchange not yet implemented!" << std::endl;
                return nullptr;
            };

            ExchangeFactory::instance().register_exchange("coinbase", placeholder_creator);
            ExchangeFactory::instance().register_exchange("kraken", placeholder_creator);
            ExchangeFactory::instance().register_exchange("bybit", placeholder_creator);
            ExchangeFactory::instance().register_exchange("okx", placeholder_creator);
            ExchangeFactory::instance().register_exchange("bitget", placeholder_creator);
            ExchangeFactory::instance().register_exchange("kucoin", placeholder_creator);
        }
    };

    // Static initializer ensures exchanges are registered at program start
    static ExchangeInitializer initializer;
}

std::shared_ptr<IExchange> ExchangeFactory::create(const ExchangeConfig& config) {
    std::string normalized_name = normalize_exchange_name(config.exchange_type);

    // Check if WebSocket trading is requested for Binance
    if (normalized_name == "binance" && config.use_websocket_trading) {
        std::cout << "Creating Binance WebSocket Trading adapter..." << std::endl;

        auto ws_adapter = std::make_shared<WebSocketTradingAdapter>(
            config.api_key,
            config.api_secret,
            config.ws_url,
            config.ws_trading_url
        );

        // The adapter doesn't need initialize() call as it initializes in constructor
        std::cout << "Successfully created Binance WebSocket Trading instance" << std::endl;
        return ws_adapter;
    }

    auto& factory = instance();
    auto it = factory.exchange_registry_.find(normalized_name);

    if (it != factory.exchange_registry_.end()) {
        auto exchange = it->second();
        if (exchange) {
            // Initialize the exchange with config
            if (exchange->initialize(config)) {
                std::cout << "Successfully created " << normalized_name << " exchange instance" << std::endl;
                return exchange;
            } else {
                std::cerr << "Failed to initialize " << normalized_name << " exchange" << std::endl;
                return nullptr;
            }
        }
    }

    std::cerr << "Exchange type '" << config.exchange_type << "' not supported" << std::endl;
    std::cerr << "Supported exchanges: ";
    for (const auto& name : get_supported_exchanges()) {
        std::cerr << name << " ";
    }
    std::cerr << std::endl;

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

    static std::map<std::string, ExchangeType> type_map = {
        {"binance", ExchangeType::BINANCE},
        {"coinbase", ExchangeType::COINBASE},
        {"kraken", ExchangeType::KRAKEN},
        {"ftx", ExchangeType::FTX},
        {"bybit", ExchangeType::BYBIT},
        {"okx", ExchangeType::OKX},
        {"bitget", ExchangeType::BITGET},
        {"kucoin", ExchangeType::KUCOIN}
    };

    auto it = type_map.find(normalized);
    if (it != type_map.end()) {
        return it->second;
    }

    return ExchangeType::UNKNOWN;
}

std::string ExchangeFactory::get_exchange_name(ExchangeType type) {
    switch (type) {
        case ExchangeType::BINANCE:  return "binance";
        case ExchangeType::COINBASE: return "coinbase";
        case ExchangeType::KRAKEN:   return "kraken";
        case ExchangeType::FTX:      return "ftx";
        case ExchangeType::BYBIT:    return "bybit";
        case ExchangeType::OKX:      return "okx";
        case ExchangeType::BITGET:   return "bitget";
        case ExchangeType::KUCOIN:   return "kucoin";
        default:                     return "unknown";
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

    // Convert to lowercase
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Remove common variations
    if (normalized == "binance.com" || normalized == "binance.us") {
        normalized = "binance";
    } else if (normalized == "coinbase" || normalized == "coinbasepro" || normalized == "coinbase pro") {
        normalized = "coinbase";
    } else if (normalized == "okex") {
        normalized = "okx";  // OKEx rebranded to OKX
    }

    return normalized;
}

} // namespace MarketMaker