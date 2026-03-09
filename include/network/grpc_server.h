#pragma once

#include <memory>

namespace MarketMaker {

class MarketMakerBot;

class GrpcServer {
public:
    GrpcServer(MarketMakerBot& bot, int port = 50051);
    ~GrpcServer();

    void start();
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace MarketMaker
