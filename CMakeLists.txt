cmake_minimum_required(VERSION 3.10)
project(MarketMakerBot VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags for optimization and warnings
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native -mtune=native")

# Find required packages
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# Find or fetch dependencies
include(FetchContent)

# JsonCpp - disable tests and examples
set(JSONCPP_WITH_TESTS OFF CACHE BOOL "Disable JsonCpp tests")
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "Disable JsonCpp post-build tests")
set(JSONCPP_WITH_EXAMPLE OFF CACHE BOOL "Disable JsonCpp examples")
set(BUILD_TESTING OFF CACHE BOOL "Disable testing")

FetchContent_Declare(
    jsoncpp
    GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
    GIT_TAG 1.9.5
)
FetchContent_MakeAvailable(jsoncpp)

# Asio (standalone)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-28-0
)
FetchContent_MakeAvailable(asio)

# WebSocket++
FetchContent_Declare(
    websocketpp
    GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
    GIT_TAG 0.8.2
)
FetchContent_MakeAvailable(websocketpp)

add_compile_definitions(ASIO_STANDALONE _WEBSOCKETPP_CPP11_STL_)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${asio_SOURCE_DIR}/asio/include
    ${websocketpp_SOURCE_DIR}
)

# Source files
set(SOURCES
    src/main.cpp
    # Core
    src/core/config.cpp
    src/core/config_loader.cpp
    src/core/logger.cpp
    # Exchange
    src/exchange/exchange_factory.cpp
    src/exchange/binance_exchange.cpp
    # Network
    src/network/rest_client.cpp
    src/network/websocket_client.cpp
    src/network/websocket_trading_client.cpp
    src/network/websocket_trading_adapter.cpp
    # Trading
    src/trading/market_maker.cpp
    src/trading/order_manager.cpp
    src/trading/order_validator.cpp
    src/trading/rate_limiter.cpp
    src/trading/position_tracker.cpp
    src/trading/pnl_tracker.cpp
    src/trading/risk_manager.cpp
)

# Create executable
add_executable(market_maker ${SOURCES})

# Link libraries
target_link_libraries(market_maker
    PRIVATE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    jsoncpp_lib
)

# Installation
install(TARGETS market_maker
    RUNTIME DESTINATION bin
)

# Create logs directory
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/logs)

# Print build configuration
message(STATUS "")
message(STATUS "Market Maker Bot Build Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "")
message(STATUS "Dependencies:")
message(STATUS "  OpenSSL: ${OPENSSL_VERSION}")
message(STATUS "  CURL: Found")
message(STATUS "  JsonCpp: Will be fetched")
message(STATUS "")