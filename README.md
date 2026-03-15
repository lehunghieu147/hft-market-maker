# High-Frequency Trading Market Maker Bot

A high-performance, low-latency market maker bot for cryptocurrency trading on Binance exchange, implemented in C++17.

## System Overview

This market maker bot implements a complete trading system with:

- **Real-time market data** via WebSocket connections
- **Low-latency order execution** using Binance WebSocket Trading API
- **Risk management** with position tracking, P&L monitoring, and kill switch
- **Real-time fill tracking** via Binance User Data Stream
- **Volatility-adjusted spreads** using Welford's online algorithm
- **VWAP mid pricing** from orderbook depth analysis
- **Order validation** with crossed-order and self-trade prevention
- **Automatic reconnection** with exponential backoff
- **Multi-exchange architecture** (currently Binance)

### Target Performance

- **Reaction Latency**: < 50ms (orderbook update -> order placement)

## Architecture

```
                    +--------------------+
                    |  MarketMakerBot    |
                    +--------+-----------+
                             |
          +------------------+------------------+
          |                  |                  |
+---------v------+  +--------v--------+  +-----v-----------+
| OrderManager   |  | RiskManager     |  | WS Trading API  |
| - place orders |  | - kill switch   |  | - user data sub |
| - cancel/replace| | - error tracking|  | - real fills    |
| - fill events  |  +--------+--------+  | - balance events|
+----------------+           |            +-----------------+
                    +--------+--------+
                    |                 |
              +-----v-----+   +------v------+
              | Position   |   | PnL         |
              | Tracker    |   | Tracker     |
              | - net pos  |   | - daily P&L |
              | - limits   |   | - drawdown  |
              +-----------+   | - fees      |
                              +-------------+
```

### Core Components

| Component | File | Purpose |
|-----------|------|---------|
| MarketMakerBot | `trading/market_maker` | Main bot orchestration, VWAP mid price, volatility feed |
| OrderManager | `trading/order_manager` | Order lifecycle, fill events, timestamp validation |
| OrderValidator | `trading/order_validator` | Pre-trade validation, crossed-order detection |
| RiskManager | `trading/risk_manager` | Kill switch, error tracking, pre-trade risk gate |
| PositionTracker | `trading/position_tracker` | Net position tracking with configurable limits |
| PnLTracker | `trading/pnl_tracker` | Realized P&L, daily loss, drawdown, fee tracking |
| VolatilityTracker | `trading/volatility_tracker` | Rolling stddev via Welford's algorithm |
| WebSocketTradingClient | `network/websocket_trading_client` | Order execution + user data stream (single WS connection) |
| RateLimiter | `trading/rate_limiter` | Exchange rate limit compliance |
| WebSocketClient | `network/websocket_client` | Market data WebSocket |
| RestClient | `network/rest_client` | REST API with connection pooling |

### Directory Structure

```
include/
├── core/          # Config, types, logger
├── exchange/      # Exchange interface, factory, Binance impl
├── network/       # REST, WebSocket, User Data Stream clients
└── trading/       # Order management, risk, position, P&L, volatility
src/
├── core/          # Config loader, logger implementation
├── exchange/      # Exchange factory, Binance exchange
├── network/       # Network client implementations
└── trading/       # Trading logic implementations
config/            # JSON configuration files
```

## Key Features

### Risk Management
- **Position limits**: Configurable max position size with per-side asymmetric limits (`max_long_position`, `max_short_position`)
- **Daily loss limit**: Automatic trading halt when daily P&L breaches threshold
- **Max drawdown**: Peak-to-trough drawdown monitoring with drawdown-aware spread widening
- **Kill switch**: Emergency stop with manual reset
- **Consecutive error tracking**: Auto-halt after N consecutive failures
- **Fee-aware P&L**: Supports signed maker rebates (negative fees = rebate)
- **Toxic flow detection**: Widen spread when fills are one-sided (`use_toxic_flow_detection`)
- **Time-of-day rules**: Schedule-based spread multipliers for specific market hours

