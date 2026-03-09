# Project Changelog

All significant changes, features, and fixes are documented here. See `docs/development-roadmap.md` for phase-based milestones.

## [Unreleased]

### Phase B1-B5 - Advanced Strategies & Backtesting (2025-03-09)
- **Status**: Complete
- **Scope**: Inventory-aware quoting, order book imbalance detection, multi-timeframe momentum, dynamic sizing, backtesting framework
- **New Executables**: `backtest` for tick-level replay testing
- **New Tools**: `tools/download-historical-data.sh` for fetching Binance tick data

### Phase B1 - Avellaneda-Stoikov Model
- **Status**: Complete
- **Changes**:
  - Added `AvellanedaStoikovModel` class (`include/trading/avellaneda-stoikov-model.h`)
  - Implements inventory-aware quoting: reservation price adjusts for position, optimal spread widens with volatility
  - Configuration: `use_avellaneda_stoikov`, `as_gamma` (risk aversion), `as_kappa` (order intensity), `as_time_horizon_sec`
  - Reservation price: `r(s,q,t) = s - q*gamma*sigma^2*tau` (when long, r < mid; when short, r > mid)
  - Optimal spread: `delta = gamma*sigma^2*tau + (2/gamma)*ln(1 + gamma/kappa)`
  - Prevents crossed orders via `half_spread >= mid_price * 1e-6`

### Phase B2 - Multi-Timeframe Momentum & VWAP Tracker
- **Status**: Complete
- **Changes**:
  - Created `VwapTracker` class (rolling VWAP with Welford variance, upper/lower bands)
  - Extended `SignalEngine` with multi-timeframe confirmation (fast EMA + slow EMA)
  - Configuration: `use_multi_timeframe`, `fast_ema_window` (default 8), `slow_ema_window` (default 50), `volume_expansion_threshold` (default 1.2)
  - Signal fires only when both EMAs confirm: fast must cross slow AND volume >= 1.2x average
  - Reduces false signals and improves trade quality

### Phase B3 - Order Book Imbalance Detection
- **Status**: Complete
- **Changes**:
  - Created `OrderBookImbalanceTracker` class (OBI computation and EMA smoothing)
  - OBI formula: `(bid_volume - ask_volume) / (bid_volume + ask_volume)` → range [-1, +1]
  - Positive OBI = buy pressure (compress bid spread), Negative OBI = sell pressure (compress ask spread)
  - Configuration: `use_obi_tilt`, `obi_levels` (orderbook depth, default 5), `obi_tilt_factor` (max tilt %, default 0.3), `obi_min_volume` (min vol for signal)
  - Spread tilt: `bid_spread = base_spread * (1 - tilt_factor * max(0, obi))`, `ask_spread = base_spread * (1 + tilt_factor * max(0, -obi))`

### Phase B4 - Volatility-Adjusted Position Sizing
- **Status**: Complete
- **Changes**:
  - Extended `VolatilityTracker` with dynamic sizing logic
  - Configuration: `use_dynamic_sizing`, `vol_sizing_exponent` (default 0.5 = sqrt), `min_size_multiplier` (default 0.5), `max_size_multiplier` (default 2.0)
  - Size scaling: `order_size = base_size * (baseline_vol / current_vol) ^ exponent`, clamped to [min_mult, max_mult]
  - Larger orders in calm markets, smaller in turbulent markets (risk-adjusted)
  - Works alongside Avellaneda-Stoikov model for coordinated inventory + vol management

### Phase B5 - Backtesting Framework
- **Status**: Complete
- **Changes**:
  - Created `BacktestEngine` class for tick-level replay from CSV data
  - Created `DataLoader` for parsing CSV: timestamp, OHLCV, bids[5], asks[5]
  - Created `SimulatedExchange` for order book simulation, matching, fill latency injection
  - Created `PerformanceMetrics` class: Sharpe ratio, max drawdown, win rate, avg trade duration, trade list
  - Created `backtest_main.cpp` executable: `./backtest <data.csv> [output.csv]`
  - CSV export: PnL curve + trade log for external analysis (Jupyter, etc.)
  - Configuration: `simulation.network_delay_ms` (latency injection for realistic fills)
- **Usage**:
  - Download data: `./tools/download-historical-data.sh DOGEEUSDT 2025-03-01 ./data/`
  - Run backtest: `./backtest ./data/DOGEEUSDT_2025-03-01.csv ./results.csv`
  - Analyze results in Jupyter/Excel

### Bug Fixes & Enhancements (Recent)
- **WebSocketClient heap-use-after-free fix**: Destructor now properly shuts down socket before joining threads, then frees SSL resources. Prevents use-after-free errors on disconnect when threads try to access freed SSL context.
- **Config precision fields**: Added `price_precision` and `quantity_precision` to trading config section. Enables proper handling of low-price assets (e.g., DOGE with price_precision=4 instead of default=2) to prevent order crossing due to rounding errors.

### Phase 08 - Backtesting & Advanced Strategies (In Progress)
- See Phase B1-B5 section above

