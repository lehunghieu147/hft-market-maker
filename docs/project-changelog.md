# Project Changelog

All significant changes, features, and fixes are documented here. See `docs/development-roadmap.md` for phase-based milestones.

## [Unreleased]

### Phase 06 - Logging Migration (In Progress)
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
