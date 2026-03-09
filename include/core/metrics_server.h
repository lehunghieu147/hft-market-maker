#pragma once

#include <memory>

namespace MarketMaker {

class MetricsServer {
public:
    explicit MetricsServer(int port = 8888);
    ~MetricsServer();

    void start();
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace MarketMaker
