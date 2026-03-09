#ifndef DATA_LOADER_H
#define DATA_LOADER_H

#include "core/types.h"
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <chrono>

namespace MarketMaker {

// Loads L2 orderbook snapshots from CSV files.
// Expected CSV format: timestamp_ms,bid1_price,bid1_qty,bid2_price,bid2_qty,...,ask1_price,ask1_qty,...
// Or simplified: timestamp_ms,best_bid,bid_qty,best_ask,ask_qty (for 1-level data)
class DataLoader {
public:
    explicit DataLoader(int depth = 5) : depth_(depth) {}

    // Open CSV file for reading
    bool open(const std::string& filename);

    // Read next orderbook snapshot. Returns false when EOF.
    bool next(OrderBook& out, double& timestamp_ms);

    // Reset to beginning of file
    void reset();

    // Total lines read
    int lines_read() const { return lines_read_; }

    bool is_open() const { return file_.is_open(); }

private:
    std::ifstream file_;
    int depth_;
    int lines_read_ = 0;
    bool header_skipped_ = false;

    // Parse a single CSV line into an OrderBook
    bool parse_line(const std::string& line, OrderBook& out, double& timestamp_ms);
};

} // namespace MarketMaker

#endif // DATA_LOADER_H
