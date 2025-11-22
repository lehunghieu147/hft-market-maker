# High-Frequency Trading Market Maker Bot

A high-performance, low-latency market maker bot for cryptocurrency trading on Binance exchange, implemented in C++17.

## Table of Contents

- [System Overview](#system-overview)
- [Key Features](#key-features)
- [Architecture](#architecture)
- [Performance Metrics](#performance-metrics)
- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Configuration](#configuration)
- [Building](#building)
- [Running](#running)
- [Technical Details](#technical-details)
- [Sample Output](#sample-output)
- [Troubleshooting](#troubleshooting)

## System Overview

This market maker bot implements a complete trading system with:

- **Real-time market data** via WebSocket connections
- **low-latency order execution** using Binance WebSocket Trading API
- **Automatic order management** with continuous price updates
- **WebSocket reconnection** with exponential backoff
- **Multi-exchange support** architecture (currently Binance)

### Target Performance

- **Reaction Latency**: < 50ms (orderbook update → order placement)

## Key Features

### Trading Strategy
- **Market making**: Places simultaneous BID and ASK orders around mid-price
- **Configurable spread**: Adjustable spread percentage
- **Dynamic order updates**: Continuously updates orders based on price movements
- **Price change threshold**: Optimizes by skipping updates for small price changes

### Reliability
- **Automatic reconnection**: WebSocket reconnects with exponential backoff
- **Connection monitoring**: Tracks connection status and reconnection attempts
- **Error handling**: Comprehensive error handling and recovery
- **Order validation**: Validates orders before placement

### Monitoring
- **Real-time metrics**: Tracks latency, order success rate, uptime
- **Detailed logging**: Comprehensive logging with configurable verbosity
- **Performance tracking**: Min/max/average latency measurements


### Core Components

1. **WebSocketTradingAdapter**: Main exchange adapter
   - Manages dual WebSocket connections (market data + trading)
   - Handles orderbook updates from market data stream
   - Executes orders via WebSocket Trading API
   - Automatic reconnection for both streams

2. **OrderManager**: Trading logic and order management
   - Calculates BID/ASK prices based on mid-price
   - Places and cancels orders asynchronously
   - Tracks active orders and updates
   - Performance optimization with price change threshold

3. **WebSocketClient**: Market data stream
   - Real-time orderbook updates
   - Maintains persistent connection
   - Handles reconnection with exponential backoff
   - Frame-level WebSocket protocol implementation

4. **WebSocketTradingClient**: Order execution stream
   - Ultra low-latency order placement via WebSocket
   - Bidirectional communication for order status
   - HMAC-SHA256 authentication
   - Async order placement and cancellation

## Performance Metrics

The bot tracks and reports:

- **Reaction Latency**: Time from orderbook update to order placement
- **Order Success Rate**: Percentage of successful orders
- **Uptime**: Connection uptime percentage
- **Reconnection Count**: Number of WebSocket reconnections
- **Average/Min/Max Latency**: Statistical latency measurements

## Prerequisites

### System Requirements
- **OS**: Linux (tested on Ubuntu 24.04)
- **Network**: Stable internet connection

### Dependencies
- **CMake** >= 3.10
- **C++ Compiler** with C++17 support (GCC 7+, Clang 5+)
- **OpenSSL** (libssl-dev)
- **CURL** (libcurl4-openssl-dev)

## Installation

### 1. Install Dependencies

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    libssl-dev \
    libcurl4-openssl-dev \
    git
```

**CentOS/RHEL:**
```bash
sudo yum install -y \
    gcc-c++ \
    cmake \
    openssl-devel \
    libcurl-devel \
    git
```

**macOS:**
```bash
brew install cmake openssl curl
```

### 2. Clone Repository

```bash
git clone <repository-url>
cd submission
```

### 3. JsonCpp Dependency

JsonCpp will be automatically downloaded and built by CMake via FetchContent. No manual installation required.

## Configuration

### Configuration File

The bot uses JSON configuration files located in `config/` directory. Example: `config/config_sei_5usd.json`

```json
{
    "api": {
        "key": "YOUR_API_KEY",
        "secret": "YOUR_API_SECRET"
    },
    "trading": {
        "symbol": "SEIUSDT",
        "order_size": 20.0,
        "spread_percentage": 0.02,
        "base_asset": "SEI",
        "quote_asset": "USDT",
        "display_assets": ["USDT", "SEI", "BTC"],
        "supported_quote_currencies": ["USDT", "BUSD", "SEI", "ETH", "BNB"]
    },
    "exchange": {
        "name": "binance",
        "ws_url": "wss://stream.binance.com:9443/ws",
        "rest_url": "https://api.binance.com",
        "ws_trading_url": "wss://ws-api.binance.com:443",
        "use_websocket_trading": true,
        "testnet": false
    },
    "performance": {
        "order_update_cooldown_ms": 500,
        "reconnect_delay_ms": 5000,
        "max_reconnect_attempts": 10,
        "max_orders_per_second": 2
    }
}
```

### Configuration Parameters

#### API Settings
- `api.key`: Your Binance API key
- `api.secret`: Your Binance API secret

#### Trading Settings
- `symbol`: Trading pair (e.g., "SEIUSDT", "BTCUSDT")
- `order_size`: Order quantity
- `spread_percentage`: Spread from mid-price (0.02 = 2%)
- `display_assets`: Assets to display in account info
- `supported_quote_currencies`: Quote currencies for symbol conversion

#### Exchange Settings
- `name`: Exchange name ("binance")
- `ws_url`: WebSocket URL for market data
- `rest_url`: REST API URL (for account info and order execution when WebSocket trading disabled)
- `ws_trading_url`: WebSocket Trading API URL for order execution
- `use_websocket_trading`: Enable WebSocket Trading API (true/false)
- `testnet`: Use testnet (true/false)

#### Performance Settings
- `order_update_cooldown_ms`: Minimum time between order updates
- `reconnect_delay_ms`: Initial reconnection delay
- `max_reconnect_attempts`: Maximum reconnection attempts
- `max_orders_per_second`: Rate limit for orders

## Building

### Build Steps

```bash
# Create build directory
mkdir -p build
cd build

# Configure with CMake
cmake ..

# Build (use -j for parallel compilation)
make -j4
```

### Build Output

The executable will be created at: `build/bin/market_maker`

### Build Options

**Debug Build:**
```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j4
```

**Release Build (optimized):**
```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j4
```

## Running

### Basic Usage

```bash
# Run with specific config file from build directory
./bin/market_maker ../config/config_sei.json
./bin/market_maker ../config/config_doge.json

# Run from project root
./build/bin/market_maker config/config_sei.json
./build/bin/market_maker config/config_doge.json
```

### Command Line Options

```bash
# Show help
./bin/market_maker --help

# Use specific config
./bin/market_maker <config_file>
```

### Environment Variables

You can override config settings with environment variables:

```bash
export BINANCE_API_KEY="your_api_key"
export BINANCE_API_SECRET="your_api_secret"
export SYMBOL="BTCUSDT"
export ORDER_SIZE="0.001"
export SPREAD_PERCENTAGE="0.02"

./bin/market_maker ../config/config_doge.json
```

### Running in Background

```bash
# Run in background with nohup
nohup ./bin/market_maker ../config/config_sei.json > output.log 2>&1 &

# Run with screen
screen -S market_maker
./bin/market_maker ../config/config_sei.json

# Detach: Ctrl+A, D
# Reattach: screen -r market_maker
```

### Stopping the Bot

- **Ctrl+C**: Graceful shutdown
- **SIGTERM**: `kill <pid>` (graceful)
- **SIGKILL**: `kill -9 <pid>` (force)


## Technical Details

### WebSocket Implementation

#### Market Data Stream
- **Protocol**: RFC 6455 WebSocket over TLS
- **Endpoint**: wss://stream.binance.com:9443/ws
- **Frame handling**: Full frame parsing and masking
- **Ping/Pong**: Automatic heartbeat handling
- **Reconnection**: Exponential backoff (5s, 10s, 20s, ...)

#### WebSocket Trading API
- **Protocol**: Binance WebSocket API v3
- **Endpoint**: wss://ws-api.binance.com:443/ws-api/v3
- **Authentication**: HMAC-SHA256 signature per request
- **Bidirectional**: Request-response model over WebSocket
- **Ultra low latency**: ~10-20ms faster than REST API
- **Features**:
  - Async order placement
  - Real-time order status updates
  - Order cancellation
  - Account queries

### Threading Model

- **Main thread**: Trading logic and order management
- **WebSocket thread**: Market data reception
- **Async operations**: Non-blocking order placement

### Security

- **API credentials**: Loaded from config file
- **HMAC signing**: SHA256 for request authentication
- **TLS/SSL**: All connections encrypted
- **No credential logging**: API keys never logged

### Performance Optimizations

1. **WebSocket Trading API**: Uses WebSocket instead of REST for ultra low latency
2. **Dual WebSocket streams**: Separate connections for market data and trading
3. **Price change threshold**: Skip updates for small price changes (< 0.01%)
4. **Async order operations**: Place and cancel orders in parallel
5. **Pre-calculation**: Calculate prices before network I/O
6. **Lock-free operations**: Atomic operations where possible
7. **Persistent connections**: No TCP handshake overhead per order

## Sample Output

```
=====================================================
  PRICE CALCULATION
=====================================================
  Mid Price:       $0.28545 (from OrderBook)
  Spread Config:    2.0%
-----------------------------------------------------
  BUY Order (BID):
    Formula: MidPrice × (1 - Spread)
    Calc: 0.28545 × 0.9800 = 0.2797410 -> $0.28000
-----------------------------------------------------
  SELL Order (ASK):
    Formula: MidPrice × (1 + Spread)
    Calc: 0.28545 × 1.0200 = 0.2911590 -> $0.29000
-----------------------------------------------------
  Calc Time: 1 us
=====================================================

=========== PLACING NEW ORDERS ===========
  Mid Price: $0.28595
  BID (Buy):  $0.28000 [Qty: 20.00000]
  ASK (Sell): $0.29000 [Qty: 20.00000]
==========================================

[SUCCESS] BID Order Placed
  Order ID: 2236530873 | Price: $0.28000000 | Qty: 20.00000000

[SUCCESS] ASK Order Placed
  Order ID: 2236530916 | Price: $0.29000000 | Qty: 20.00000000

=============================================
  BOTH ORDERS PLACED SUCCESSFULLY
=============================================

================================================
  LATENCY METRICS
================================================
  Reaction Latency: 15.123 ms (45123 us)
  Status: TARGET MET (< 50ms requirement)
================================================
```

## Troubleshooting

### WebSocket Trading API Connection Issues

When using `use_websocket_trading: true`, you may occasionally see connection timeout errors:

```
[error] handle_connect error: Timer Expired
WebSocket Trading connection failed
```

**This is normal** - the bot has built-in retry mechanism that will automatically:
- Retry connection up to 100 times
- Clean up and recreate WebSocket client between retries
- Use exponential backoff delay (1s, 2s, 3s, ...)

The bot will continue retrying until successfully connected and operate normally afterward.

### Process Management

If you need to stop the bot, use **Ctrl+C** for graceful shutdown. The bot will automatically exit after 1 second of cleanup.

