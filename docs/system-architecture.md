# System Architecture

## Overview

C++17 HFT trading system for Binance with multi-exchange abstraction. Contains three trading strategies:
1. **Market Maker Bot**: Places simultaneous BID/ASK limit orders around VWAP mid-price, adjusts spreads by volatility, optionally using Avellaneda-Stoikov model and OBI-based tilting
2. **Momentum Taker Bot**: EMA(400)-based momentum detection with optional multi-timeframe confirmation, executes IOC limit orders on threshold crossings
3. **Backtesting Engine**: Tick-level replay framework for strategy validation with realistic latency simulation

## Component Diagram

### Market Maker Bot (Enhanced - Phase C)
```
┌──────────────────────────────────────────────────────────────┐
│                      MarketMakerBot                          │
│  - main_loop (price change -> order update)                  │
│  - VWAP mid price from orderbook depth                       │
│  - volatility feed to VolatilityTracker (dual-window regime) │
├───────────┬──────────┬────────────┬──────────┬───────────────┤
│ Order     │ Risk     │Volatility  │  VWAP   │WebSocket      │
│Manager    │Manager   │ Tracker    │ Tracker │Trading API    │
│(ThreadPool)│         │(Welford's) │(Welford)│(Order+User)   │
├───────────┼──────────┼────────────┼──────────┼───────────────┤
│           │          │ Fast EMA   │Welford  │execution      │
│Inventory  │Position  │ Slow EMA   │+ stdev  │Report →       │
│Skew       │Tracker   │ Regime     │+ bands  │on_fill_event()│
│(if en)    │(Per-side │ Detection  │         │outboundAcct   │
│           │limits)   │            │         │WS ping (20s)  │
│Avellan.   │├─────────┤            │         │               │
│eda-       ││PnL Trk  │            │         │               │
│Stoikov    │├────────┤├────────────┼──────────┤               │
│(GLFT opt) ││Drawdown│ Toxic Flow│ Time-Day │               │
│           ││Spread  │ Detection  │ Rules &  │               │
│           ││WidenM. │ (if en)    │ ToD Mux  │               │
│OBI Tilt   ││        ├────────────┼──────────┤               │
│(if en)    ││        │Multi-Level │Spread    │               │
│           ││        │Quoting     │Stacking  │               │
│Dyn Sizing ││        │(if N>1)    │(5x cap)  │               │
│(if en)    │└────────┴────────────┴──────────┘               │
└───────────┴──────────────────────────────────────────────────┘

### Momentum Taker Bot (Enhanced)
┌─────────────────────────────────────────────────────────────┐
│                   MomentumTakerBot                          │
│  - main_loop (signal -> IOC order execution)                │
│  - Signal detection: EMA(400) + epsilon thresholds          │
│  - Multi-timeframe confirmation (if enabled)                │
│  - Cooldown enforcement (prevents over-trading)             │
├──────────┬──────────┬──────────────┬────────────────────────┤
│ Signal   │  Order   │  Risk        │  WebSocket Trading API │
│ Engine   │  Manager │  Manager     │  (Order + User Data)  │
├──────────┼──────────┼──────────────┼────────────────────────┤
│ EMA(400) │ place_   │ Position     │executionReport →      │
│ Fast EMA │ taker_   │ Tracker      │on_fill_event()       │
│(opt)     │ order()  │            │ outboundAccountPos    │
│ Slow EMA │          │ PnL Tracker  │WS ping/pong (20s)     │
│(opt)     │          │            │                        │
│ Epsilon+ │          │ Dynamic      │                        │
│ Cooldown │          │ Sizing       │                        │
│ Hysteresi│          │ (vol-based)  │                        │
│ Latency  │          │            │                        │
│ Tracker  │          │            │                        │
└──────────┴──────────┴──────────────┴────────────────────────┘

### Backtesting Framework
```
┌─────────────────────────────────────────────────────────────┐
│                     BacktestEngine                          │
│  - Tick-level replay from CSV (timestamp, OHLCV, L2)       │
│  - Strategy callback receives OrderBook + SimulatedExchange │
│  - Latency injection: network_delay_ms config               │
├─────────────────────────────────────────────────────────────┤
│ DataLoader              SimulatedExchange                    │
│ - CSV tick parser       - Synthetic orderbook               │
│ - OHLCV extraction      - Order matching (bid/ask)          │
│ - L2 book construction  - Fill latency simulation           │
│                         - Balance tracking                  │
├─────────────────────────────────────────────────────────────┤
│ PerformanceMetrics                                          │
│ - Sharpe ratio, max drawdown, win rate                      │
│ - Trade list, cumulative PnL curve                          │
│ - Export to CSV for external analysis                       │
└─────────────────────────────────────────────────────────────┘
```

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
| Thread pool | Order cancel/place operations with batch `cancelReplace` (replaces std::async) |
| Quill backend | Async logging I/O (lock-free, ~1-5µs per call) |

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
- Thread-safe SSL cleanup: socket shutdown → thread join → SSL_free (prevents heap-use-after-free)
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

## Configuration Management

### Phase B Advanced Strategy Configuration

#### Avellaneda-Stoikov Model (B1)
```yaml
strategy:
  use_avellaneda_stoikov: true      # Enable inventory-aware quoting
  as_gamma: 0.001                   # Risk aversion (higher = wider spreads)
  as_kappa: 1.5                     # Order arrival intensity (higher = tighter)
  as_time_horizon_sec: 300.0        # Rolling time window for spread narrowing
