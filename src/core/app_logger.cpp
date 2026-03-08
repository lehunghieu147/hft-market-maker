#include "core/app_logger.h"

namespace MarketMaker {

std::once_flag AppLogger::init_flag_;
bool AppLogger::initialized_ = false;

void AppLogger::init(const std::string& log_file, bool console_output) {
    std::call_once(init_flag_, [&]() {
        // Start Quill backend thread
        quill::Backend::start();

        // Create rotating file sink
        auto file_sink = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
            log_file,
            []() {
                quill::RotatingFileSinkConfig cfg;
                cfg.set_open_mode('a');
                cfg.set_rotation_max_file_size(100 * 1024 * 1024); // 100MB
                cfg.set_rotation_time_daily("00:00");
                return cfg;
            }());

        // Create root logger with file sink (+ optional console)
        if (console_output) {
            auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");
            auto root_logger = quill::Frontend::create_or_get_logger(
                "root", {std::move(file_sink), std::move(console_sink)});
            root_logger->set_log_level(quill::LogLevel::Info);
        } else {
            auto root_logger = quill::Frontend::create_or_get_logger(
                "root", std::move(file_sink));
            root_logger->set_log_level(quill::LogLevel::Info);
        }

        initialized_ = true;
    });
}

quill::Logger* AppLogger::get(const std::string& name) {
    if (!initialized_) {
        init(); // Fallback init with defaults
    }

    // Try to get existing logger
    auto* logger = quill::Frontend::get_logger(name);
    if (!logger) {
        // Clone sinks from root logger
        auto* root = quill::Frontend::get_logger("root");
        if (root) {
            logger = quill::Frontend::create_or_get_logger(name, root->get_sinks());
            logger->set_log_level(root->get_log_level());
        }
    }
    return logger;
}

void AppLogger::shutdown() {
    if (initialized_) {
        quill::Backend::stop();
        initialized_ = false;
    }
}

bool AppLogger::is_initialized() {
    return initialized_;
}

} // namespace MarketMaker
