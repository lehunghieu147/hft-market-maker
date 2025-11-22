#include "rate_limiter.h"
#include <thread>
#include <iostream>
#include <algorithm>

namespace MarketMaker {

RateLimiter::RateLimiter(int max_requests_per_second, int burst_size)
    : max_requests_per_second_(max_requests_per_second),
      burst_size_(burst_size) {}

bool RateLimiter::can_request() {
    std::lock_guard<std::mutex> lock(mutex_);
    cleanup_old_requests();

    // Check burst limit
    if (request_times_.size() >= static_cast<size_t>(burst_size_)) {
        return false;
    }

    // Check rate limit
    auto now = std::chrono::steady_clock::now();
    auto one_second_ago = now - std::chrono::seconds(1);

    int recent_requests = std::count_if(request_times_.begin(), request_times_.end(),
        [one_second_ago](const auto& time) {
            return time > one_second_ago;
        });

    return recent_requests < max_requests_per_second_;
}

void RateLimiter::wait_if_needed() {
    while (!can_request()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void RateLimiter::record_request() {
    std::lock_guard<std::mutex> lock(mutex_);
    request_times_.push_back(std::chrono::steady_clock::now());
    request_count_++;
}

void RateLimiter::cleanup_old_requests() {
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::seconds(60);  // Keep last 60 seconds

    while (!request_times_.empty() && request_times_.front() < cutoff) {
        request_times_.pop_front();
    }
}

RateLimiter::Stats RateLimiter::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    auto now = std::chrono::steady_clock::now();
    auto one_second_ago = now - std::chrono::seconds(1);
    auto one_minute_ago = now - std::chrono::seconds(60);

    stats.requests_in_last_second = std::count_if(request_times_.begin(), request_times_.end(),
        [one_second_ago](const auto& time) {
            return time > one_second_ago;
        });

    stats.requests_in_last_minute = std::count_if(request_times_.begin(), request_times_.end(),
        [one_minute_ago](const auto& time) {
            return time > one_minute_ago;
        });

    stats.current_rate = static_cast<double>(stats.requests_in_last_second);
    stats.is_limited = stats.requests_in_last_second >= max_requests_per_second_;

    return stats;
}

void RateLimiter::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    request_times_.clear();
    request_count_ = 0;
}

void OrderRateLimiter::log_status() const {
    auto order_stats = order_limiter_.get_stats();
    auto cancel_stats = cancel_limiter_.get_stats();

    std::cout << "[RATE LIMIT] Orders: " << order_stats.requests_in_last_second
              << "/s (limit: 10/s), Cancels: " << cancel_stats.requests_in_last_second
              << "/s (limit: 20/s)";

    if (order_stats.is_limited || cancel_stats.is_limited) {
        std::cout << " [THROTTLED]";
    }
    std::cout << std::endl;
}

} // namespace MarketMaker