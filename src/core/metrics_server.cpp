#include "core/metrics_server.h"
#include "core/metrics_collector.h"
#include "core/app_logger.h"

#include <prometheus/exposer.h>
#include <string>

namespace MarketMaker {

class MetricsServer::Impl {
public:
    explicit Impl(int port) : port_(port) {
        logger_ = AppLogger::get("core");
    }

    void start() {
        exposer_ = std::make_unique<prometheus::Exposer>(
            "0.0.0.0:" + std::to_string(port_));
        exposer_->RegisterCollectable(
            MetricsCollector::instance().registry());
        LOG_INFO(logger_, "Metrics server listening on 0.0.0.0:{}", port_);
    }

    void stop() {
        exposer_.reset();
        LOG_INFO(logger_, "{}", "Metrics server stopped");
    }

private:
    int port_;
    std::unique_ptr<prometheus::Exposer> exposer_;
    quill::Logger* logger_ = nullptr;
};

MetricsServer::MetricsServer(int port) : impl_(std::make_unique<Impl>(port)) {}
MetricsServer::~MetricsServer() { stop(); }
void MetricsServer::start() { impl_->start(); }
void MetricsServer::stop() { impl_->stop(); }

} // namespace MarketMaker
