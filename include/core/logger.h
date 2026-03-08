#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include "quill/Logger.h"

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
    quill::Logger* quill_logger_ = nullptr;
};

} // namespace MarketMaker

#endif // LOGGER_H
