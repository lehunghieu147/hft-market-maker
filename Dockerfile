# === Build Stage ===
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates \
    libssl-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY CMakeLists.txt .
COPY include/ include/
COPY src/ src/
COPY protos/ protos/
COPY benchmarks/ benchmarks/
COPY tests/ tests/

# Build (gRPC & deps fetched via FetchContent)
RUN cmake -S . -B out \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_BENCHMARKS=OFF \
    && cmake --build out -j$(nproc)

# === Runtime Stage ===
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libcurl4 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy binaries
COPY --from=builder /build/out/market_maker .
COPY --from=builder /build/out/momentum_taker .

# Copy config templates
COPY config/ config/

RUN mkdir -p logs

# gRPC + Metrics ports
EXPOSE 50051 8888

HEALTHCHECK --interval=10s --timeout=5s --retries=3 \
    CMD curl -f http://localhost:8888/health || exit 1

ENTRYPOINT ["./market_maker"]
CMD ["config/config.json"]
