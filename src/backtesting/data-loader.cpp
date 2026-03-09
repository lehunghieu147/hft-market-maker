#include "backtesting/data-loader.h"
#include <algorithm>

namespace MarketMaker {

bool DataLoader::open(const std::string& filename) {
    file_.open(filename);
    if (!file_.is_open()) return false;
    lines_read_ = 0;
    header_skipped_ = false;
    return true;
}

bool DataLoader::next(OrderBook& out, double& timestamp_ms) {
    if (!file_.is_open()) return false;

    std::string line;
    while (std::getline(file_, line)) {
        // Skip empty lines and header
        if (line.empty()) continue;
        if (!header_skipped_ && (line[0] < '0' || line[0] > '9')) {
            header_skipped_ = true;
            continue;
        }
        header_skipped_ = true;

        if (parse_line(line, out, timestamp_ms)) {
            lines_read_++;
            return true;
        }
    }
    return false;
}

void DataLoader::reset() {
    if (file_.is_open()) {
        file_.clear();
        file_.seekg(0);
        lines_read_ = 0;
        header_skipped_ = false;
    }
}

bool DataLoader::parse_line(const std::string& line, OrderBook& out, double& timestamp_ms) {
    std::istringstream ss(line);
    std::string token;

    out.bids.clear();
    out.asks.clear();
    out.timestamp = std::chrono::steady_clock::now();

    // First field: timestamp
    if (!std::getline(ss, token, ',')) return false;
    timestamp_ms = std::stod(token);

    // Read bid levels: pairs of (price, quantity)
    for (int i = 0; i < depth_; ++i) {
        double price, qty;
        if (!std::getline(ss, token, ',')) break;
        price = std::stod(token);
        if (!std::getline(ss, token, ',')) break;
        qty = std::stod(token);
        if (price > 0 && qty > 0) {
            out.bids.emplace_back(price, qty);
        }
    }

    // Read ask levels: pairs of (price, quantity)
    for (int i = 0; i < depth_; ++i) {
        double price, qty;
        if (!std::getline(ss, token, ',')) break;
        price = std::stod(token);
        if (!std::getline(ss, token, ',')) break;
        qty = std::stod(token);
        if (price > 0 && qty > 0) {
            out.asks.emplace_back(price, qty);
        }
    }

    return !out.bids.empty() && !out.asks.empty();
}

} // namespace MarketMaker
