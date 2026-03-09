#include "core/metrics_server.h"
#include "core/metrics_collector.h"
#include "core/app_logger.h"
#include "httplib.h"

#include <thread>
#include <chrono>

namespace MarketMaker {

class MetricsServer::Impl {
public:
    explicit Impl(int port) : port_(port) {
        logger_ = AppLogger::get("core");
    }

    void start() {
        server_.Get("/metrics", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(MetricsCollector::instance().render(),
                            "text/plain; version=0.0.4; charset=utf-8");
        });

        auto start_time = std::chrono::steady_clock::now();
        server_.Get("/health", [start_time](const httplib::Request&, httplib::Response& res) {
            auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            std::string body = R"({"status":"ok","uptime_ms":)" + std::to_string(uptime_ms) + "}";
            res.set_content(body, "application/json");
        });

        thread_ = std::thread([this]() {
            LOG_INFO(logger_, "Metrics server listening on 0.0.0.0:{}", port_);
            server_.listen("0.0.0.0", port_);
        });
    }

    void stop() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
        LOG_INFO(logger_, "{}", "Metrics server stopped");
    }

private:
    httplib::Server server_;
    int port_;
    std::thread thread_;
    quill::Logger* logger_ = nullptr;
};

MetricsServer::MetricsServer(int port) : impl_(std::make_unique<Impl>(port)) {}
MetricsServer::~MetricsServer() { stop(); }
void MetricsServer::start() { impl_->start(); }
void MetricsServer::stop() { impl_->stop(); }

} // namespace MarketMaker
