#pragma once

#include <prometheus/registry.h>
#include <prometheus/gauge.h>
#include <prometheus/counter.h>
#include <prometheus/histogram.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>

namespace MarketMaker {

class MetricsCollector {
public:
    static MetricsCollector& instance();

    // Same API as before — callers unchanged
    void set_gauge(const std::string& name, double val);
    void increment(const std::string& name, double val = 1.0);
    void observe(const std::string& name, double val);

    // Expose registry for Exposer to bind
    std::shared_ptr<prometheus::Registry> registry() { return registry_; }

private:
    MetricsCollector();

    std::shared_ptr<prometheus::Registry> registry_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, prometheus::Gauge*> gauges_;
    std::unordered_map<std::string, prometheus::Counter*> counters_;
    std::unordered_map<std::string, prometheus::Histogram*> histograms_;

    prometheus::Gauge& get_or_create_gauge(const std::string& name);
    prometheus::Counter& get_or_create_counter(const std::string& name);
    prometheus::Histogram& get_or_create_histogram(const std::string& name);
};

} // namespace MarketMaker
