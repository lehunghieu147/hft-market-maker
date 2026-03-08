# ============================================================================
# HFT Market Maker Bot - Multi-stage Dockerfile
# ============================================================================
# Stage 1: Builder - compile the application
# Stage 2: Runtime - minimal image with only runtime dependencies
# ============================================================================

# ============================================================================
# Stage 1: Builder
# ============================================================================
FROM ubuntu:24.04 AS builder

# Install build dependencies
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    libcurl4-openssl-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source code
COPY . .

# Build the application
# Using Release build for production optimization (-O3, -march=native)
RUN cmake -S . -B out -DCMAKE_BUILD_TYPE=Release && \
    cmake --build out -j$(nproc)

# ============================================================================
# Stage 2: Runtime
# ============================================================================
FROM ubuntu:24.04

# Install runtime dependencies only
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
    libssl3 \
    libcurl4 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user for security
RUN useradd -m -u 1000 trader && \
    mkdir -p /app/logs /app/config && \
    chown -R trader:trader /app

# Set working directory
WORKDIR /app

# Copy compiled binary from builder
COPY --from=builder --chown=trader:trader /app/out/market_maker ./market_maker

# Copy config files
COPY --chown=trader:trader config/ ./config/

# Switch to non-root user
USER trader

# Create logs directory
RUN mkdir -p logs

# Environment variables (override these at runtime)
ENV BINANCE_API_KEY="" \
    BINANCE_API_SECRET="" \
    SYMBOL="BTCUSDT" \
    ORDER_SIZE="0.001" \
    SPREAD_PERCENTAGE="0.02"

# Expose port (if metrics/monitoring needed in future)
# EXPOSE 8080

# Health check (optional - check if process is running)
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD pgrep -f market_maker || exit 1

# Run the application
# Use config file passed as argument or default to config.json
ENTRYPOINT ["./market_maker"]
CMD ["config/config.json"]
