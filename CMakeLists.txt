cmake_minimum_required(VERSION 3.14)
project(MarketMakerBot VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
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

# WebSocket++ (develop branch has C++20 compatibility fixes)
FetchContent_Declare(
    websocketpp
    GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
    GIT_TAG develop
)
FetchContent_MakeAvailable(websocketpp)

# Quill - High-performance async logging
set(QUILL_BUILD_EXAMPLES OFF CACHE BOOL "Disable Quill examples")
set(QUILL_BUILD_TESTS OFF CACHE BOOL "Disable Quill tests")
set(QUILL_BUILD_BENCHMARKS OFF CACHE BOOL "Disable Quill benchmarks")

FetchContent_Declare(
    quill
    GIT_REPOSITORY https://github.com/odygrd/quill.git
    GIT_TAG v7.5.0
)
FetchContent_MakeAvailable(quill)

# cpp-httplib (single header HTTP server for Prometheus metrics)
# Disable SSL - only need plain HTTP for metrics/health endpoints
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "")
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "")
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(httplib)

add_compile_definitions(ASIO_STANDALONE _WEBSOCKETPP_CPP11_STL_)

# Include directories
# Project headers as normal (warnings enabled)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
# Third-party headers as SYSTEM (suppresses warnings from their code)
include_directories(SYSTEM
    ${asio_SOURCE_DIR}/asio/include
    ${websocketpp_SOURCE_DIR}
)

# Common source files (shared between both bots)
set(COMMON_SOURCES
    # Core
    src/core/config.cpp
    src/core/config_loader.cpp
    src/core/logger.cpp
    src/core/app_logger.cpp
    # Exchange
    src/exchange/exchange_factory.cpp
    src/exchange/binance_exchange.cpp
    # Network
    src/network/rest_client.cpp
    src/network/websocket_client.cpp
    src/network/websocket_trading_client.cpp
    src/network/websocket_trading_adapter.cpp
    # Trading (shared)
    src/trading/order_manager.cpp
    src/trading/order_validator.cpp
    src/trading/rate_limiter.cpp
    src/trading/position_tracker.cpp
    src/trading/pnl_tracker.cpp
    src/trading/risk_manager.cpp
    src/trading/volatility_tracker.cpp
    src/trading/signal_engine.cpp
    src/trading/latency_tracker.cpp
    src/trading/orderbook-imbalance-tracker.cpp
    # Cloud / GCP
    src/core/gcp_auth_provider.cpp
    src/cloud/gcp_publisher.cpp
    src/cloud/gcp_storage_client.cpp
    # Metrics
    src/core/metrics_server.cpp
)

set(LINK_LIBS
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    jsoncpp_lib
    quill::quill
    httplib::httplib
)

# Static curl requires transitive dependencies - find by full path
if(UNIX AND NOT APPLE)
    foreach(_lib nghttp2 idn2 psl ssh gssapi_krb5 lber ldap zstd brotlidec brotlicommon z rtmp)
        find_library(_${_lib}_path ${_lib} PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu)
        if(_${_lib}_path)
            list(APPEND LINK_LIBS ${_${_lib}_path})
        endif()
        unset(_${_lib}_path CACHE)
    endforeach()
endif()

