#include "core/metrics_collector.h"

namespace MarketMaker {

MetricsCollector& MetricsCollector::instance() {
    static MetricsCollector inst;
    return inst;
}

MetricsCollector::MetricsCollector()
    : registry_(std::make_shared<prometheus::Registry>()) {}

void MetricsCollector::set_gauge(const std::string& name, double val) {
    std::lock_guard<std::mutex> lock(mutex_);
    get_or_create_gauge(name).Set(val);
}

void MetricsCollector::increment(const std::string& name, double val) {
    std::lock_guard<std::mutex> lock(mutex_);
    get_or_create_counter(name).Increment(val);
}

void MetricsCollector::observe(const std::string& name, double val) {
    std::lock_guard<std::mutex> lock(mutex_);
    get_or_create_histogram(name).Observe(val);
}

prometheus::Gauge& MetricsCollector::get_or_create_gauge(const std::string& name) {
    auto it = gauges_.find(name);
    if (it != gauges_.end()) return *it->second;

    auto& gauge = prometheus::BuildGauge()
        .Name(name)
        .Register(*registry_)
        .Add({});
    gauges_[name] = &gauge;
    return gauge;
}

prometheus::Counter& MetricsCollector::get_or_create_counter(const std::string& name) {
    auto it = counters_.find(name);
    if (it != counters_.end()) return *it->second;

    auto& counter = prometheus::BuildCounter()
        .Name(name)
        .Register(*registry_)
        .Add({});
    counters_[name] = &counter;
    return counter;
}

prometheus::Histogram& MetricsCollector::get_or_create_histogram(const std::string& name) {
    auto it = histograms_.find(name);
    if (it != histograms_.end()) return *it->second;

    auto& histogram = prometheus::BuildHistogram()
        .Name(name)
        .Register(*registry_)
        .Add({}, prometheus::Histogram::BucketBoundaries{1, 5, 10, 25, 50, 100, 500});
    histograms_[name] = &histogram;
    return histogram;
}

} // namespace MarketMaker
