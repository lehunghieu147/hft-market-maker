#include "logger.h"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace MarketMaker {

Logger::Logger(const std::string& log_file, bool verbose)
    : log_file_(log_file), verbose_(verbose), min_level_(LogLevel::INFO), running_(true) {

    // Open log file
    file_stream_.open(log_file_, std::ios::app);
    if (!file_stream_.is_open()) {
        std::cerr << "Failed to open log file: " << log_file_ << std::endl;
    }

    // Start writer thread for async logging
    writer_thread_ = std::thread(&Logger::writer_loop, this);

    //log(LogLevel::INFO, "Logger initialized - Log file: " + log_file_);
}

Logger::~Logger() {
    running_ = false;
    cv_.notify_all();

    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    flush();

    if (file_stream_.is_open()) {
        file_stream_.close();
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < min_level_) {
        return;
    }

    std::string formatted = format_log_entry(level, message);

    // Console output if verbose
    if (verbose_) {
        if (level >= LogLevel::WARNING) {
            std::cerr << formatted << std::endl;
        } else {
            std::cout << formatted << std::endl;
        }
    }

    // Queue for file writing
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        log_queue_.push(formatted);
    }
    cv_.notify_one();
}

void Logger::log_order_event(const std::string& event, const std::string& details) {
    std::stringstream ss;
    ss << "[ORDER] " << event << " - " << details;
    log(LogLevel::INFO, ss.str());
}

void Logger::log_latency(const std::string& operation, double latency_ms) {
    std::stringstream ss;
    ss << "[LATENCY] " << operation << ": " << std::fixed << std::setprecision(2)
       << latency_ms << " ms";
    log(LogLevel::DEBUG, ss.str());
}

void Logger::log_connection_event(const std::string& event) {
    std::stringstream ss;
    ss << "[CONNECTION] " << event;
    log(LogLevel::INFO, ss.str());
}

void Logger::set_log_level(LogLevel level) {
    min_level_ = level;
}

void Logger::flush() {
    // Process all remaining logs
    std::queue<std::string> temp_queue;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        temp_queue.swap(log_queue_);
    }

    std::lock_guard<std::mutex> file_lock(file_mutex_);
    while (!temp_queue.empty()) {
        if (file_stream_.is_open()) {
            file_stream_ << temp_queue.front() << std::endl;
        }
        temp_queue.pop();
    }

    if (file_stream_.is_open()) {
        file_stream_.flush();
    }
}

void Logger::writer_loop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(100),
                     [this] { return !log_queue_.empty() || !running_; });

        if (log_queue_.empty()) {
            continue;
        }

        std::queue<std::string> temp_queue;
        temp_queue.swap(log_queue_);
        lock.unlock();

        // Write to file
        std::lock_guard<std::mutex> file_lock(file_mutex_);
        while (!temp_queue.empty()) {
            if (file_stream_.is_open()) {
                file_stream_ << temp_queue.front() << std::endl;
            }
            temp_queue.pop();
        }

        if (file_stream_.is_open()) {
            file_stream_.flush();
        }
    }
}

std::string Logger::format_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();

    return ss.str();
}

std::string Logger::level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:    return "DEBUG";
        case LogLevel::INFO:     return "INFO";
        case LogLevel::WARNING:  return "WARN";
        case LogLevel::ERROR:    return "ERROR";
        case LogLevel::CRITICAL: return "CRIT";
        default:                 return "UNKNOWN";
    }
}

std::string Logger::format_log_entry(LogLevel level, const std::string& message) {
    std::stringstream ss;
    ss << "[" << format_timestamp() << "] ";
    ss << "[" << level_to_string(level) << "] ";
    ss << message;
    return ss.str();
}

} // namespace MarketMaker