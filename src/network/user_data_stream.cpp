#include "network/user_data_stream.h"
#include "core/app_logger.h"
#include <cstring>
#include <curl/curl.h>
#include <openssl/ssl.h>
#include <json/json.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <random>
#include <algorithm>

namespace {
    quill::Logger* get_logger() {
        static quill::Logger* logger = MarketMaker::AppLogger::get("network");
        return logger;
    }
}

namespace MarketMaker {

// CURL write callback
static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t total = size * nmemb;
    output->append(static_cast<char*>(contents), total);
    return total;
}

UserDataStream::UserDataStream(const std::string& api_key,
                               const std::string& api_secret,
                               const std::string& rest_base_url,
                               const std::string& ws_base_url)
    : api_key_(api_key), api_secret_(api_secret),
      rest_base_url_(rest_base_url), ws_base_url_(ws_base_url) {}

UserDataStream::~UserDataStream() {
    stop();
}

void UserDataStream::set_fill_callback(FillCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    fill_callback_ = std::move(callback);
}

void UserDataStream::set_balance_callback(BalanceCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    balance_callback_ = std::move(callback);
}

bool UserDataStream::start() {
    if (running_.load()) return true;

    if (!create_listen_key()) {
        LOG_ERROR(get_logger(), "{}", "[USER_STREAM] Failed to create listen key");
        return false;
    }

    running_.store(true);

    stream_thread_ = std::thread([this]() { stream_loop(); });
    keepalive_thread_ = std::thread([this]() { keepalive_loop(); });

    LOG_INFO(get_logger(), "[USER_STREAM] Started with listen key: {}...", listen_key_.substr(0, 8));
    return true;
}

void UserDataStream::stop() {
    if (!running_.exchange(false)) return;

    connected_.store(false);

    // Shutdown socket to unblock SSL_read (prevents use-after-free)
    {
        std::lock_guard<std::mutex> lock(ssl_mutex_);
        if (socket_fd_ >= 0) {
            ::shutdown(socket_fd_, SHUT_RDWR);
        }
    }

    // Join threads BEFORE freeing SSL resources
    if (stream_thread_.joinable()) stream_thread_.join();
    if (keepalive_thread_.joinable()) keepalive_thread_.join();

    // Now safe to free SSL resources
    {
        std::lock_guard<std::mutex> lock(ssl_mutex_);
        if (ssl_) {
            SSL_free(static_cast<SSL*>(ssl_));
            ssl_ = nullptr;
        }
        if (ssl_ctx_) {
            SSL_CTX_free(static_cast<SSL_CTX*>(ssl_ctx_));
            ssl_ctx_ = nullptr;
        }
        if (socket_fd_ >= 0) {
            ::close(socket_fd_);
            socket_fd_ = -1;
        }
    }

    delete_listen_key();
    LOG_INFO(get_logger(), "{}", "[USER_STREAM] Stopped");
}

bool UserDataStream::create_listen_key() {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = rest_base_url_ + "/api/v3/userDataStream";
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key_).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR(get_logger(), "[USER_STREAM] CURL error creating listen key: {}", curl_easy_strerror(res));
        return false;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream stream(response);
    std::string errors;
    if (!Json::parseFromStream(builder, stream, &root, &errors)) {
        LOG_ERROR(get_logger(), "{}", "[USER_STREAM] Failed to parse listen key response");
        return false;
    }

    if (root.isMember("listenKey")) {
        listen_key_ = root["listenKey"].asString();
        return true;
    }

    LOG_ERROR(get_logger(), "[USER_STREAM] No listenKey in response: {}", response);
    return false;
}

bool UserDataStream::keepalive_listen_key() {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = rest_base_url_ + "/api/v3/userDataStream?listenKey=" + listen_key_;
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key_).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return res == CURLE_OK;
}

void UserDataStream::delete_listen_key() {
    if (listen_key_.empty()) return;

    CURL* curl = curl_easy_init();
    if (!curl) return;

    std::string url = rest_base_url_ + "/api/v3/userDataStream?listenKey=" + listen_key_;
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key_).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    listen_key_.clear();
}

