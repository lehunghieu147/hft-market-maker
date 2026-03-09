#pragma once

#include <string>
#include <mutex>
#include <chrono>

namespace MarketMaker {

class GcpAuthProvider {
public:
    explicit GcpAuthProvider(const std::string& service_account_path);
    ~GcpAuthProvider();

    // Thread-safe; returns cached token or refreshes
    std::string get_access_token();

    const std::string& project_id() const { return project_id_; }
    const std::string& client_email() const { return client_email_; }
    bool is_loaded() const { return loaded_; }

private:
    void load_service_account(const std::string& path);
    std::string create_jwt() const;
    std::string exchange_jwt_for_token(const std::string& jwt);

    std::string project_id_;
    std::string client_email_;
    std::string private_key_pem_;
    std::string token_uri_;

    mutable std::mutex mutex_;
    std::string cached_token_;
    std::chrono::system_clock::time_point token_expiry_;

    bool loaded_ = false;
};

} // namespace MarketMaker
