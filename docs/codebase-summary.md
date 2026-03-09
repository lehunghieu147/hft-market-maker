# Codebase Summary

## Project Stats
- **Language**: C++17
- **Build**: CMake 3.14+ with FetchContent, three-target build (market_maker + momentum_taker + backtest)
- **Source files**: 32 (.h) + 28 (.cpp) [Phase B additions: 7 backtesting files, 3 strategy files]
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
| `websocket_trading_client.h/.cpp` | ~420 | Order execution + user data stream via Binance WS API v3; `userDataStream.subscribe.signature` |
| `websocket_trading_adapter.h/.cpp` | ~200 | Adapter bridging WS trading to IExchange |

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
| `avellaneda-stoikov-model.h` | ~79 | Inventory-aware quoting: reservation price, optimal spread computation |
| `orderbook-imbalance-tracker.h/.cpp` | ~43 | OBI detection: (bid_vol - ask_vol) / total_vol, EMA smoothing, spread tilting |
| `vwap-tracker.h` | ~80 | Cumulative VWAP with Welford variance, rolling bands |

### Backtesting (`include/backtesting/`, `src/backtesting/`)
| File | Lines | Purpose |
|------|-------|---------|
| `backtest-engine.h/.cpp` | ~46 | Tick-level replay engine, strategy callback, CSV export |
| `data-loader.h/.cpp` | ~50 | CSV tick parser: timestamp, OHLCV, L2 book (bids/asks) |
| `simulated-exchange.h/.cpp` | ~180 | Simulated orderbook, order matching, latency simulation, fill tracking |
| `performance-metrics.h` | ~100 | Sharpe ratio, max drawdown, win rate, trade stats |

## Build Command
```bash
cmake -S . -B out -DCMAKE_BUILD_TYPE=Release
cmake --build out -j$(nproc)
# Produces: out/market_maker, out/momentum_taker, out/backtest
```

## Key Design Decisions

1. **Three-target build**: Shared COMMON_SOURCES (~26 files), separate main.cpp / momentum_main.cpp / backtest_main.cpp
2. **Exchange abstraction via IExchange**: Allows adding new exchanges without modifying trading logic
3. **Integrated user data stream**: WebSocket Trading API handles both order execution and user data (fill/balance events) on same connection via `userDataStream.subscribe.signature`
4. **Real fills over approximation**: Receives actual fills from `executionReport` events instead of assuming fills on placement
5. **No separate keepalive thread**: WS-level ping/pong (every 20s) maintains user data subscription automatically
6. **Welford's algorithm**: Numerically stable variance calculation for high-priced assets (avoids catastrophic cancellation)
7. **VWAP mid price**: Volume-weighted mid from top N orderbook levels provides better price signal than simple mid
8. **Atomic pair position check**: `can_place_pair()` checks both bid+ask position impact under single lock (TOCTOU fix)
9. **Signed fee rates**: Maker fee can be negative (rebate), correctly reduces trading cost
10. **SSL certificate verification**: VERIFY_PEER enabled for WebSocket Trading API connections
11. **Quill async logger**: Lock-free ring buffer provides ~1-5µs log latency vs ~10-50µs for std::cout, zero blocking on call-site
12. **Named loggers per domain**: "trading", "network", "core", "risk" enable selective debugging and monitoring
13. **EMA(400) momentum strategy**: Fast signal detection, epsilon-based thresholds, cooldown prevents over-trading
14. **IOC limit orders**: Taker bot uses immediate-or-cancel orders for momentum execution with price protection
15. **Avellaneda-Stoikov model**: Inventory-aware quoting adjusts spread and reservation price based on position and volatility
16. **Order Book Imbalance (OBI)**: Detects directional pressure, tilts spread asymmetrically (compress bid if buy pressure, compress ask if sell pressure)
17. **Dynamic position sizing**: Scales order size inversely with volatility (larger in calm, smaller in turbulent markets)
18. **Multi-timeframe momentum**: Fast/slow EMA confirmation requires both to align before signal fires
19. **Backtesting framework**: Tick-level replay via SimulatedExchange with realistic latency/slippage simulation

## Improvement History

| Phase | Commit | Description |
|-------|--------|-------------|
| A1 | `fcfd2dc` | 10 critical bug fixes (atomics, race conditions, validation) |
| A2 | `fb0eff7` | Security hardening across network layer |
| A3 | `4f0d351` | Risk management framework (position, P&L, kill switch) |
| A4 | `ff4c88d` | Trading logic (UserDataStream, volatility, VWAP, fill tracking) |
| A5 | — | Documentation and final polish |
| A6 | `92ceabd` | Migrate logging: std::cout/cerr → Quill v7.5.0 async logger (16 files) |
| A7 | `d6514ad` | Momentum taker bot: EMA signal engine, IOC orders, latency tracking |
| B1 | `d6514ad` | Avellaneda-Stoikov inventory-aware quoting model |
| B2 | `d6514ad` | Multi-timeframe momentum: fast/slow EMA + VWAP tracker |
| B3 | `d6514ad` | Order Book Imbalance detection + spread tilting |
| B4 | `d6514ad` | Volatility-adjusted position sizing + dynamic limits |
| B5 | `d6514ad` | Backtesting framework + data loader + performance metrics |
