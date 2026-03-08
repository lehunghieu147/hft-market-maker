#include "core/logger.h"
#include "core/app_logger.h"
#include "quill/core/LogLevel.h"

namespace MarketMaker {

Logger::Logger(const std::string& log_file, bool verbose) {
    AppLogger::init(log_file, verbose);
    quill_logger_ = AppLogger::get("root");
}

Logger::~Logger() {
    // No-op: AppLogger owns Quill lifecycle
}

void Logger::log(LogLevel level, const std::string& message) {
    if (!quill_logger_) return;

    switch (level) {
        case LogLevel::DEBUG:
            LOG_DEBUG(quill_logger_, "{}", message);
            break;
        case LogLevel::INFO:
            LOG_INFO(quill_logger_, "{}", message);
            break;
        case LogLevel::WARNING:
            LOG_WARNING(quill_logger_, "{}", message);
            break;
        case LogLevel::ERROR:
            LOG_ERROR(quill_logger_, "{}", message);
            break;
        case LogLevel::CRITICAL:
            LOG_CRITICAL(quill_logger_, "{}", message);
            break;
    }
}

void Logger::log_order_event(const std::string& event, const std::string& details) {
    if (!quill_logger_) return;
    LOG_INFO(quill_logger_, "[ORDER] {} - {}", event, details);
}

void Logger::log_latency(const std::string& operation, double latency_ms) {
    if (!quill_logger_) return;
    LOG_DEBUG(quill_logger_, "[LATENCY] {}: {:.2f} ms", operation, latency_ms);
}

void Logger::log_connection_event(const std::string& event) {
    if (!quill_logger_) return;
    LOG_INFO(quill_logger_, "[CONNECTION] {}", event);
}

void Logger::set_log_level(LogLevel level) {
    if (!quill_logger_) return;

    switch (level) {
        case LogLevel::DEBUG:    quill_logger_->set_log_level(quill::LogLevel::Debug); break;
        case LogLevel::INFO:     quill_logger_->set_log_level(quill::LogLevel::Info); break;
        case LogLevel::WARNING:  quill_logger_->set_log_level(quill::LogLevel::Warning); break;
        case LogLevel::ERROR:    quill_logger_->set_log_level(quill::LogLevel::Error); break;
        case LogLevel::CRITICAL: quill_logger_->set_log_level(quill::LogLevel::Critical); break;
    }
}

void Logger::flush() {
    if (quill_logger_) {
        quill_logger_->flush_log();
    }
}

} // namespace MarketMaker
