#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include <chrono>
#include <deque>
#include <mutex>
#include <atomic>

namespace MarketMaker {

class RateLimiter {
public:
    RateLimiter(int max_requests_per_second = 10, int burst_size = 20);

    // Check if we can make a request
    bool can_request();

    // Wait until we can make a request
    void wait_if_needed();

    // Record that a request was made
    void record_request();

    // Get current usage stats
    struct Stats {
        int requests_in_last_second;
        int requests_in_last_minute;
        double current_rate;
        bool is_limited;
    };
    Stats get_stats() const;

    // Reset the limiter
    void reset();

private:
    void cleanup_old_requests();

    const int max_requests_per_second_;
    const int burst_size_;
    const std::chrono::milliseconds window_size_{1000};

    mutable std::mutex mutex_;
    std::deque<std::chrono::steady_clock::time_point> request_times_;
    std::atomic<int> request_count_{0};
};

// Global rate limiter for order operations
class OrderRateLimiter {
public:
    static OrderRateLimiter& instance() {
        static OrderRateLimiter instance;
        return instance;
    }

    bool can_place_order() {
        return order_limiter_.can_request();
    }

    bool can_cancel_order() {
        return cancel_limiter_.can_request();
    }

    void wait_for_order_slot() {
        order_limiter_.wait_if_needed();
    }

    void record_order_placed() {
        order_limiter_.record_request();
    }

    void record_order_cancelled() {
        cancel_limiter_.record_request();
    }

    void log_status() const;

private:
    OrderRateLimiter()
        : order_limiter_(10, 20),    // 10 orders/sec, burst of 20
          cancel_limiter_(20, 40) {}  // 20 cancels/sec, burst of 40

    RateLimiter order_limiter_;
    RateLimiter cancel_limiter_;
};

} // namespace MarketMaker

#endif // RATE_LIMITER_H