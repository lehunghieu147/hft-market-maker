#include "core/config.h"

namespace MarketMaker {

// Initialize static exchange endpoints database
std::map<std::string, ExchangeEndpoints> Config::EXCHANGE_ENDPOINTS = {
    {"binance", {
        "wss://stream.binance.com:9443/ws",             // Production WebSocket
        "https://api.binance.com",                      // Production REST
        "wss://stream.testnet.binance.vision:9443/ws",  // Testnet WebSocket
        "https://testnet.binance.vision"                // Testnet REST
    }}
};

} // namespace MarketMaker