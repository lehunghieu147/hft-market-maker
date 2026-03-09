#include "network/trading_bot_service_impl.h"
#include "trading/market_maker.h"
#include "core/app_logger.h"
#include <chrono>
#include <thread>

namespace {
    quill::Logger* get_logger() {
        static quill::Logger* logger = MarketMaker::AppLogger::get("network");
        return logger;
    }
} // anonymous namespace

namespace MarketMaker {

TradingBotServiceImpl::TradingBotServiceImpl(MarketMakerBot& bot) : bot_(bot) {}

grpc::Status TradingBotServiceImpl::GetStatus(
    grpc::ServerContext*, const trading::Empty*, trading::StatusResponse* resp) {

    auto metrics = bot_.get_metrics();
    auto [bid, ask] = bot_.get_active_orders();

    resp->set_net_position(bot_.get_position());
    resp->set_daily_pnl(bot_.get_daily_pnl());
    resp->set_is_trading(bot_.is_running());
    resp->set_active_orders((bid ? 1 : 0) + (ask ? 1 : 0));
    resp->set_symbol(bot_.get_symbol());
    resp->set_spread_pct(bot_.get_spread_percentage());
    resp->set_kill_switch_active(bot_.is_kill_switch_active());
    resp->set_total_pnl(bot_.get_total_pnl());
    resp->set_fees_paid(bot_.get_fees_paid());

    auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - metrics.start_time).count();
    resp->set_uptime_ms(uptime);

    LOG_DEBUG(get_logger(), "{}", "gRPC GetStatus called");
    return grpc::Status::OK;
}

grpc::Status TradingBotServiceImpl::SetConfig(
    grpc::ServerContext*, const trading::ConfigUpdate* req, trading::Ack* resp) {

    if (req->has_spread_percentage()) {
        double val = req->spread_percentage();
        if (val <= 0 || val > 0.1) {
            resp->set_success(false);
            resp->set_message("spread_percentage must be in (0, 0.1]");
            return grpc::Status::OK;
        }
        bot_.set_spread_percentage(val);
    }
    if (req->has_order_size()) {
        double val = req->order_size();
        if (val <= 0) {
            resp->set_success(false);
            resp->set_message("order_size must be positive");
            return grpc::Status::OK;
        }
        bot_.set_order_size(val);
    }

    resp->set_success(true);
    resp->set_message("Config updated");
    LOG_INFO(get_logger(), "{}", "gRPC SetConfig applied");
    return grpc::Status::OK;
}

grpc::Status TradingBotServiceImpl::StartTrading(
    grpc::ServerContext*, const trading::Empty*, trading::Ack* resp) {
    if (bot_.is_running()) {
        resp->set_success(false);
        resp->set_message("Already running");
    } else {
        bot_.run();
        resp->set_success(true);
        resp->set_message("Trading started");
    }
    return grpc::Status::OK;
}

grpc::Status TradingBotServiceImpl::StopTrading(
    grpc::ServerContext*, const trading::Empty*, trading::Ack* resp) {
    if (!bot_.is_running()) {
        resp->set_success(false);
        resp->set_message("Already stopped");
    } else {
        bot_.stop();
        resp->set_success(true);
        resp->set_message("Trading stopped");
    }
    return grpc::Status::OK;
}

grpc::Status TradingBotServiceImpl::KillSwitch(
    grpc::ServerContext*, const trading::Empty*, trading::Ack* resp) {
    bot_.activate_kill_switch("gRPC kill switch triggered");
    resp->set_success(true);
    resp->set_message("Kill switch activated");
    LOG_WARNING(get_logger(), "{}", "gRPC KillSwitch activated");
    return grpc::Status::OK;
}

grpc::Status TradingBotServiceImpl::StreamMetrics(
    grpc::ServerContext* ctx, const trading::MetricsStreamRequest* req,
    grpc::ServerWriter<trading::MetricsUpdate>* writer) {

    int interval_ms = req->interval_ms() > 0 ? req->interval_ms() : 500;
    LOG_INFO(get_logger(), "gRPC StreamMetrics started (interval={}ms)", interval_ms);

    while (!ctx->IsCancelled()) {
        trading::MetricsUpdate update;

        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        update.set_timestamp_ms(now_ms);
        update.set_mid_price(bot_.get_mid_price());
        update.set_net_position(bot_.get_position());
        update.set_daily_pnl(bot_.get_daily_pnl());

        auto metrics = bot_.get_metrics();
        update.set_orders_placed(metrics.total_orders);
        update.set_orders_filled(metrics.successful_orders);
        update.set_latency_avg_ms(metrics.avg_order_latency_ms);

        auto [bid, ask] = bot_.get_active_orders();
        if (bid) update.set_bid_price(bid->price);
        if (ask) update.set_ask_price(ask->price);

        if (!writer->Write(update)) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    LOG_INFO(get_logger(), "{}", "gRPC StreamMetrics ended");
    return grpc::Status::OK;
}

} // namespace MarketMaker
