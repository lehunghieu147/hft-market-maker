# System Architecture

## Overview

C++17 HFT trading system for Binance with multi-exchange abstraction. Contains two trading strategies:
1. **Market Maker Bot**: Places simultaneous BID/ASK limit orders around VWAP mid-price, adjusts spreads by volatility
2. **Momentum Taker Bot**: EMA(400)-based momentum detection, executes IOC limit orders on threshold crossings

## Component Diagram

### Market Maker Bot
```
┌─────────────────────────────────────────────────────────────┐
│                      MarketMakerBot                         │
│  - main_loop (price change -> order update)                 │
│  - VWAP mid price from orderbook depth                      │
│  - volatility feed to VolatilityTracker                     │
├──────────┬──────────┬──────────────┬────────────────────────┤
│          │          │              │                        │
│  Order   │  Risk    │  Volatility  │  WebSocket Trading API │
│  Manager │  Manager │  Tracker     │  (Order + User Data)  │
│          │          │              │                        │
│  ┌───────┤  ┌───────┤  Welford's   │  executionReport →    │
│  │Validator  │Position  online alg  │  on_fill_event()     │
│  │       │  │Tracker │              │  outboundAccountPos  │
│  │       │  │       │              │  WS ping/pong (20s)   │
│  │       │  ├───────┤              │                        │
│  │       │  │PnL    │              │                        │
│  │       │  │Tracker│              │                        │
└──┴───────┴──┴───────┴──────────────┴────────────────────────┘

### Momentum Taker Bot
┌─────────────────────────────────────────────────────────────┐
│                   MomentumTakerBot                          │
│  - main_loop (signal -> IOC order execution)                │
│  - Signal detection: EMA(400) + epsilon thresholds          │
│  - Cooldown enforcement (prevents over-trading)             │
├──────────┬──────────┬──────────────┬────────────────────────┤
│          │          │              │                        │
│  Signal  │  Order   │  Risk        │  WebSocket Trading API │
│  Engine  │  Manager │  Manager     │  (Order + User Data)  │
│          │          │              │                        │
│  ┌───────┤  place_  │  ┌───────┐   │  executionReport →    │
│  │EMA(400)  taker_  │  │Position│  │  on_fill_event()     │
│  │       │  order() │  │Tracker │  │  outboundAccountPos  │
│  │epsilon│          │  ├───────┤   │  WS ping/pong (20s)   │
│  │cooldown          │  │PnL    │   │                        │
│  │       │          │  │Tracker│   │                        │
│  │Latency│          │  │       │   │                        │
│  │Tracker│          │  │       │   │                        │
└──┴───────┴──────────┴──┴───────┴───┴────────────────────────┘

### Shared Infrastructure
┌─────────────────────────────────────────────────────────────┐
│                   AppLogger (Quill v7.5.0)                  │
│  AsyncLogger: ConsoleSink + RotatingFileSink               │
│  Named loggers: "trading", "network", "core", "risk"       │
│  Lock-free ring buffer (~1-5µs per call)                    │
├─────────────────────────────────────────────────────────────┤
│  IExchange Interface                                         │
│  connect/disconnect, place/cancel/modify orders             │
│  subscribe_orderbook, get_balance, get_order_status         │
│  place_market_order, place_ioc_order (taker bot support)   │
├─────────────────────────────────────────────────────────────┤
│  ExchangeFactory::create("binance") → BinanceExchange       │
├──────────┬──────────┬───────────────────────────────────────┤
│ RestClient│ WS Client│ WS Trading Client                    │
│ (CURL)   │(market)  │ (orders + user data via WS API)      │
│ HMAC-256 │ RFC 6455 │ HMAC-SHA256 per request              │
│ Market/  │          │ IOC support + fill/balance events    │
│ IOC      │          │                                       │
└──────────┴──────────┴───────────────────────────────────────┘
```

## Data Flow

### Order Lifecycle
1. WebSocket receives orderbook update → `handle_orderbook_update()`
2. VWAP mid price calculated from top 5 levels
3. Price change detected → `check_and_update_orders()`
4. Timestamp validation: reject data > 5s stale
5. Risk gate: `should_trade()` checks kill switch, errors, P&L limits
6. Order validation: crossed-order detection, price/qty sanity
7. Position check: `can_place_pair()` under single lock
8. Cancel existing orders (parallel async)
9. Place new BID/ASK orders (parallel threads)
10. WebSocket Trading API receives `executionReport` event → `on_fill_event()`
11. Position/PnL trackers updated from real fill data
12. WS-level ping/pong (every 20s) maintains user data subscription

