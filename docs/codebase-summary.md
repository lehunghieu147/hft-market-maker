# Codebase Summary

## Project Stats
- **Language**: C++17
- **Build**: CMake 3.10+ with FetchContent
- **Source files**: 19 (.h) + 15 (.cpp)
- **Dependencies**: OpenSSL, CURL, JsonCpp, Asio, WebSocket++

## File Map

### Core (`include/core/`, `src/core/`)
| File | Lines | Purpose |
|------|-------|---------|
| `config.h` | ~97 | Config struct, ExchangeEndpoints, exchange URL database |
| `config_loader.h/.cpp` | ~274 | JSON config loading, env var merge, validation |
| `types.h` | ~116 | OrderSide, OrderStatus, PriceLevel, OrderBook (VWAP, imbalance), Order, LatencyMetrics |
| `logger.h/.cpp` | ~50 | File + console logging with log levels |

### Exchange (`include/exchange/`, `src/exchange/`)
| File | Lines | Purpose |
|------|-------|---------|
| `exchange_interface.h` | ~148 | IExchange abstract interface, ExchangeConfig |
| `exchange_factory.h/.cpp` | ~40 | Factory pattern for exchange creation |
| `binance_exchange.h/.cpp` | ~400 | Binance implementation (REST + WS + WS Trading) |

### Network (`include/network/`, `src/network/`)
| File | Lines | Purpose |
|------|-------|---------|
| `rest_client.h/.cpp` | ~300 | CURL-based REST client, HMAC signing, connection pool |
| `websocket_client.h/.cpp` | ~350 | Market data WebSocket (orderbook streaming) |
| `websocket_trading_client.h/.cpp` | ~300 | Order execution via Binance WS API v3 |
| `websocket_trading_adapter.h/.cpp` | ~200 | Adapter bridging WS trading to IExchange |
| `user_data_stream.h/.cpp` | ~400 | Binance User Data Stream (fills, balances) |

### Trading (`include/trading/`, `src/trading/`)
| File | Lines | Purpose |
|------|-------|---------|
| `market_maker.h/.cpp` | ~360 | Main bot: init, run, stop, VWAP mid, volatility |
| `order_manager.h/.cpp` | ~480 | Order lifecycle, fill events, cancel-and-replace |
| `order_validator.h/.cpp` | ~80 | Pre-trade validation (crossed orders, sanity) |
| `risk_manager.h/.cpp` | ~55 | Kill switch, error tracking, pre-trade gate |
| `position_tracker.h/.cpp` | ~78 | Net position tracking with configurable limits |
| `pnl_tracker.h/.cpp` | ~90 | Realized P&L, daily loss, drawdown, signed fees |
| `volatility_tracker.h/.cpp` | ~90 | Rolling stddev (Welford), spread adjustment |
| `rate_limiter.h/.cpp` | ~60 | Token bucket rate limiting |

## Build Command
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Key Design Decisions

1. **Exchange abstraction via IExchange**: Allows adding new exchanges without modifying trading logic
2. **Real fills over approximation**: UserDataStream tracks actual fills instead of assuming fills on placement
3. **Welford's algorithm**: Numerically stable variance calculation for high-priced assets (avoids catastrophic cancellation)
4. **VWAP mid price**: Volume-weighted mid from top N orderbook levels provides better price signal than simple mid
5. **Atomic pair position check**: `can_place_pair()` checks both bid+ask position impact under single lock (TOCTOU fix)
6. **Signed fee rates**: Maker fee can be negative (rebate), correctly reduces trading cost
7. **Thread join before SSL free**: Prevents use-after-free in UserDataStream cleanup
8. **SSL certificate verification**: VERIFY_PEER enabled for User Data Stream connections

## Improvement History

| Phase | Commit | Description |
|-------|--------|-------------|
| 01 | `fcfd2dc` | 10 critical bug fixes (atomics, race conditions, validation) |
| 02 | `fb0eff7` | Security hardening across network layer |
| 03 | `4f0d351` | Risk management framework (position, P&L, kill switch) |
| 04 | `ff4c88d` | Trading logic (UserDataStream, volatility, VWAP, fill tracking) |
| 05 | — | Documentation and final polish |