# Market Maker executable
add_executable(market_maker
    src/main.cpp
    src/trading/market_maker.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(market_maker PRIVATE ${LINK_LIBS})

# Momentum Taker executable
add_executable(momentum_taker
    src/momentum_main.cpp
    src/trading/momentum_taker.cpp
    ${COMMON_SOURCES}
)
target_link_libraries(momentum_taker PRIVATE ${LINK_LIBS})

# Backtest executable
add_executable(backtest
    src/backtest_main.cpp
    src/backtesting/backtest-engine.cpp
    src/backtesting/data-loader.cpp
    src/backtesting/simulated-exchange.cpp
    src/trading/volatility_tracker.cpp
    src/trading/position_tracker.cpp
    src/core/app_logger.cpp
)
target_link_libraries(backtest PRIVATE Threads::Threads quill::quill)

# Phase B Test executable
add_executable(phase_b_tests
    tests/phase-b-tests.cpp
    src/trading/volatility_tracker.cpp
    src/trading/risk_manager.cpp
    src/trading/position_tracker.cpp
    src/trading/pnl_tracker.cpp
    src/trading/orderbook-imbalance-tracker.cpp
    src/trading/signal_engine.cpp
    src/core/app_logger.cpp
    src/backtesting/data-loader.cpp
    src/backtesting/simulated-exchange.cpp
    src/backtesting/backtest-engine.cpp
)
target_link_libraries(phase_b_tests PRIVATE Threads::Threads quill::quill)

# --- gRPC Control API (optional) ---
option(BUILD_GRPC "Build with gRPC control API" ON)

if(BUILD_GRPC)
    set(ABSL_ENABLE_INSTALL ON CACHE BOOL "")
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "")
    set(protobuf_INSTALL OFF CACHE BOOL "")
    set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "")
    set(gRPC_BUILD_TESTS OFF CACHE BOOL "")
    set(gRPC_BUILD_CSHARP_EXT OFF CACHE BOOL "")
    set(gRPC_BUILD_GRPC_CSHARP_PLUGIN OFF CACHE BOOL "")
    set(gRPC_BUILD_GRPC_NODE_PLUGIN OFF CACHE BOOL "")
    set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN OFF CACHE BOOL "")
    set(gRPC_BUILD_GRPC_PHP_PLUGIN OFF CACHE BOOL "")
    set(gRPC_BUILD_GRPC_PYTHON_PLUGIN OFF CACHE BOOL "")
    set(gRPC_BUILD_GRPC_RUBY_PLUGIN OFF CACHE BOOL "")
    # Use system OpenSSL instead of bundled BoringSSL to avoid symbol conflicts
    set(gRPC_SSL_PROVIDER "package" CACHE STRING "")

    FetchContent_Declare(
        gRPC
        GIT_REPOSITORY https://github.com/grpc/grpc.git
        GIT_TAG v1.60.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(gRPC)

    # Proto code generation
    set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/protos/trading_bot.proto")
    set(PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY ${PROTO_OUT})

    add_custom_command(
        OUTPUT
            "${PROTO_OUT}/trading_bot.pb.cc"
            "${PROTO_OUT}/trading_bot.pb.h"
            "${PROTO_OUT}/trading_bot.grpc.pb.cc"
            "${PROTO_OUT}/trading_bot.grpc.pb.h"
        COMMAND $<TARGET_FILE:protoc>
            --grpc_out="${PROTO_OUT}"
            --cpp_out="${PROTO_OUT}"
            --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
            -I "${CMAKE_CURRENT_SOURCE_DIR}/protos"
            "${PROTO_SRC}"
        DEPENDS "${PROTO_SRC}" protoc grpc_cpp_plugin
    )

    # gRPC service library
    add_library(trading_bot_grpc
        src/network/grpc_server.cpp
        src/network/trading_bot_service_impl.cpp
        "${PROTO_OUT}/trading_bot.pb.cc"
        "${PROTO_OUT}/trading_bot.grpc.pb.cc"
    )
    target_include_directories(trading_bot_grpc PUBLIC
        ${PROTO_OUT}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_include_directories(trading_bot_grpc SYSTEM PUBLIC
        ${asio_SOURCE_DIR}/asio/include
        ${websocketpp_SOURCE_DIR}
    )
    target_link_libraries(trading_bot_grpc PUBLIC
        grpc++
        libprotobuf
        quill::quill
    )

    target_compile_definitions(market_maker PRIVATE BUILD_GRPC)
    target_compile_definitions(momentum_taker PRIVATE BUILD_GRPC)
    target_link_libraries(market_maker PRIVATE trading_bot_grpc)
    target_link_libraries(momentum_taker PRIVATE trading_bot_grpc)
endif()

option(BUILD_BENCHMARKS "Build benchmarks" ON)

if(BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Disable benchmark tests")
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "Disable benchmark gtests")
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3
    )
    FetchContent_MakeAvailable(benchmark)
    add_subdirectory(benchmarks)
endif()

# Installation
install(TARGETS market_maker momentum_taker
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