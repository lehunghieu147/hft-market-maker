#ifndef EXCHANGE_FACTORY_H
#define EXCHANGE_FACTORY_H

#include "exchange_interface.h"
#include <memory>
#include <string>
#include <map>
#include <functional>

namespace MarketMaker {

// Supported exchange types
enum class ExchangeType {
    BINANCE,
    COINBASE,
    KRAKEN,
    FTX,       // For historical reference
    BYBIT,
    OKX,
    BITGET,
    KUCOIN,
    UNKNOWN
};

// Factory class for creating exchange instances
class ExchangeFactory {
public:
    // Singleton pattern for factory
    static ExchangeFactory& instance() {
        static ExchangeFactory instance;
        return instance;
    }

    // Main factory method
    static std::shared_ptr<IExchange> create(const ExchangeConfig& config);

    // Alternative factory method using exchange type enum
    static std::shared_ptr<IExchange> create(
        ExchangeType type,
        const ExchangeConfig& config
    );

    // Register a custom exchange implementation
    using ExchangeCreator = std::function<std::shared_ptr<IExchange>()>;
    void register_exchange(const std::string& name, ExchangeCreator creator);

    // Get exchange type from string
    static ExchangeType get_exchange_type(const std::string& name);

    // Get string from exchange type
    static std::string get_exchange_name(ExchangeType type);

    // List all supported exchanges
    static std::vector<std::string> get_supported_exchanges();

    // Check if an exchange is supported
    static bool is_supported(const std::string& exchange_name);

private:
    ExchangeFactory() = default;
    ~ExchangeFactory() = default;

    // Delete copy and move constructors
    ExchangeFactory(const ExchangeFactory&) = delete;
    ExchangeFactory& operator=(const ExchangeFactory&) = delete;

    // Registry of exchange creators
    std::map<std::string, ExchangeCreator> exchange_registry_;

    // Helper to normalize exchange names
    static std::string normalize_exchange_name(const std::string& name);
};

// Helper class for auto-registration of exchanges
template<typename ExchangeClass>
class ExchangeRegistrar {
public:
    ExchangeRegistrar(const std::string& name) {
        ExchangeFactory::instance().register_exchange(
            name,
            []() { return std::make_shared<ExchangeClass>(); }
        );
    }
};

// Macro for easy registration
#define REGISTER_EXCHANGE(ClassName, Name) \
    static ExchangeRegistrar<ClassName> registrar_##ClassName(Name);

} // namespace MarketMaker

#endif // EXCHANGE_FACTORY_H