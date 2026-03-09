/// Benchmark: SPSCRingBuffer throughput vs mutex-based queue baseline.
/// Measures single-thread push/pop latency and multi-thread producer/consumer
/// throughput for the lock-free SPSC ring buffer in include/core/spsc-ring-buffer.h

#include <benchmark/benchmark.h>
#include "core/spsc-ring-buffer.h"

#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

namespace {

// Simple payload representative of market data updates
struct Payload {
    double price;
    int64_t seq;
};

static_assert(std::is_nothrow_move_constructible_v<Payload>);

constexpr size_t kRingSize = 1024;  // Must be power of 2

// ─── Single-thread: push then immediately pop ───────────────────────────────

static void BM_SPSCPushPop(benchmark::State& state) {
    MarketMaker::SPSCRingBuffer<Payload, kRingSize> ring;
    Payload out{};

    for (auto _ : state) {
        Payload p{42.0, 1};
        benchmark::DoNotOptimize(ring.try_push(p));
        benchmark::DoNotOptimize(ring.try_pop(out));
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCPushPop);

// ─── Two-thread: producer pushes N items, consumer pops N items ─────────────

static void BM_SPSCProducerConsumer(benchmark::State& state) {
    MarketMaker::SPSCRingBuffer<Payload, kRingSize> ring;
    const int64_t n = state.range(0);

    for (auto _ : state) {
        std::atomic<int64_t> consumed{0};

        // Consumer thread
        std::thread consumer([&] {
            Payload out{};
            int64_t count = 0;
            while (count < n) {
                if (ring.try_pop(out)) {
                    benchmark::DoNotOptimize(out);
                    ++count;
                }
            }
            consumed.store(count, std::memory_order_release);
        });

        // Producer (this thread)
        for (int64_t i = 0; i < n; ++i) {
            Payload p{static_cast<double>(i), i};
            while (!ring.try_push(p)) { /* spin - buffer full */ }
        }

        consumer.join();
        benchmark::DoNotOptimize(consumed.load());
    }

    state.SetItemsProcessed(state.iterations() * n);
    state.SetBytesProcessed(state.iterations() * n * static_cast<int64_t>(sizeof(Payload)));
}
BENCHMARK(BM_SPSCProducerConsumer)->Arg(4096)->Arg(65536);

// ─── Baseline: same pattern with std::queue + std::mutex ────────────────────

struct MutexQueue {
    std::mutex mtx;
    std::queue<Payload> q;

    bool try_push(const Payload& p) {
        std::lock_guard<std::mutex> lk(mtx);
        q.push(p);
        return true;
    }

    bool try_pop(Payload& out) {
        std::lock_guard<std::mutex> lk(mtx);
        if (q.empty()) return false;
        out = q.front();
        q.pop();
        return true;
    }
};

static void BM_MutexQueueBaseline(benchmark::State& state) {
    MutexQueue mq;
    const int64_t n = state.range(0);

    for (auto _ : state) {
        std::atomic<int64_t> consumed{0};

        std::thread consumer([&] {
            Payload out{};
            int64_t count = 0;
            while (count < n) {
                if (mq.try_pop(out)) {
                    benchmark::DoNotOptimize(out);
                    ++count;
                }
            }
            consumed.store(count, std::memory_order_release);
        });

        for (int64_t i = 0; i < n; ++i) {
            Payload p{static_cast<double>(i), i};
            mq.try_push(p);
        }

        consumer.join();
        benchmark::DoNotOptimize(consumed.load());
    }

    state.SetItemsProcessed(state.iterations() * n);
    state.SetBytesProcessed(state.iterations() * n * static_cast<int64_t>(sizeof(Payload)));
}
BENCHMARK(BM_MutexQueueBaseline)->Arg(4096)->Arg(65536);

} // namespace

BENCHMARK_MAIN();
