# Code Standards & Logging Patterns

This document defines the coding standards, naming conventions, and logging patterns enforced across the trading bot codebase.

## C++ Language Standards

- **Standard**: C++17 (`-std=c++17`)
- **Build**: CMake 3.14+ with FetchContent for dependencies
- **Compiler Flags**: `-Wall -Wextra -Wpedantic`
- **Optimization**: `-O3 -march=native -mtune=native` (Release builds)

## Naming Conventions

### Files
- **Headers**: `snake_case.h` (e.g., `order_manager.h`)
- **Sources**: `snake_case.cpp` (e.g., `order_manager.cpp`)
- **Directories**: `snake_case/` (e.g., `include/core/`, `src/trading/`)

### Types & Classes
- **Classes**: `PascalCase` (e.g., `OrderManager`, `RiskManager`)
- **Enums**: `PascalCase` (e.g., `OrderSide`, `OrderStatus`, `LogLevel`)
- **Structs**: `PascalCase` (e.g., `OrderBook`, `LatencyMetrics`)

### Variables & Functions
- **Local variables**: `snake_case` (e.g., `bid_price`, `order_id`)
- **Member variables**: `snake_case_` (with trailing underscore, e.g., `active_orders_`, `price_mutex_`)
- **Functions**: `snake_case` (e.g., `get_balance()`, `place_order()`)
- **Constants**: `UPPER_CASE` (e.g., `MAX_POSITION_SIZE`, `DEFAULT_TIMEOUT_MS`)

### Namespace
- **Primary namespace**: `MarketMaker`
- **Sub-namespaces**: Not used (keep flat for clarity)

## Logging Standards

### Logging System: Quill v7.5.0

The codebase uses Quill, a high-performance async logger, for all logging operations.

#### Initialization

At application startup (in `main()`):

```cpp
#include "core/app_logger.h"

int main(int argc, char* argv[]) {
    // Initialize logging once at startup
    AppLogger::init("logs/market_maker.log", true); // file path, console output

    // Application code...

    // Graceful shutdown at exit
    AppLogger::shutdown();
}
```

#### Getting Named Loggers

Retrieve loggers at function/class scope (not global):

```cpp
#include "core/app_logger.h"

void some_function() {
    auto* logger = AppLogger::get("trading");
    LOG_INFO(logger, "Message");
}

class OrderManager {
    void on_fill_event(...) {
        auto* logger = AppLogger::get("trading");
        LOG_WARNING(logger, "Fill event details");
    }
};
```

#### Named Logger Categories

Use these predefined logger names:
- **"trading"**: Order placement, fills, position updates, volatility
- **"network"**: WebSocket connection events, REST API calls, errors
- **"core"**: Config loading, initialization, shutdown
- **"risk"**: Kill switch, error tracking, P&L limits
- **"root"**: General bot startup/shutdown, catchall

#### Log Macros & Levels

Available macros with fmt-style formatting:

```cpp
auto* logger = AppLogger::get("trading");

// DEBUG level (disabled by default, set via log level)
LOG_DEBUG(logger, "Orderbook depth: {} levels", depth_count);

// INFO level (default minimum)
LOG_INFO(logger, "Placed order {} @ {:.2f}", order_id, price);

// WARNING level (important events that don't stop trading)
LOG_WARNING(logger, "High latency detected: {:.1f}ms", latency_ms);

// ERROR level (failures that affect trading)
LOG_ERROR(logger, "Order placement failed: {}", error_msg);

// CRITICAL level (system failure, bot should stop)
LOG_CRITICAL(logger, "Authentication failed, stopping bot");
```

#### Formatting Examples

```cpp
// Numbers with precision
LOG_INFO(logger, "Balance: {:.8f} BTC", balance);

// Multiple fields
LOG_INFO(logger, "Order {} | Side: {} | Price: {:.2f} | Qty: {}",
         order_id, side, price, quantity);

// String formatting
LOG_INFO(logger, "Symbol: {} | Pair: {}-{}",
         symbol, base_asset, quote_asset);

// Conditional complexity (avoid string construction on hot path)
if (logger->should_log(quill::LogLevel::Debug)) {
    LOG_DEBUG(logger, "Complex data: {}", expensive_string_build());
}
```

#### Log Rotation & Storage

- **File**: `logs/market_maker.log`
- **Rotation**: 100MB per file + daily snapshot at 00:00 UTC
- **Mode**: Append mode (auto-creates logs/ directory)
- **Sinks**: ConsoleSink (stdout) + RotatingFileSink
- **Level**: Info by default (suppresses DEBUG)

#### Performance Characteristics

- **Call latency**: ~1-5µs (lock-free ring buffer, non-blocking)
- **Throughput**: Millions of messages/sec
- **Thread model**: Single dedicated backend thread handles I/O
- **No blocking**: Frontend calls return immediately

#### Thread Safety

All Quill operations are thread-safe:
- Multiple threads can log simultaneously without contention
- Backend thread dequeues and flushes asynchronously
- AppLogger::shutdown() flushes all pending messages before returning

### Signal Handler Logging

Signal handlers cannot safely use async logging. Exception: std::cout allowed here:

```cpp
void signal_handler(int sig) {
    std::cout << "Signal received: " << sig << std::endl;
    std::cout << "Shutting down..." << std::endl;
    // Don't use LOG_* macros in signal handlers
}
```

### Startup Logging

If logger not yet initialized, print to stdout before AppLogger::init():

