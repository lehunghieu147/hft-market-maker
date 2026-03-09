#include "network/grpc_server.h"
#include "network/trading_bot_service_impl.h"
#include "core/app_logger.h"

#include <grpcpp/grpcpp.h>
#include <thread>

namespace MarketMaker {

class GrpcServer::Impl {
public:
    Impl(MarketMakerBot& bot, int port)
        : service_(bot), port_(port) {
        logger_ = AppLogger::get("network");
    }

    void start() {
        std::string addr = "127.0.0.1:" + std::to_string(port_);

        grpc::ServerBuilder builder;
        builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
        builder.RegisterService(&service_);

        server_ = builder.BuildAndStart();
        if (!server_) {
            LOG_ERROR(logger_, "Failed to start gRPC server on {}", addr);
            return;
        }

        LOG_INFO(logger_, "gRPC server listening on {}", addr);
        thread_ = std::thread([this]() { server_->Wait(); });
    }

    void stop() {
        if (server_) {
            server_->Shutdown();
            LOG_INFO(logger_, "{}", "gRPC server shutting down");
        }
        if (thread_.joinable()) thread_.join();
    }

private:
    TradingBotServiceImpl service_;
    int port_;
    std::unique_ptr<grpc::Server> server_;
    std::thread thread_;
    quill::Logger* logger_ = nullptr;
};

GrpcServer::GrpcServer(MarketMakerBot& bot, int port)
    : impl_(std::make_unique<Impl>(bot, port)) {}

GrpcServer::~GrpcServer() { stop(); }
void GrpcServer::start() { impl_->start(); }
void GrpcServer::stop() { impl_->stop(); }

} // namespace MarketMaker
