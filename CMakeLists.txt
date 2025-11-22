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

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Source files
set(SOURCES
    src/main.cpp
    # src/market_maker.cpp
    src/websocket_client.cpp
    src/rest_client.cpp
    src/order_manager.cpp
    src/logger.cpp
    src/config_loader.cpp
    src/rate_limiter.cpp
    src/order_validator.cpp
    # Multi-exchange support files
    src/config.cpp
    src/exchange_factory.cpp
    src/binance_exchange.cpp
    src/market_maker.cpp
    # WebSocket Trading files
    src/websocket_trading_client.cpp
    src/websocket_trading_adapter.cpp
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

# Copy configuration files to build directory
# configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.json
#     ${CMAKE_CURRENT_BINARY_DIR}/config.json COPYONLY)
# configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.testnet.json
#     ${CMAKE_CURRENT_BINARY_DIR}/config.testnet.json COPYONLY)

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