#pragma once

#include "core/gcp_auth_provider.h"
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstdint>

namespace MarketMaker {

class GcpPublisher {
public:
    GcpPublisher(GcpAuthProvider& auth, const std::string& project_id,
                 const std::string& topic);
    ~GcpPublisher();

    // Non-blocking enqueue (called from trading thread)
    void publish(const std::string& event_json);

    void start();
    void stop();

    uint64_t messages_sent() const { return sent_count_.load(); }
    uint64_t messages_failed() const { return fail_count_.load(); }

private:
    void worker_loop();
    bool send_to_pubsub(const std::string& message_data);

    GcpAuthProvider& auth_;
    std::string project_id_;
    std::string topic_;
    std::string publish_url_;

    static constexpr size_t MAX_QUEUE_SIZE = 10000;
    std::queue<std::string> queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::thread worker_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> sent_count_{0};
    std::atomic<uint64_t> fail_count_{0};
};

} // namespace MarketMaker