### Trading Strategy
- **Market making**: Simultaneous BID/ASK orders around VWAP mid-price
- **Volatility-adjusted spreads**: Spread scales with market volatility
- **Multi-level quoting**: Place N levels per side at staggered prices/sizes (`num_quote_levels`)
- **Inventory skew**: Bias pricing to revert position toward neutral (non-Avellaneda mode)
- **Orderbook depth analysis**: VWAP mid price from top N levels
- **Imbalance ratio**: Bid/ask volume imbalance detection with OBI-based spread tilting
- **Price change threshold**: Skip updates for insignificant price moves (< 0.01%)
- **Stale data rejection**: Reject orderbook updates older than 5 seconds
- **Volatility regime detection**: Dual-window volatility tracking for regime-aware spreads

### Real-Time Fill Tracking
- **Binance WS API User Data**: Integrated on same WebSocket connection as order execution
- **Method**: `userDataStream.subscribe.signature` with HMAC-SHA256 authentication
- **Accurate position tracking**: Based on real fills from `executionReport` events
- **Partial fill handling**: Tracks cumulative fill quantities
- **Balance monitoring**: Real-time `outboundAccountPosition` events
- **Automatic keepalive**: WS-level ping/pong (every 20s) maintains subscription
- **No separate connection**: User data events share the trading WebSocket (wss://ws-api.binance.com:443)

### Order Safety
- **Order validation**: Price, quantity, spread sanity checks before placement
- **Crossed-order prevention**: Detects bid >= ask before sending
- **Cancel-and-replace**: Checks order status if cancel fails (handles already-filled)
- **Parallel cancel/place**: Async order operations for minimal latency
- **Timestamp validation**: Rejects stale orderbook data

### Security
- **SSL/TLS**: All connections encrypted with certificate verification
- **HMAC-SHA256**: Request authentication for all signed endpoints
- **Payload size limits**: WebSocket frame cap at 16MB (OOM prevention)
- **No credential logging**: API keys never appear in logs
- **Resource cleanup**: Proper SSL/thread cleanup on all failure paths

## Prerequisites

### System Requirements
- **OS**: Linux (tested on Ubuntu 24.04)
- **Network**: Stable internet connection

### Dependencies
- **CMake** >= 3.10
- **C++ Compiler** with C++17 support (GCC 7+, Clang 5+)
- **OpenSSL** (libssl-dev)
- **CURL** (libcurl4-openssl-dev)

Auto-fetched by CMake:
- **JsonCpp** 1.9.5
- **Asio** 1.28.0 (standalone)
- **WebSocket++** 0.8.2

## Installation

### 1. Install Dependencies

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libssl-dev libcurl4-openssl-dev git
```

**macOS:**
```bash
brew install cmake openssl curl
```

### 2. Clone & Build

```bash
git clone <repository-url>
cd hft-market-maker

# Configure and build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The executable will be at: `build/bin/market_maker`

## Configuration

### Configuration File

The bot uses JSON configuration files in `config/` directory:

```json
{
    "api": {
        "key": "YOUR_API_KEY",
        "secret": "YOUR_API_SECRET"
    },
    "trading": {
        "symbol": "BTCUSDT",
        "order_size": 0.001,
        "spread_percentage": 0.02,
        "base_asset": "BTC",
        "quote_asset": "USDT",
        "display_assets": ["USDT", "BTC"],
        "supported_quote_currencies": ["USDT", "BUSD", "ETH", "BNB"]
    },
    "exchange": {
        "name": "binance",
        "ws_url": "wss://stream.binance.com:9443/ws",
        "rest_url": "https://api.binance.com",
        "ws_trading_url": "wss://ws-api.binance.com:443",
        "use_websocket_trading": true,
        "testnet": false
    },
    "risk": {
        "max_daily_loss": -100.0,
        "max_position_size": 0.5,
        "max_drawdown": -500.0,
        "max_consecutive_errors": 5,
        "maker_fee_rate": -0.0001,
        "taker_fee_rate": 0.001
    },
    "performance": {
        "order_update_cooldown_ms": 500,
        "reconnect_delay_ms": 5000,
        "max_reconnect_attempts": 10,
        "max_orders_per_second": 2
    }
}
```

### Risk Configuration

| Parameter | Default | Description |
|-----------|---------|-------------|
| `max_daily_loss` | -100.0 | Daily loss threshold (negative USDT). Trading halts when breached |
| `max_position_size` | 0.5 | Maximum absolute net position (base currency) |
| `max_long_position` | 0.0 | Max long position (0 = use symmetric `max_position_size` for both sides) |
| `max_short_position` | 0.0 | Max short position (0 = use symmetric `max_position_size` for both sides) |
| `max_drawdown` | -500.0 | Peak-to-trough drawdown limit (negative USDT) |
| `max_drawdown_spread_multiplier` | 0.0 | Spread multiplier at max drawdown (0 = disabled). At breach, spread *= (1 + value) |
| `max_consecutive_errors` | 5 | Kill switch triggers after N consecutive order failures |
| `maker_fee_rate` | -0.0001 | Maker fee rate (negative = rebate). Binance VIP0: -0.01% |
| `taker_fee_rate` | 0.001 | Taker fee rate. Binance VIP0: 0.1% |

### Environment Variables

Override config values with environment variables:

```bash
export BINANCE_API_KEY="your_api_key"
export BINANCE_API_SECRET="your_api_secret"
export SYMBOL="BTCUSDT"
export ORDER_SIZE="0.001"
export SPREAD_PERCENTAGE="0.02"
```

## Running

```bash
# Run with config file
./build/bin/market_maker config/config.json

# Run in background
nohup ./build/bin/market_maker config/config.json > output.log 2>&1 &

# Stop: Ctrl+C (graceful shutdown)
```

## Advanced Strategy Configuration

### Multi-Level Quoting
Place multiple bid/ask levels at staggered prices to capture different price levels.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `num_quote_levels` | 1 | Number of quote levels per side (1 = standard single pair) |
| `level_spacing_multiplier` | 1.5 | Each level spreads *= this^level (higher = wider spacing) |
| `level_size_decay` | 0.5 | Each level size *= this^level (higher = more aggressive) |

**Example**: With 3 levels, spacing=1.5, decay=0.5:
- Level 1: spread=0.02%, size=0.001
- Level 2: spread=0.03%, size=0.0005
- Level 3: spread=0.045%, size=0.00025

### Inventory Skew (Non-AS Mode)
Mean-revert position toward neutral by biasing bid/ask prices (alternative to Avellaneda-Stoikov).

| Parameter | Default | Description |
|-----------|---------|-------------|
| `inventory_skew_factor` | 0.0 | Skew magnitude (0 = disabled, typical: 0.1-0.5) |

When long, bid spread is reduced and ask spread is increased to encourage selling. Aggressive mean-reversion at factor=0.5.

### Toxic Flow Detection
Widen spread when recent fills are heavily one-sided (indicates adverse flow).

| Parameter | Default | Description |
|-----------|---------|-------------|
| `use_toxic_flow_detection` | false | Enable toxic flow detection |
| `toxic_flow_window` | 50 | Rolling window of recent fills |
| `toxic_flow_threshold` | 0.7 | Trigger when one-sided % >= threshold (0.7 = 70%) |
| `toxic_flow_spread_mult` | 1.5 | Spread multiplier when triggered |

### Time-of-Day Rules
Apply different spread multipliers during specific market hours.

```json
"time_of_day_rules": [
  {"start_hour_utc": 8, "end_hour_utc": 14, "spread_multiplier": 1.0},   // Peak hours, tight spreads
  {"start_hour_utc": 14, "end_hour_utc": 20, "spread_multiplier": 1.5},  // Evening, wider spreads
  {"start_hour_utc": 20, "end_hour_utc": 8, "spread_multiplier": 2.0}    // Night, conservative
]
```

### Dual-Window Volatility Regime
Detect high-volatility regimes by comparing fast vs slow volatility windows.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `vol_fast_window` | 20 | Fast volatility window (number of ticks) |
| `vol_regime_threshold` | 2.0 | When `vol_fast / vol_slow >= threshold`, apply regime multiplier |
| `vol_regime_spread_mult` | 2.0 | Spread multiplier during high-vol regime |

### Spread Multiplier Stacking
Multiple multipliers (toxic flow, drawdown, regime, time-of-day) are stacked with a **5x hard cap** to prevent excessive widening:

`final_spread = base_spread * min(5.0, toxic_mult * drawdown_mult * regime_mult * time_mult)`

## Momentum Taker Bot

A signal-driven taker bot that detects EMA crossover momentum and fires IOC/market orders.

### Running

```bash
# Build (same as market maker)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run with momentum config
./build/bin/momentum_taker config/config.momentum.json
```

### Momentum Configuration

| Parameter | Default | Description |
|-----------|---------|-------------|
| `epsilon` | 0.0002 | Signal threshold: fires when `\|mid - ema\| / ema > epsilon` |
| `ema_window` | 400 | EMA period (number of ticks) |
| `cooldown_ms` | 500 | Minimum ms between signals |
| `max_position` | 10.0 | Max absolute position size |
| `order_size` | 0.001 | Order quantity per signal |
| `order_type` | "ioc" | `"ioc"` or `"market"` |
| `min_profit_bps` | 0.0 | Min profit above cost (basis points of mid price). 0 = disabled |

### Epsilon Tuning Guide

Epsilon controls signal sensitivity. Too tight = over-signaling in volatile markets. Too wide = missed opportunities.

**Rule of thumb:** `epsilon >= spread + taker_fee` for signals to be profitable.

| Symbol | Typical Spread | Suggested Epsilon Range |
|--------|---------------|------------------------|
| BTCUSDT | 0.01-0.05% | 0.0002-0.0005 |
| ETHUSDT | 0.02-0.08% | 0.0003-0.0008 |
| SOLUSDT | 0.03-0.10% | 0.0005-0.001 |

### Cost Gate (`min_profit_bps`)

Signals are rejected when: `|mid - ema| < spread + taker_fee * mid + min_profit_bps * mid`

This prevents executing signals where the edge doesn't cover trading costs. Set `min_profit_bps: 0` to disable (backward compatible). A typical value of `0.0001` (1 bps) adds a small profit buffer above break-even.

## Performance Metrics

The bot tracks and reports every 30 seconds:

- **Reaction Latency**: Time from orderbook update to order placement
  - P50 / P95 / P99 percentiles (target: P50 < 50ms, P99 < 100ms)
- **Execution Latency**: Time to execute the order placement function
  - P50 / P95 / P99 percentiles
- **Order Success Rate**: Successful / total orders
- **Position**: Current net position in base currency
- **Daily P&L**: Realized profit/loss for the day
- **Total P&L**: Cumulative realized P&L
- **Fees Paid**: Total trading fees (net of maker rebates)
- **Kill Switch**: Active/off status
- **Uptime**: Connection uptime percentage

## Troubleshooting

### WebSocket Trading API Connection Issues

When using `use_websocket_trading: true`, connection timeouts are normal:
```
[error] handle_connect error: Timer Expired
```
The bot auto-retries with exponential backoff (1s, 2s, 3s, ...).

### Kill Switch Activated

If trading halts with `KILL SWITCH ACTIVATED`:
1. Check logs for the reason (consecutive errors, daily loss, drawdown)
2. Fix the underlying issue
3. Restart the bot (kill switch resets on restart)

### Stale Data Warnings

```
[ORDER] Rejecting stale orderbook data (Xms old)
```
Indicates network latency > 5 seconds. Check internet connection stability.