```

#### Multi-Timeframe Momentum (B2)
```yaml
strategy:
  use_multi_timeframe: true         # Require both EMAs to confirm
  fast_ema_window: 8                # Fast EMA period
  slow_ema_window: 50               # Slow EMA period
  volume_expansion_threshold: 1.2   # Volume must be 1.2x avg to confirm
```

#### Order Book Imbalance Tilting (B3)
```yaml
strategy:
  use_obi_tilt: true                # Enable OBI-based spread tilting
  obi_levels: 5                     # Orderbook depth for OBI calculation
  obi_tilt_factor: 0.3              # Max tilt as % of spread (30%)
  obi_min_volume: 50.0              # Min volume for signal validity
```

#### Dynamic Position Sizing (B4)
```yaml
strategy:
  use_dynamic_sizing: true          # Scale order size by volatility
  vol_sizing_exponent: 0.5          # Power for scaling (0.5 = sqrt)
  min_size_multiplier: 0.5          # Min size relative to base
  max_size_multiplier: 2.0          # Max size relative to base
```

### Phase C Enhanced Strategy Configuration

#### Inventory Skew (Non-AS Alternative)
```yaml
strategy:
  inventory_skew_factor: 0.2        # Mean-revert position (0 = disabled, 0.1-0.5 typical)
```

#### Multi-Level Quoting
```yaml
strategy:
  num_quote_levels: 3               # Number of levels per side (1 = single bid/ask)
  level_spacing_multiplier: 1.5     # Each level spread *= this^level
  level_size_decay: 0.5             # Each level size *= this^level
```

#### Toxic Flow Detection
```yaml
strategy:
  use_toxic_flow_detection: true    # Widen spread on one-sided fills
  toxic_flow_window: 50             # Rolling window of recent fills
  toxic_flow_threshold: 0.7         # Trigger when one-sided % >= 70%
  toxic_flow_spread_mult: 1.5       # Spread multiplier when triggered
```

#### GLFT Extension
```yaml
strategy:
  use_glft: true                    # GLFT adds inventory penalty near position limits
```

#### Dual-Window Volatility Regime Detection
```yaml
strategy:
  vol_fast_window: 20               # Fast volatility window
  vol_regime_threshold: 2.0         # When vol_fast/vol_slow >= 2.0, apply mult
  vol_regime_spread_mult: 2.0       # Spread multiplier during high-vol regime
```

#### Per-Side Position Limits (Asymmetric)
```yaml
risk:
  max_long_position: 1.0            # Max long position (0 = symmetric to max_position_size)
  max_short_position: 0.5           # Max short position (e.g., more conservative on shorts)
```

#### Drawdown-Based Spread Widening
```yaml
risk:
  max_drawdown_spread_multiplier: 1.0  # At max_drawdown, spread *= (1 + 1.0) = 2x
```

#### Time-of-Day Spread Rules
```yaml
risk:
  time_of_day_rules:
    - start_hour_utc: 8
      end_hour_utc: 14
      spread_multiplier: 1.0        # Tight spreads during peak hours
    - start_hour_utc: 14
      end_hour_utc: 20
      spread_multiplier: 1.5        # Wider spreads in evening
```

#### Spread Multiplier Stacking (5x Hard Cap)
```
final_spread = base_spread * min(5.0,
  toxic_mult * drawdown_mult * regime_mult * time_mult)
```

### Precision Settings
- **`price_precision`** (default: 2): Decimal places for order prices
- **`quantity_precision`** (default: 6): Decimal places for order quantities

### Full Config Structure Example
```yaml
trading:
  symbol: "DOGEEUSDT"
  order_size: 100
  spread_percentage: 0.1
  price_precision: 4
  quantity_precision: 1
  # Phase B Strategy Features
  use_avellaneda_stoikov: true
  as_gamma: 0.001
  use_obi_tilt: true
  obi_levels: 5
  use_dynamic_sizing: true
  vol_sizing_exponent: 0.5
```

## Exchange Abstraction
- `IExchange` abstract interface defines all exchange operations
- `ExchangeFactory::create()` instantiates exchange by name
- `ExchangeConfig` carries exchange-specific parameters
- Currently implemented: Binance (REST + WebSocket + WS Trading API)
- Adding new exchange: implement IExchange, register in factory
