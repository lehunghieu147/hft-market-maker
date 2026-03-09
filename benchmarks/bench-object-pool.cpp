/// Benchmark: ObjectPool<Order,N> vs heap allocation strategies.
/// Compares pool alloc/dealloc, std::make_shared, raw new/delete,
/// and burst allocation patterns for include/core/object-pool.h

#include <benchmark/benchmark.h>
#include "core/object-pool.h"
#include "core/types.h"

#include <array>
#include <chrono>
#include <memory>

namespace {

// Helper: construct a realistic Order (avoids default-constructed noise)
static MarketMaker::Order make_order() {
    MarketMaker::Order o;
    o.order_id        = "ORD-0001";
    o.client_order_id = "CLIENT-0001";
    o.symbol          = "BTCUSDT";
    o.side            = MarketMaker::OrderSide::BUY;
    o.price           = 45000.0;
    o.quantity        = 0.01;
    o.executed_quantity = 0.0;
    o.status          = MarketMaker::OrderStatus::NEW;
    o.created_time    = std::chrono::steady_clock::now();
    o.updated_time    = o.created_time;
    return o;
}

// ─── ObjectPool: single allocate + deallocate cycle ─────────────────────────

static void BM_PoolAllocDealloc(benchmark::State& state) {
    MarketMaker::ObjectPool<MarketMaker::Order, 128> pool;

    for (auto _ : state) {
        MarketMaker::Order* p = pool.allocate(make_order());
        benchmark::DoNotOptimize(p);
        pool.deallocate(p);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PoolAllocDealloc);

// ─── std::make_shared baseline ───────────────────────────────────────────────

static void BM_MakeSharedOrder(benchmark::State& state) {
    for (auto _ : state) {
        auto p = std::make_shared<MarketMaker::Order>(make_order());
        benchmark::DoNotOptimize(p.get());
        p.reset();
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MakeSharedOrder);

// ─── Raw new/delete baseline ─────────────────────────────────────────────────

static void BM_NewDeleteOrder(benchmark::State& state) {
    for (auto _ : state) {
        MarketMaker::Order* p = new MarketMaker::Order(make_order());
        benchmark::DoNotOptimize(p);
        delete p;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NewDeleteOrder);

// ─── Pool burst: allocate 64, then deallocate all ────────────────────────────

static void BM_PoolBurstAlloc(benchmark::State& state) {
    constexpr size_t kBurst = 64;
    MarketMaker::ObjectPool<MarketMaker::Order, 128> pool;
    std::array<MarketMaker::Order*, kBurst> ptrs{};

    for (auto _ : state) {
        // Allocate burst
        for (size_t i = 0; i < kBurst; ++i) {
            ptrs[i] = pool.allocate(make_order());
            benchmark::DoNotOptimize(ptrs[i]);
        }
        // Deallocate all
        for (size_t i = 0; i < kBurst; ++i) {
            pool.deallocate(ptrs[i]);
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kBurst));
}
BENCHMARK(BM_PoolBurstAlloc);

} // namespace

BENCHMARK_MAIN();
