#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <sstream>
#include <cstdint>
#include <iomanip>

namespace MarketMaker {

class MetricsCollector {
public:
    static MetricsCollector& instance() {
        static MetricsCollector inst;
        return inst;
    }

    // Counter: monotonic increment
    void increment(const std::string& name, double val = 1.0) {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_[name] += val;
    }

    // Gauge: set to arbitrary value
    void set_gauge(const std::string& name, double val) {
        std::lock_guard<std::mutex> lock(mutex_);
        gauges_[name] = val;
    }

    // Histogram: observe a value into pre-defined buckets
    void observe(const std::string& name, double val) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& h = histograms_[name];
        h.sum += val;
        h.count++;
        for (size_t i = 0; i < h.bucket_bounds.size(); ++i) {
            if (val <= h.bucket_bounds[i]) {
                h.bucket_counts[i]++;
            }
        }
        h.bucket_counts.back()++; // +Inf always incremented
    }

    // Render all metrics in Prometheus text exposition format
    std::string render() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(6);

        for (const auto& [name, val] : counters_) {
            ss << "# TYPE " << name << " counter\n";
            ss << name << " " << val << "\n";
        }

        for (const auto& [name, val] : gauges_) {
            ss << "# TYPE " << name << " gauge\n";
            ss << name << " " << val << "\n";
        }

        for (const auto& [name, h] : histograms_) {
            ss << "# TYPE " << name << " histogram\n";
            for (size_t i = 0; i < h.bucket_bounds.size(); ++i) {
                ss << name << "_bucket{le=\"" << h.bucket_bounds[i] << "\"} "
                   << h.bucket_counts[i] << "\n";
            }
            ss << name << "_bucket{le=\"+Inf\"} " << h.bucket_counts.back() << "\n";
            ss << name << "_sum " << h.sum << "\n";
            ss << name << "_count " << h.count << "\n";
        }

        return ss.str();
    }

private:
    MetricsCollector() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, double> counters_;
    std::unordered_map<std::string, double> gauges_;

    struct Histogram {
        std::vector<double> bucket_bounds = {1, 5, 10, 25, 50, 100, 500};
        std::vector<uint64_t> bucket_counts; // bounds.size() + 1 for +Inf
        double sum = 0;
        uint64_t count = 0;
        Histogram() : bucket_counts(8, 0) {}
    };
    std::unordered_map<std::string, Histogram> histograms_;
};

} // namespace MarketMaker