bool UserDataStream::connect_websocket() {
    // Parse ws_base_url to get host
    // ws_base_url_ format: wss://stream.binance.com:9443/ws
    std::string host = "stream.binance.com";
    int port = 9443;
    std::string path = "/ws/" + listen_key_;

    // Extract host from URL
    auto pos = ws_base_url_.find("://");
    if (pos != std::string::npos) {
        std::string remainder = ws_base_url_.substr(pos + 3);
        auto colon_pos = remainder.find(':');
        auto slash_pos = remainder.find('/');
        if (colon_pos != std::string::npos && colon_pos < slash_pos) {
            host = remainder.substr(0, colon_pos);
            try {
                port = std::stoi(remainder.substr(colon_pos + 1, slash_pos - colon_pos - 1));
            } catch (...) {
                port = 443;
            }
        } else if (slash_pos != std::string::npos) {
            host = remainder.substr(0, slash_pos);
            port = 443;
        }
    }

    // DNS resolution
    struct addrinfo hints{}, *result;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(port);

    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        LOG_ERROR(get_logger(), "[USER_STREAM] DNS resolution failed for: {}", host);
        return false;
    }

    socket_fd_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (socket_fd_ < 0) {
        freeaddrinfo(result);
        return false;
    }

    if (::connect(socket_fd_, result->ai_addr, result->ai_addrlen) < 0) {
        freeaddrinfo(result);
        ::close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    freeaddrinfo(result);

    // Helper lambda to clean up on failure
    auto cleanup = [this]() {
        if (ssl_) { SSL_free(static_cast<SSL*>(ssl_)); ssl_ = nullptr; }
        if (ssl_ctx_) { SSL_CTX_free(static_cast<SSL_CTX*>(ssl_ctx_)); ssl_ctx_ = nullptr; }
        if (socket_fd_ >= 0) { ::close(socket_fd_); socket_fd_ = -1; }
    };

    // SSL setup with certificate verification (C2 fix)
    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) {
        ::close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    SSL_CTX_set_default_verify_paths(static_cast<SSL_CTX*>(ssl_ctx_));
    SSL_CTX_set_verify(static_cast<SSL_CTX*>(ssl_ctx_), SSL_VERIFY_PEER, nullptr);

    ssl_ = SSL_new(static_cast<SSL_CTX*>(ssl_ctx_));
    SSL_set_fd(static_cast<SSL*>(ssl_), socket_fd_);
    SSL_set_tlsext_host_name(static_cast<SSL*>(ssl_), host.c_str());

    if (SSL_connect(static_cast<SSL*>(ssl_)) <= 0) {
        LOG_ERROR(get_logger(), "{}", "[USER_STREAM] SSL handshake failed");
        cleanup();
        return false;
    }

    // WebSocket upgrade request
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    std::string key_bytes(16, '\0');
    for (auto& b : key_bytes) b = static_cast<char>(dis(gen));

    // Base64 encode (simplified)
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ws_key;
    for (size_t i = 0; i < key_bytes.size(); i += 3) {
        uint32_t n = (static_cast<uint8_t>(key_bytes[i]) << 16);
        if (i + 1 < key_bytes.size()) n |= (static_cast<uint8_t>(key_bytes[i + 1]) << 8);
        if (i + 2 < key_bytes.size()) n |= static_cast<uint8_t>(key_bytes[i + 2]);
        ws_key += b64[(n >> 18) & 0x3F];
        ws_key += b64[(n >> 12) & 0x3F];
        ws_key += (i + 1 < key_bytes.size()) ? b64[(n >> 6) & 0x3F] : '=';
        ws_key += (i + 2 < key_bytes.size()) ? b64[n & 0x3F] : '=';
    }

    std::string request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + ":" + port_str + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + ws_key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    if (SSL_write(static_cast<SSL*>(ssl_), request.c_str(),
                  static_cast<int>(request.size())) <= 0) {
        cleanup();
        return false;
    }

    // Read upgrade response
    char buf[4096];
    int bytes = SSL_read(static_cast<SSL*>(ssl_), buf, sizeof(buf) - 1);
    if (bytes <= 0) {
        cleanup();
        return false;
    }
    buf[bytes] = '\0';

    std::string response(buf);
    if (response.find("101") == std::string::npos) {
        LOG_ERROR(get_logger(), "[USER_STREAM] WebSocket upgrade failed: {}", response);
        cleanup();
        return false;
    }

    connected_.store(true);
    LOG_INFO(get_logger(), "{}", "[USER_STREAM] WebSocket connected");
    return true;
}