### Momentum Strategy Signal Flow
1. WebSocket receives orderbook update → `handle_orderbook_update()`
2. Extract best bid/ask from L2 orderbook
3. `SignalEngine::on_tick(best_bid, best_ask)`:
   - Update EMA(400) with mid price: `(best_bid + best_ask) / 2.0`
   - Check BUY threshold: `best_ask > EMA * (1.0 + epsilon)` (momentum up)
   - Check SELL threshold: `best_bid < EMA * (1.0 - epsilon)` (momentum down)
   - Enforce cooldown: skip if last signal < cooldown period
   - Hysteresis: require price cross back to EMA before new signal
4. If signal fired → notify main loop via condition variable
5. Main loop validates signal is not stale (< 100ms old)
6. Risk gate: `should_trade()` checks position, P&L limits
7. Place IOC limit order via `OrderManager::place_taker_order()`
8. Record latency: signal detection → order placement time
9. WebSocket Trading API receives `executionReport` event → update position/PnL trackers

### Threading Model
| Thread | Purpose |
|--------|---------|
| Main thread | Bot initialization, signal handling |
| `main_thread_` | Trading loop (market maker: price change → order update; momentum: signal → order exec) |
| WebSocket thread | Market data reception (orderbook) |
| WS Trading thread | Order execution responses + user data events (single connection) |
| Async cancel/place | `std::thread` for parallel order ops (market maker only) |

### Synchronization
- `orderbook_mutex_`: Protects `current_orderbook_`, `last_orderbook_time_`
- `orders_mutex_`: Protects `active_bid_order_`, `active_ask_order_`
- `metrics_mutex_`: Protects `LatencyMetrics`
- `ssl_mutex_`: Protects SSL resources in UserDataStream
- `callback_mutex_`: Protects fill/balance callbacks
- `price_change_cv_`: Condition variable for immediate reaction to price changes
- Atomics: `current_mid_price_`, `price_changed_`, `running_`, `kill_switch_`

## Risk Management Pipeline

```
Pre-trade gate (ordered checks):
  1. kill_switch_ == false
  2. consecutive_errors_ < max_consecutive_errors
  3. daily_pnl_ >= max_daily_loss
  4. drawdown >= max_drawdown
  5. OrderValidator::validate_market_maker_orders()
  6. PositionTracker::can_place_pair(buy_qty, sell_qty)
```

## Security Measures
- SSL certificate verification (`SSL_VERIFY_PEER`)
- HMAC-SHA256 for all authenticated requests
- WebSocket payload size limit (16 MB)
- RFC 6455 compliant pong (echoes ping payload)
- Thread-safe SSL cleanup (join before free)
- API credentials loaded from config/env, never logged

## Logging Subsystem (Quill v7.5.0)

### Architecture
- **AppLogger singleton**: Manages Quill backend lifecycle (init → get → shutdown)
- **Async I/O**: Lock-free ring buffer ~1-5µs per log call (vs ~10-50µs for std::cout)
- **Named loggers**: "trading", "network", "core", "risk", "root" with consistent sinks
- **Dual sinks**: ConsoleSink (stdout) + RotatingFileSink (logs/market_maker.log)

### Rotation Policy
- **File**: 100MB max size + daily rotation at 00:00 UTC
- **Mode**: Append mode, auto-creates logs/ directory
- **Level**: Info by default (covers WARNING/ERROR/CRITICAL)

### Usage Pattern
```cpp
// Get a named logger (creates if not exists)
auto* logger = AppLogger::get("trading");
LOG_INFO(logger, "Placed BID order {} @ {}", order_id, price);
```

### Thread Safety
- Quill backend thread handles all I/O (non-blocking frontend)
- Frontend calls use lock-free atomic operations
- No blocking on log() call (completes in microseconds)

### Graceful Shutdown
```cpp
AppLogger::shutdown(); // Flushes all pending logs, stops backend
```

## Exchange Abstraction
- `IExchange` abstract interface defines all exchange operations
- `ExchangeFactory::create()` instantiates exchange by name
- `ExchangeConfig` carries exchange-specific parameters
- Currently implemented: Binance (REST + WebSocket + WS Trading API)
- Adding new exchange: implement IExchange, register in factory
