#ifndef APP_LOGGER_H
#define APP_LOGGER_H

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/Logger.h"
#include "quill/LogMacros.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/RotatingFileSink.h"
#include <string>
#include <mutex>

namespace MarketMaker {

class AppLogger {
public:
    // Initialize logging system. Call once at startup.
    static void init(const std::string& log_file = "logs/market_maker.log",
                     bool console_output = true);

    // Get a named logger (creates if not exists)
    static quill::Logger* get(const std::string& name = "root");

    // Shutdown logging (flush + stop backend)
    static void shutdown();

    // Check if initialized
    static bool is_initialized();

private:
    AppLogger() = default;
    static std::once_flag init_flag_;
    static bool initialized_;
};

} // namespace MarketMaker

#endif // APP_LOGGER_H