std::string UserDataStream::read_websocket_frame(std::vector<uint8_t>& ping_payload, bool& is_ping) {
    is_ping = false;
    ping_payload.clear();

    SSL* ssl_ptr = static_cast<SSL*>(ssl_);
    if (!ssl_ptr) return "";

    // Read first 2 bytes (header)
    uint8_t header[2];
    int n = SSL_read(ssl_ptr, header, 2);
    if (n <= 0) return "";

    uint8_t opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    // Extended payload length
    if (payload_len == 126) {
        uint8_t ext[2];
        if (SSL_read(ssl_ptr, ext, 2) != 2) return "";
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (SSL_read(ssl_ptr, ext, 8) != 8) return "";
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | ext[i];
        }
    }

    // Reject oversized frames (max 16 MB)
    constexpr uint64_t MAX_PAYLOAD = 16 * 1024 * 1024;
    if (payload_len > MAX_PAYLOAD) {
        LOG_ERROR(get_logger(), "[USER_STREAM] Rejecting oversized frame: {} bytes", payload_len);
        connected_.store(false);
        return "";
    }

    // Read masking key if present
    uint8_t mask_key[4] = {0};
    if (masked) {
        if (SSL_read(ssl_ptr, mask_key, 4) != 4) return "";
    }

    // Read payload
    std::vector<uint8_t> payload(payload_len);
    uint64_t total_read = 0;
    while (total_read < payload_len) {
        int chunk = SSL_read(ssl_ptr, payload.data() + total_read,
                             static_cast<int>(std::min(payload_len - total_read, uint64_t(4096))));
        if (chunk <= 0) return "";
        total_read += chunk;
    }

    // Unmask if needed
    if (masked) {
        for (uint64_t i = 0; i < payload_len; ++i) {
            payload[i] ^= mask_key[i % 4];
        }
    }

    // Handle ping (opcode 0x9) - echo payload back in pong per RFC 6455
    if (opcode == 0x9) {
        is_ping = true;
        ping_payload = payload;
        return "";
    }

    // Handle close (opcode 0x8)
    if (opcode == 0x8) {
        connected_.store(false);
        return "";
    }

    // Text frame (opcode 0x1)
    if (opcode == 0x1) {
        return std::string(payload.begin(), payload.end());
    }

    return "";
}