```cpp
void print_banner() {
    std::cout << "Market Maker Bot v1.0" << std::endl; // Allowed before init
}

int main(...) {
    print_banner();
    AppLogger::init(...);
    // Now use AppLogger::get()
}
```

## Code Organization

### Header Files
- Include guards: `#ifndef FILENAME_H` / `#define FILENAME_H` / `#endif`
- Includes in order: C++ std libs → external libs → local includes
- Declarations only (no implementations except inline)
- Doxygen-style comments for public interfaces

### Source Files
- Implementation of corresponding .h file
- Local includes at top (relative path)
- Static functions/anonymous namespaces for internal linkage only
- One concept per file, split large implementations

### Class Guidelines
- Public interface first, private last
- Constructor/destructor next
- Member variables at end with trailing underscore
- Const-correctness throughout
- Prefer composition over inheritance

## Error Handling

### Exceptions
- Use std::exception and derived types
- Log at ERROR or CRITICAL level
- Catch broadly at entry points, specifically elsewhere
- Clean up resources on exception (RAII)

### Return Codes
- Return bool for simple success/failure
- Return std::optional<T> for nullable results
- Return value semantics for error codes (avoid output params)

## Concurrency

### Mutexes & Locks
- Prefer `std::mutex` over raw locks
- Use `std::lock_guard<>` or `std::scoped_lock<>` (RAII)
- Document lock ordering to prevent deadlock
- Member mutex names: `{resource}_mutex_`

### Atomics
- Use `std::atomic<T>` for single-variable synchronization
- Prefer atomics over mutexes for flags/counters
- Use appropriate memory ordering (usually `std::memory_order_relaxed` or `acquire/release`)

### Condition Variables
- Pair with mutex for complex synchronization
- Use spurious wakeup-safe loops
- Name as `{event}_cv_`

## Performance Considerations

### Hot Paths
- Minimize allocations (use pre-allocated buffers)
- Avoid string formatting in tight loops
- Defer logging until non-critical section
- Use lock-free structures where possible

### Latency-Sensitive Code
- Order lifecycle (orderbook → placement) must be <50ms
- No system calls in decision path
- No dynamic allocation on every order
- Profile with sampling profilers, not instrumentation

## Testing & Validation

### Compilation
- Build with all warnings enabled
- No compiler errors or warnings allowed
- Test both Debug and Release configurations

### Runtime Validation
- Check all return codes (curl, OpenSSL, etc.)
- Validate user inputs (symbol, quantities, prices)
- Bounds-check array accesses
- Handle malformed WebSocket/REST responses

### Testing Strategy
- Unit tests for business logic (validators, calculators)
- Integration tests for order lifecycle
- Load tests for rate limiting and throughput
- Manual testing with testnet before mainnet

## Security Guidelines

### API Credentials
- Load from environment variables or config files
- Never hardcode API keys
- Never log API keys or secrets
- Use string clearing or secure_string if available

### Input Validation
- Validate symbol, side, quantity, price on entry
- Range-check price changes (detect erroneous ticks)
- Reject orders with invalid timestamps
- Validate WebSocket frame payloads
- Enforce price/quantity precision: round to configured decimal places before order submission
- Use asset-specific precision settings (config.price_precision, config.quantity_precision)

### Network
- Use HTTPS for REST (enforced by CURL)
- Use WSS (WebSocket Secure) for all WebSocket connections
- Verify SSL certificates (VERIFY_PEER enabled)
- Set reasonable timeouts

## Formatting & Style

### Indentation
- 4 spaces per level (no tabs)
- Braces: Opening on same line (Stroustrup style)

### Line Length
- Prefer <100 characters
- 120 character hard limit for readability

### Comments
- Use `//` for line comments
- Use `/* */` for block comments or Doxygen
- Explain "why", not "what" (code shows what)
- Comment non-obvious algorithms and trade-offs

### Blank Lines
- One blank line between functions/methods
- One blank line between logical sections
- No multiple consecutive blank lines

## Dependencies

### Approved External Libraries
- **JsonCpp** (1.9.5): JSON parsing
- **Asio** (1.28.0, standalone): Async I/O
- **WebSocket++** (0.8.2): WebSocket protocol
- **Quill** (7.5.0): Async logging
- **OpenSSL** (system): TLS/SSL
- **CURL** (system): HTTP client

### Adding New Dependencies
- Justify in code review (is it necessary?)
- Prefer header-only libraries
- Update CMakeLists.txt and README.md
- Document in this standards file
- Check license compatibility

## Documentation Requirements

### Code Comments
- Public functions: Describe purpose, parameters, return value
- Non-obvious algorithms: Explain approach and complexity
- Hacks or workarounds: Explain why and when to remove

### File Headers
- Optional: Brief description of file purpose
- Omit detailed API docs (IDE tooltips sufficient)

### Git Commits
- Format: `type: subject` (e.g., `feat: add order validation`)
- Types: feat, fix, refactor, docs, test, chore
- Keep commits focused and logically independent
- Use conventional commits format

## Checklist Before Commit

- [ ] Code compiles without errors/warnings
- [ ] All tests pass (unit + integration)
- [ ] Logging uses AppLogger (not std::cout except signal/startup)
- [ ] No API credentials in code or logs
- [ ] Thread safety verified (mutexes/atomics sufficient)
- [ ] Performance targets met (latency, throughput)
- [ ] Error cases handled gracefully
- [ ] Comments explain "why", not "what"
- [ ] Naming follows conventions
- [ ] License/attribution for borrowed code included
