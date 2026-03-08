# Codebase Summary

## Project Stats
- **Language**: C++17
- **Build**: CMake 3.14+ with FetchContent, two-target build (market_maker + momentum_taker)
- **Source files**: 25 (.h) + 24 (.cpp)
- **Dependencies**: OpenSSL, CURL, JsonCpp, Asio, WebSocket++, Quill v7.5.0

## File Map

### Core (`include/core/`, `src/core/`)
| File | Lines | Purpose |
|------|-------|---------|
| `config.h` | ~130 | Config struct, MomentumConfig struct, ExchangeEndpoints, exchange URL database |
| `config_loader.h/.cpp` | ~320 | JSON config loading, env var merge, validation (both market maker & momentum configs) |
| `types.h` | ~116 | OrderSide, OrderStatus, PriceLevel, OrderBook (VWAP, imbalance), Order, LatencyMetrics |
| `app_logger.h/.cpp` | ~70 | AppLogger singleton, Quill init/get/shutdown, RotatingFileSink config |
| `logger.h/.cpp` | ~50 | Logger wrapper, delegates to Quill (legacy interface) |

### Exchange (`include/exchange/`, `src/exchange/`)
| File | Lines | Purpose |
|------|-------|---------|
| `exchange_interface.h` | ~155 | IExchange abstract interface, ExchangeConfig, place_market_order/place_ioc_order |
| `exchange_factory.h/.cpp` | ~40 | Factory pattern for exchange creation |
| `binance_exchange.h/.cpp` | ~450 | Binance implementation (REST + WS + WS Trading), market/IOC orders |

### Network (`include/network/`, `src/network/`)
| File | Lines | Purpose |
|------|-------|---------|
| `rest_client.h/.cpp` | ~340 | CURL-based REST client, HMAC signing, connection pool, market/IOC order support |
| `websocket_client.h/.cpp` | ~350 | Market data WebSocket (orderbook streaming) |
| `websocket_trading_client.h/.cpp` | ~300 | Order execution via Binance WS API v3 |
| `websocket_trading_adapter.h/.cpp` | ~200 | Adapter bridging WS trading to IExchange |
| `user_data_stream.h/.cpp` | ~400 | Binance User Data Stream (fills, balances) |

### Trading (`include/trading/`, `src/trading/`)
| File | Lines | Purpose |
|------|-------|---------|
| `market_maker.h/.cpp` | ~360 | Market maker bot: init, run, stop, VWAP mid, volatility |
| `momentum_taker.h/.cpp` | ~280 | Momentum taker bot: EMA-based signal execution, IOC orders |
| `ema_engine.h` | ~35 | Exponential moving average engine (header-only) |
| `signal_engine.h/.cpp` | ~100 | Momentum signal detection (EMA + epsilon thresholds, cooldown) |
| `latency_tracker.h/.cpp` | ~80 | Circular buffer latency tracker with percentiles |
| `order_manager.h/.cpp` | ~520 | Order lifecycle, fill events, cancel-and-replace, place_taker_order |
| `order_validator.h/.cpp` | ~80 | Pre-trade validation (crossed orders, sanity) |
| `risk_manager.h/.cpp` | ~55 | Kill switch, error tracking, pre-trade gate |
| `position_tracker.h/.cpp` | ~78 | Net position tracking with configurable limits |
| `pnl_tracker.h/.cpp` | ~90 | Realized P&L, daily loss, drawdown, signed fees |
| `volatility_tracker.h/.cpp` | ~90 | Rolling stddev (Welford), spread adjustment |
| `rate_limiter.h/.cpp` | ~60 | Token bucket rate limiting |

## Build Command
```bash
cmake -S . -B out -DCMAKE_BUILD_TYPE=Release
cmake --build out -j$(nproc)
# Produces: out/market_maker and out/momentum_taker
```

## Key Design Decisions

1. **Two-target build**: Shared COMMON_SOURCES (16 files), separate main.cpp / momentum_main.cpp (market maker vs momentum taker)
2. **Exchange abstraction via IExchange**: Allows adding new exchanges without modifying trading logic
3. **Real fills over approximation**: UserDataStream tracks actual fills instead of assuming fills on placement
4. **Welford's algorithm**: Numerically stable variance calculation for high-priced assets (avoids catastrophic cancellation)
5. **VWAP mid price**: Volume-weighted mid from top N orderbook levels provides better price signal than simple mid
6. **Atomic pair position check**: `can_place_pair()` checks both bid+ask position impact under single lock (TOCTOU fix)
7. **Signed fee rates**: Maker fee can be negative (rebate), correctly reduces trading cost
8. **Thread join before SSL free**: Prevents use-after-free in UserDataStream cleanup
9. **SSL certificate verification**: VERIFY_PEER enabled for User Data Stream connections
10. **Quill async logger**: Lock-free ring buffer provides ~1-5µs log latency vs ~10-50µs for std::cout, zero blocking on call-site
11. **Named loggers per domain**: "trading", "network", "core", "risk" enable selective debugging and monitoring
12. **EMA(400) momentum strategy**: Fast signal detection, epsilon-based thresholds, cooldown prevents over-trading
13. **IOC limit orders**: Taker bot uses immediate-or-cancel orders for momentum execution with price protection

## Improvement History

| Phase | Commit | Description |
|-------|--------|-------------|
| 01 | `fcfd2dc` | 10 critical bug fixes (atomics, race conditions, validation) |
| 02 | `fb0eff7` | Security hardening across network layer |
| 03 | `4f0d351` | Risk management framework (position, P&L, kill switch) |
| 04 | `ff4c88d` | Trading logic (UserDataStream, volatility, VWAP, fill tracking) |
| 05 | — | Documentation and final polish |
| 06 | — | Migrate logging: std::cout/cerr → Quill v7.5.0 async logger (16 files) |
