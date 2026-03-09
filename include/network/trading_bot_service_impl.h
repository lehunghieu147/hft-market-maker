#pragma once

#include "trading_bot.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace MarketMaker {

class MarketMakerBot;

class TradingBotServiceImpl final : public trading::TradingBotControl::Service {
public:
    explicit TradingBotServiceImpl(MarketMakerBot& bot);

    grpc::Status GetStatus(grpc::ServerContext* ctx,
                          const trading::Empty* req,
                          trading::StatusResponse* resp) override;

    grpc::Status SetConfig(grpc::ServerContext* ctx,
                          const trading::ConfigUpdate* req,
                          trading::Ack* resp) override;

    grpc::Status StartTrading(grpc::ServerContext* ctx,
                             const trading::Empty* req,
                             trading::Ack* resp) override;

    grpc::Status StopTrading(grpc::ServerContext* ctx,
                            const trading::Empty* req,
                            trading::Ack* resp) override;

    grpc::Status KillSwitch(grpc::ServerContext* ctx,
                           const trading::Empty* req,
                           trading::Ack* resp) override;

    grpc::Status StreamMetrics(grpc::ServerContext* ctx,
                              const trading::MetricsStreamRequest* req,
                              grpc::ServerWriter<trading::MetricsUpdate>* writer) override;

private:
    MarketMakerBot& bot_;
};

} // namespace MarketMaker
