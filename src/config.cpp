#include "config.h"

namespace MarketMaker {

// Initialize static exchange endpoints database
std::map<std::string, ExchangeEndpoints> Config::EXCHANGE_ENDPOINTS = {
    {"binance", {
        "wss://stream.binance.com:9443/ws",           // Production WebSocket
        "https://api.binance.com",                    // Production REST
        "wss://stream.testnet.binance.vision:9443/ws",  // Testnet WebSocket
        "https://testnet.binance.vision"              // Testnet REST
    }},
    {"coinbase", {
        "wss://ws-feed.exchange.coinbase.com",        // Production WebSocket
        "https://api.exchange.coinbase.com",          // Production REST
        "wss://ws-feed-public.sandbox.exchange.coinbase.com",  // Sandbox WebSocket
        "https://api-public.sandbox.exchange.coinbase.com"     // Sandbox REST
    }},
    {"kraken", {
        "wss://ws.kraken.com",                        // Production WebSocket
        "https://api.kraken.com",                     // Production REST
        "wss://ws-sandbox.kraken.com",                // Sandbox WebSocket
        "https://api-sandbox.kraken.com"              // Sandbox REST
    }},
    {"bybit", {
        "wss://stream.bybit.com/v5/public/spot",      // Production WebSocket
        "https://api.bybit.com",                      // Production REST
        "wss://stream-testnet.bybit.com/v5/public/spot",  // Testnet WebSocket
        "https://api-testnet.bybit.com"               // Testnet REST
    }},
    {"okx", {
        "wss://ws.okx.com:8443/ws/v5/public",         // Production WebSocket
        "https://www.okx.com",                        // Production REST
        "wss://wspap.okx.com:8443/ws/v5/public",      // Demo WebSocket
        "https://www.okx.com"                         // Demo REST (same as prod)
    }},
    {"kucoin", {
        "wss://ws-api-spot.kucoin.com",               // Production WebSocket
        "https://api.kucoin.com",                     // Production REST
        "wss://ws-api-spot-sandbox.kucoin.com",       // Sandbox WebSocket
        "https://openapi-sandbox.kucoin.com"          // Sandbox REST
    }},
    {"bitget", {
        "wss://ws.bitget.com/v2/ws/public",           // Production WebSocket
        "https://api.bitget.com",                     // Production REST
        "wss://ws.bitget.com/v2/ws/public",           // No separate testnet
        "https://api.bitget.com"                      // No separate testnet
    }}
};

} // namespace MarketMaker