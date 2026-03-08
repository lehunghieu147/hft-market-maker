# System Architecture

## Overview

C++17 HFT market maker bot for Binance with multi-exchange abstraction. Places simultaneous BID/ASK limit orders around a VWAP mid-price, adjusts spreads by volatility, and tracks positions from real fills.

## Component Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                      MarketMakerBot                         │
│  - main_loop (price change -> order update)                 │
│  - VWAP mid price from orderbook depth                      │
│  - volatility feed to VolatilityTracker                     │
├──────────┬──────────┬──────────────┬────────────────────────┤
│          │          │              │                        │
│  Order   │  Risk    │  Volatility  │  User Data Stream     │
│  Manager │  Manager │  Tracker     │  (Binance WebSocket)  │
│          │          │              │                        │
│  ┌───────┤  ┌───────┤  Welford's   │  executionReport →    │
│  │Validator  │Position  online alg  │  on_fill_event()     │
│  │       │  │Tracker │              │  outboundAccountPos  │
│  │       │  │       │              │  listen key keepalive │
│  │       │  ├───────┤              │                        │
│  │       │  │PnL    │              │                        │
│  │       │  │Tracker│              │                        │
└──┴───────┴──┴───────┴──────────────┴────────────────────────┘
           │
┌──────────▼──────────────────────────────────────────────────┐
│                    IExchange Interface                       │
│  connect/disconnect, place/cancel/modify orders             │
│  subscribe_orderbook, get_balance, get_order_status         │
├─────────────────────────────────────────────────────────────┤
│  ExchangeFactory::create("binance") → BinanceExchange       │
├──────────┬──────────┬───────────────────────────────────────┤
│ RestClient│ WS Client│ WS Trading Client                    │
│ (CURL)   │(market)  │ (orders via WS API)                  │
│ HMAC-256 │ RFC 6455 │ HMAC-SHA256 per request              │
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
10. UserDataStream receives `executionReport` → `on_fill_event()`
11. Position/PnL trackers updated from real fill data

### Threading Model
| Thread | Purpose |
|--------|---------|
| Main thread | Bot initialization, signal handling |
| `main_thread_` | Trading loop (price change → order update) |
| WebSocket thread | Market data reception (orderbook) |
| WS Trading thread | Order execution responses |
| `stream_thread_` | User Data Stream (fill events) |
| `keepalive_thread_` | Listen key keepalive (30 min interval) |
| Async cancel/place | `std::thread` for parallel order ops |

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

## Exchange Abstraction
- `IExchange` abstract interface defines all exchange operations
- `ExchangeFactory::create()` instantiates exchange by name
- `ExchangeConfig` carries exchange-specific parameters
- Currently implemented: Binance (REST + WebSocket + WS Trading API)
- Adding new exchange: implement IExchange, register in factory