bool UserDataStream::send_pong(const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(ssl_mutex_);
    SSL* ssl_ptr = static_cast<SSL*>(ssl_);
    if (!ssl_ptr) return false;

    // Build pong frame (opcode 0xA, masked per client requirement)
    std::vector<uint8_t> frame;
    frame.push_back(0x8A); // FIN + pong opcode

    // Client frames must be masked
    size_t plen = payload.size();
    if (plen < 126) {
        frame.push_back(static_cast<uint8_t>(plen | 0x80)); // masked
    } else if (plen < 65536) {
        frame.push_back(0xFE); // 126 | masked
        frame.push_back(static_cast<uint8_t>((plen >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(plen & 0xFF));
    }

    // Generate mask key
    std::random_device rd;
    uint8_t mask[4];
    for (auto& m : mask) m = static_cast<uint8_t>(rd());
    frame.insert(frame.end(), mask, mask + 4);

    // Masked payload (echo ping payload per RFC 6455)
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(payload[i] ^ mask[i % 4]);
    }

    int written = SSL_write(ssl_ptr, frame.data(), static_cast<int>(frame.size()));
    return written > 0;
}

void UserDataStream::stream_loop() {
    while (running_.load()) {
        if (!connected_.load()) {
            if (!connect_websocket()) {
                LOG_ERROR(get_logger(), "{}", "[USER_STREAM] Connection failed, retrying in 5s...");
                for (int i = 0; i < 50 && running_.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
        }

        std::vector<uint8_t> ping_payload;
        bool is_ping = false;
        std::string message = read_websocket_frame(ping_payload, is_ping);

        if (is_ping) {
            send_pong(ping_payload);
            continue;
        }

        if (!message.empty()) {
            process_message(message);
        }

        if (!connected_.load() && running_.load()) {
            LOG_ERROR(get_logger(), "{}", "[USER_STREAM] Disconnected, reconnecting...");
            // Clean up old connection
            {
                std::lock_guard<std::mutex> lock(ssl_mutex_);
                if (ssl_) { SSL_free(static_cast<SSL*>(ssl_)); ssl_ = nullptr; }
                if (ssl_ctx_) { SSL_CTX_free(static_cast<SSL_CTX*>(ssl_ctx_)); ssl_ctx_ = nullptr; }
                if (socket_fd_ >= 0) { ::close(socket_fd_); socket_fd_ = -1; }
            }
        }
    }
}

void UserDataStream::keepalive_loop() {
    while (running_.load()) {
        // Keep alive every 30 minutes
        for (int i = 0; i < 1800 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (running_.load()) {
            if (keepalive_listen_key()) {
                LOG_INFO(get_logger(), "{}", "[USER_STREAM] Listen key keepalive sent");
            } else {
                LOG_ERROR(get_logger(), "{}", "[USER_STREAM] Listen key keepalive failed");
            }
        }
    }
}

void UserDataStream::process_message(const std::string& message) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream stream(message);
    std::string errors;

    if (!Json::parseFromStream(builder, stream, &root, &errors)) {
        LOG_ERROR(get_logger(), "{}", "[USER_STREAM] Failed to parse message");
        return;
    }

    std::string event_type = root.get("e", "").asString();

    if (event_type == "executionReport") {
        // Order execution report
        std::string order_id = root.get("i", 0).asString();
        if (order_id == "0") {
            order_id = std::to_string(root.get("i", 0).asInt64());
        }
        std::string client_order_id = root.get("c", "").asString();
        std::string side_str = root.get("S", "").asString();
        std::string status_str = root.get("X", "").asString();

        double price = 0.0, qty = 0.0, cum_qty = 0.0;
        try {
            price = std::stod(root.get("L", "0").asString());   // Last filled price
            qty = std::stod(root.get("l", "0").asString());     // Last filled quantity
            cum_qty = std::stod(root.get("z", "0").asString()); // Cumulative filled quantity
        } catch (const std::exception& e) {
            LOG_ERROR(get_logger(), "[USER_STREAM] Failed to parse fill data: {}", e.what());
            return;
        }

        OrderSide side = (side_str == "BUY") ? OrderSide::BUY : OrderSide::SELL;

        OrderStatus status = OrderStatus::NEW;
        if (status_str == "FILLED") status = OrderStatus::FILLED;
        else if (status_str == "PARTIALLY_FILLED") status = OrderStatus::PARTIALLY_FILLED;
        else if (status_str == "CANCELED") status = OrderStatus::CANCELED;
        else if (status_str == "REJECTED") status = OrderStatus::REJECTED;
        else if (status_str == "EXPIRED") status = OrderStatus::EXPIRED;

        LOG_INFO(get_logger(), "[USER_STREAM] ExecutionReport: {} {} price={} qty={} cum_qty={}",
                 side_str, status_str, price, qty, cum_qty);

        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (fill_callback_) {
            fill_callback_(order_id, client_order_id, side, status,
                          price, qty, cum_qty);
        }
    } else if (event_type == "outboundAccountPosition") {
        // Account balance update
        auto balances = root["B"];
        if (balances.isArray()) {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            for (const auto& bal : balances) {
                std::string asset = bal.get("a", "").asString();
                double free_bal = 0.0, locked_bal = 0.0;
                try {
                    free_bal = std::stod(bal.get("f", "0").asString());
                    locked_bal = std::stod(bal.get("l", "0").asString());
                } catch (...) {
                    continue;
                }

                if (balance_callback_) {
                    balance_callback_(asset, free_bal, locked_bal);
                }
            }
        }
    }
}

} // namespace MarketMaker
