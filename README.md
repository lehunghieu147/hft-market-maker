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
| OrderManager   |  | RiskManager     |  | UserDataStream  |
| - place orders |  | - kill switch   |  | - real fills    |
| - cancel/replace| | - error tracking|  | - balance events|
| - fill events  |  +--------+--------+  +-----------------+
+----------------+           |
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
| UserDataStream | `network/user_data_stream` | Binance User Data Stream for real-time fills |
| RateLimiter | `trading/rate_limiter` | Exchange rate limit compliance |
| WebSocketClient | `network/websocket_client` | Market data WebSocket |
| WebSocketTradingClient | `network/websocket_trading_client` | Order execution WebSocket |
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
- **Position limits**: Configurable max position size with atomic pair checks
- **Daily loss limit**: Automatic trading halt when daily P&L breaches threshold
- **Max drawdown**: Peak-to-trough drawdown monitoring
- **Kill switch**: Emergency stop with manual reset
- **Consecutive error tracking**: Auto-halt after N consecutive failures
- **Fee-aware P&L**: Supports signed maker rebates (negative fees = rebate)

### Trading Strategy
- **Market making**: Simultaneous BID/ASK orders around VWAP mid-price
- **Volatility-adjusted spreads**: Spread scales with market volatility
- **Orderbook depth analysis**: VWAP mid price from top N levels
- **Imbalance ratio**: Bid/ask volume imbalance detection
- **Price change threshold**: Skip updates for insignificant price moves (< 0.01%)
- **Stale data rejection**: Reject orderbook updates older than 5 seconds

### Real-Time Fill Tracking
- **Binance User Data Stream**: WebSocket connection for `executionReport` events
- **Accurate position tracking**: Based on real fills, not placement assumptions
- **Partial fill handling**: Tracks cumulative fill quantities
- **Balance monitoring**: Real-time `outboundAccountPosition` events
- **Listen key management**: Auto-create, 30-min keepalive, cleanup on stop

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
| `max_drawdown` | -500.0 | Peak-to-trough drawdown limit (negative USDT) |
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

## Performance Metrics

The bot tracks and reports every 30 seconds:

- **Reaction Latency**: Time from orderbook update to order placement (target: < 50ms)
- **Execution Latency**: Time to execute the order placement function
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