### Phase 07 - Momentum Taker Strategy
- **Status**: Complete
- **Changes**:
  - Implemented momentum-based taker trading strategy bot (`momentum_taker`)
  - Created `EmaEngine` header-only class for EMA(400) calculation
  - Implemented `SignalEngine` with epsilon thresholds and cooldown enforcement
  - Added `LatencyTracker` circular buffer for percentile latency analysis
  - Created `MomentumTakerBot` with signal-driven IOC order execution
  - Extended `IExchange` with `place_market_order()` and `place_ioc_order()` methods
  - Enhanced `OrderManager` with `place_taker_order()` for immediate execution
  - Updated `BinanceExchange` to support market/IOC order types
  - Modified `RestClient` to handle market/IOC order parameters
  - Extended `Config` with `MomentumConfig` struct (EMA window, epsilon, cooldown, order size)
  - Created `config.momentum.json` configuration template
  - Updated CMakeLists.txt for two-target build (market_maker + momentum_taker)
  - Fixed signal handler cleanup (removed Quill calls from async-signal-unsafe context)
  - Added `.env` to .gitignore for credential safety
- **Strategy Details**:
  - BUY signal: `bestAsk > EMA(400) * (1.0 + epsilon)` → price breaking above trend
  - SELL signal: `bestBid < EMA(400) * (1.0 - epsilon)` → price breaking below trend
  - Cooldown prevents over-trading (default 5 seconds between signals)
  - Hysteresis: price must cross back to EMA before new signal allowed
  - IOC limit orders provide price protection vs pure market orders
- **Build**: Two binaries sharing 16 common source files, separate main.cpp/momentum_main.cpp

### Phase 06 - Logging Migration
- **Status**: Complete
- **Commits**: `92ceabd` (docs), migration branch
- **Changes**:
  - Migrated entire logging system from std::cout/std::cerr to Quill v7.5.0 async logger
  - Created `AppLogger` singleton in `include/core/app_logger.h` / `src/core/app_logger.cpp`
  - Implemented dual-sink architecture: ConsoleSink (stdout) + RotatingFileSink (logs/market_maker.log)
  - Configured 100MB file rotation with daily snapshot at 00:00 UTC
  - Named loggers: "trading", "network", "core", "risk", "root"
  - Updated 16 source files with LOG_* macros (57 calls in order_manager, 28 in websocket_trading_client, etc.)
  - Intentionally kept std::cout in 4 places: signal handler (2 calls), print_usage (1), banner (1)
  - CMakeLists.txt: Bumped cmake_minimum_required 3.10 → 3.14, added Quill v7.5.0 via FetchContent
- **Performance**: ~1-5µs per log call (lock-free ring buffer) vs ~10-50µs for std::cout
- **Benefits**:
  - Zero blocking on call site (async backend thread handles I/O)
  - Low-latency logging suitable for HFT context
  - Structured logging with fmt-style formatting
  - Automatic file rotation prevents disk bloat
  - Graceful shutdown flushes pending logs

### Phase 05 - Documentation & Polish
- **Status**: Complete
- **Commits**: `92ceabd` docs: add comprehensive documentation
- **Changes**:
  - Added system architecture documentation with component diagrams
  - Created codebase summary with file map and design decisions
  - Documented all core components and their responsibilities
  - Added troubleshooting guide and performance metrics

## [v1.0.0] - 2025-03-08

### Phase 04 - Trading Logic & Real-Time Fills
- **Status**: Complete
- **Commits**: `ff4c88d` feat: add trading logic improvements
- **Changes**:
  - Implemented Binance User Data Stream for real-time fill tracking
  - Added VolatilityTracker with Welford's online algorithm
  - Implemented VWAP mid-price calculation from orderbook depth
  - Added fill event handlers and position/PnL updates
  - Implemented listen key management with 30-min keepalive
  - Added cancel-and-replace order handling

### Phase 03 - Risk Management Framework
- **Status**: Complete
- **Commits**: `4f0d351` feat: add risk management framework
- **Changes**:
  - Implemented RiskManager with kill switch functionality
  - Added PositionTracker with configurable position limits
  - Implemented PnLTracker for realized P&L, daily loss, and drawdown monitoring
  - Added fee-aware P&L calculation (supports maker rebates)
  - Implemented consecutive error tracking for auto-halt
  - Added pre-trade risk gate checking

### Phase 02 - Security Hardening
- **Status**: Complete
- **Commits**: `fb0eff7` fix: harden security across network layer
- **Changes**:
  - Enhanced SSL/TLS certificate verification
  - Strengthened HMAC-SHA256 authentication for all signed endpoints
  - Added WebSocket payload size limits (16MB cap for OOM prevention)
  - Implemented proper SSL/thread cleanup on all failure paths
  - Added credential sanitization (API keys never logged)
  - Enhanced RFC 6455 WebSocket compliance (pong echoes ping)

### Phase 01 - Critical Bug Fixes
- **Status**: Complete
- **Commits**: `fcfd2dc` fix: resolve 10 critical bugs in Phase 01
- **Changes**:
  - Fixed race conditions in orderbook synchronization
  - Corrected atomic variable usage for thread safety
  - Fixed order validation logic and crossed-order detection
  - Improved error handling and recovery paths
  - Enhanced connection resilience with exponential backoff
  - Added comprehensive error tracking and logging

## Notation

- **[Unreleased]**: Changes in development, not yet released
- **[v1.0.0]**: Stable release versions
- **Status**: Planned, In Progress, Complete
- **Type**: Feature (feat), Fix (fix), Refactor, Docs (docs), Security (sec)
- **Impact**: Breaking, Enhancement, Bugfix
