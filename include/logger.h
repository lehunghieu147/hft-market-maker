#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>
#include <mutex>
#include <memory>
#include <chrono>
#include <queue>
#include <thread>
#include <atomic>
#include <condition_variable>

namespace MarketMaker {

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    CRITICAL
};

class Logger {
public:
    Logger(const std::string& log_file = "market_maker.log", bool verbose = true);
    ~Logger();

    void log(LogLevel level, const std::string& message);
    void log_order_event(const std::string& event, const std::string& details);
    void log_latency(const std::string& operation, double latency_ms);
    void log_connection_event(const std::string& event);

    void set_log_level(LogLevel level);
    void flush();

private:
    std::string log_file_;
    bool verbose_;
    LogLevel min_level_;

    std::ofstream file_stream_;
    std::mutex file_mutex_;

    // Async logging
    std::queue<std::string> log_queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::thread writer_thread_;
    std::atomic<bool> running_;

    void writer_loop();
    std::string format_timestamp();
    std::string level_to_string(LogLevel level);
    std::string format_log_entry(LogLevel level, const std::string& message);
};

} // namespace MarketMaker

#endif // LOGGER_H