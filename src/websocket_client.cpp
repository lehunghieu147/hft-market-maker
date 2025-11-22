#include "websocket_client.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace MarketMaker {

// Simple WebSocket implementation without external WebSocket++ library
class WebSocketClient::Impl {
public:
    int socket_fd = -1;
    SSL_CTX* ssl_ctx = nullptr;
    SSL* ssl = nullptr;
    bool connected = false;
    std::string buffer;

    Impl() {
        // Initialize OpenSSL
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();

        // Create SSL context
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx) {
            std::cerr << "Failed to create SSL context" << std::endl;
        }

        // Set SSL options to allow older TLS versions and be more permissive
        SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
        SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_2_VERSION);

        // Set cipher suites
        SSL_CTX_set_cipher_list(ssl_ctx, "DEFAULT:!DH");

        // Enable SNI (Server Name Indication)
        SSL_CTX_set_tlsext_servername_callback(ssl_ctx, nullptr);
    }

    ~Impl() {
        disconnect();
        if (ssl_ctx) {
            SSL_CTX_free(ssl_ctx);
        }
    }

    void disconnect() {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (socket_fd >= 0) {
            close(socket_fd);
            socket_fd = -1;
        }
        connected = false;
    }
};

WebSocketClient::WebSocketClient() : pImpl(std::make_unique<Impl>()) {}

WebSocketClient::~WebSocketClient() {
    should_run_ = false;
    disconnect();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    if (reconnect_thread_.joinable()) {
        reconnect_thread_.join();
    }
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

bool WebSocketClient::connect(const std::string& uri) {
    current_uri_ = uri;
    std::cout << "WebSocketClient::connect() called with URI: " << uri << std::endl;

    // Parse URI (simplified - assumes wss://host:port/path format)
    std::string host;
    int port = 443;  // Default HTTPS port
    std::string path = "/";

    size_t start = uri.find("://");
    if (start != std::string::npos) {
        start += 3;
        size_t end = uri.find('/', start);
        if (end != std::string::npos) {
            host = uri.substr(start, end - start);
            path = uri.substr(end);
        } else {
            host = uri.substr(start);
        }

        // Extract port if present
        size_t colon = host.find(':');
        if (colon != std::string::npos) {
            port = std::stoi(host.substr(colon + 1));
            host = host.substr(0, colon);
        }
    }

    // Create socket
    pImpl->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (pImpl->socket_fd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return false;
    }

    // Set socket timeout to prevent indefinite blocking
    struct timeval timeout;
    timeout.tv_sec = 10;   // 10 seconds timeout
    timeout.tv_usec = 0;

    if (setsockopt(pImpl->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        std::cerr << "Failed to set receive timeout" << std::endl;
    }

    if (setsockopt(pImpl->socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        std::cerr << "Failed to set send timeout" << std::endl;
    }

    // Resolve hostname
    struct hostent* server = gethostbyname(host.c_str());
    if (!server) {
        std::cerr << "Failed to resolve hostname: " << host << std::endl;
        return false;
    }

    // Connect to server
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (::connect(pImpl->socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to connect to server" << std::endl;
        close(pImpl->socket_fd);
        pImpl->socket_fd = -1;
        return false;
    }

    // Setup SSL
    pImpl->ssl = SSL_new(pImpl->ssl_ctx);
    if (!pImpl->ssl) {
        std::cerr << "Failed to create SSL connection" << std::endl;
        return false;
    }

    // Set SNI hostname (critical for modern servers)
    SSL_set_tlsext_host_name(pImpl->ssl, host.c_str());

    SSL_set_fd(pImpl->ssl, pImpl->socket_fd);

    if (SSL_connect(pImpl->ssl) <= 0) {
        std::cerr << "SSL connection failed" << std::endl;
        ERR_print_errors_fp(stderr);
        return false;
    }

    // Send WebSocket upgrade request
    std::stringstream request;
    request << "GET " << path << " HTTP/1.1\r\n";
    request << "Host: " << host << "\r\n";
    request << "Upgrade: websocket\r\n";
    request << "Connection: Upgrade\r\n";
    request << "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    request << "Sec-WebSocket-Version: 13\r\n";
    request << "\r\n";

    std::string req_str = request.str();
    SSL_write(pImpl->ssl, req_str.c_str(), req_str.length());

    // Read response (simplified - just check if upgrade was successful)
    char response[1024];
    int bytes = SSL_read(pImpl->ssl, response, sizeof(response) - 1);
    if (bytes > 0) {
        response[bytes] = '\0';
        if (strstr(response, "101 Switching Protocols")) {
            pImpl->connected = true;
            connected_ = true;

            // Update last message time
            {
                std::lock_guard<std::mutex> lock(last_message_mutex_);
                last_message_time_ = std::chrono::steady_clock::now();
            }

            // Start/restart worker thread
            if (worker_thread_.joinable()) {
                // Join old thread if it's still running
                std::cout << "[WS] Joining old worker thread..." << std::endl;
                worker_thread_.join();
            }
            worker_thread_ = std::thread(&WebSocketClient::run_worker, this);
            std::cout << "[WS] Worker thread started" << std::endl;

            // Start/restart heartbeat thread
            if (heartbeat_thread_.joinable()) {
                std::cout << "[WS] Joining old heartbeat thread..." << std::endl;
                heartbeat_thread_.join();
            }
            heartbeat_thread_ = std::thread(&WebSocketClient::run_heartbeat, this);
            std::cout << "[WS] Heartbeat thread started" << std::endl;

            if (connection_handler_) {
                connection_handler_(true);
            }

            return true;
        }
    }

    std::cerr << "WebSocket upgrade failed" << std::endl;
    pImpl->disconnect();
    return false;
}

void WebSocketClient::disconnect() {
    std::cout << "[WS] Disconnecting..." << std::endl;
    connected_ = false;
    if (connection_handler_) {
        connection_handler_(false);
    }
    pImpl->disconnect();
    std::cout << "[WS] Disconnected" << std::endl;
}

bool WebSocketClient::is_connected() const {
    return connected_ && pImpl->connected;
}

void WebSocketClient::subscribe_orderbook(const std::string& symbol, int depth) {
    if (!connected_) return;

    // Create subscription message (JSON format for Binance)
    std::stringstream json;
    json << "{";
    json << "\"method\":\"SUBSCRIBE\",";
    json << "\"params\":[\"" << symbol << "@depth" << depth << "@100ms\"],";
    json << "\"id\":1";
    json << "}";

    std::string message = json.str();

    // WebSocket frame header (simplified text frame)
    unsigned char header[10];
    int header_len = 2;
    header[0] = 0x81;  // FIN=1, opcode=1 (text)

    size_t payload_len = message.length();
    if (payload_len <= 125) {
        header[1] = 0x80 | payload_len;  // Mask=1, length
    } else if (payload_len <= 65535) {
        header[1] = 0x80 | 126;
        header[2] = (payload_len >> 8) & 0xFF;
        header[3] = payload_len & 0xFF;
        header_len = 4;
    }

    // Masking key (required for client-to-server messages)
    unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(&header[header_len], mask, 4);
    header_len += 4;

    // Mask the payload
    std::string masked_payload = message;
    for (size_t i = 0; i < masked_payload.length(); i++) {
        masked_payload[i] ^= mask[i % 4];
    }

    // Send frame
    SSL_write(pImpl->ssl, header, header_len);
    SSL_write(pImpl->ssl, masked_payload.c_str(), masked_payload.length());
}

void WebSocketClient::subscribe_trades(const std::string& symbol) {
    if (!connected_) return;

    // Similar to subscribe_orderbook
    std::stringstream json;
    json << "{";
    json << "\"method\":\"SUBSCRIBE\",";
    json << "\"params\":[\"" << symbol << "@trade\"],";
    json << "\"id\":2";
    json << "}";

    std::string message = json.str();

    // Send WebSocket frame (simplified implementation)
    // In production, this should properly construct WebSocket frames
}

void WebSocketClient::set_message_handler(MessageHandler handler) {
    message_handler_ = handler;
}

void WebSocketClient::set_connection_handler(ConnectionHandler handler) {
    connection_handler_ = handler;
}

void WebSocketClient::enable_auto_reconnect(bool enable) {
    auto_reconnect_ = enable;
}

void WebSocketClient::set_reconnect_delay(std::chrono::milliseconds delay) {
    reconnect_delay_ = delay;
}

void WebSocketClient::run_worker() {
    std::string accumulated_data;
    unsigned char buffer[65536];  // Larger buffer for WebSocket frames

    while (should_run_ && pImpl->connected) {
        int bytes = SSL_read(pImpl->ssl, buffer, sizeof(buffer));

        if (bytes > 0) {
            int pos = 0;

            while (pos < bytes) {
                // Parse WebSocket frame header
                if (bytes - pos < 2) break;  // Not enough data for header

                unsigned char fin = (buffer[pos] & 0x80) >> 7;
                unsigned char opcode = buffer[pos] & 0x0F;
                pos++;

                unsigned char masked = (buffer[pos] & 0x80) >> 7;
                uint64_t payload_len = buffer[pos] & 0x7F;
                pos++;

                // Handle extended payload length
                if (payload_len == 126) {
                    if (bytes - pos < 2) break;
                    payload_len = (buffer[pos] << 8) | buffer[pos + 1];
                    pos += 2;
                } else if (payload_len == 127) {
                    if (bytes - pos < 8) break;
                    payload_len = 0;
                    for (int i = 0; i < 8; i++) {
                        payload_len = (payload_len << 8) | buffer[pos + i];
                    }
                    pos += 8;
                }

                // Skip mask if present (server shouldn't send masked frames)
                if (masked) {
                    pos += 4;
                }

                // Check if we have the full payload
                if (static_cast<uint64_t>(bytes - pos) < payload_len) {
                    // Need more data
                    break;
                }

                // Handle different frame types
                if (opcode == 0x01 || opcode == 0x00) {  // Text frame or continuation
                    std::string payload((char*)&buffer[pos], payload_len);

                    if (opcode == 0x01) {
                        accumulated_data.clear();
                    }
                    accumulated_data += payload;

                    if (fin) {
                        // Complete message received
                        if (message_handler_ && !accumulated_data.empty()) {
                            // Debug: Log first 100 chars of message
                            std::string preview = accumulated_data.substr(0, std::min(size_t(100), accumulated_data.length()));
                            std::cout << "[WS] Message received: " << preview << "..." << std::endl;

                            message_handler_(accumulated_data);

                            // Update last message time
                            std::lock_guard<std::mutex> lock(last_message_mutex_);
                            last_message_time_ = std::chrono::steady_clock::now();
                        }
                        accumulated_data.clear();
                    }
                } else if (opcode == 0x08) {  // Close frame
                    disconnect();
                    return;
                } else if (opcode == 0x09) {  // Ping frame
                    // Send pong with same payload
                    std::vector<unsigned char> pong_frame(125 + payload_len);
                    pong_frame[0] = 0x8A;  // FIN=1, opcode=10 (pong)

                    if (payload_len <= 125) {
                        pong_frame[1] = 0x80 | payload_len;  // Masked
                        // Add simple mask
                        pong_frame[2] = 0x12;
                        pong_frame[3] = 0x34;
                        pong_frame[4] = 0x56;
                        pong_frame[5] = 0x78;

                        // Copy and mask payload
                        for (size_t i = 0; i < payload_len; i++) {
                            pong_frame[6 + i] = buffer[pos + i] ^ pong_frame[2 + (i % 4)];
                        }
                        SSL_write(pImpl->ssl, pong_frame.data(), 6 + payload_len);
                    }
                }

                pos += payload_len;
            }
        } else if (bytes == 0) {
            // Connection closed
            std::cerr << "WebSocket connection closed by server" << std::endl;
            disconnect();
            break;
        } else {
            // Error occurred
            int ssl_error = SSL_get_error(pImpl->ssl, bytes);

            // Check if it's a timeout (not a real error)
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                // Timeout occurred - check if connection is still alive
                continue;  // Continue reading
            }

            // Real error occurred
            std::cerr << "SSL read error: " << ssl_error;

            // Check errno for socket-level errors
            if (errno == ETIMEDOUT || errno == ECONNRESET || errno == EPIPE ||
                errno == ENETUNREACH || errno == EHOSTUNREACH) {
                std::cerr << " (Socket error: " << strerror(errno) << ")";
            }
            std::cerr << std::endl;

            disconnect();
            break;
        }

        // Small delay to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!connected_ && auto_reconnect_ && should_run_) {
        handle_reconnect();
    }
}

void WebSocketClient::handle_reconnect() {
    if (reconnect_thread_.joinable()) {
        return;
    }

    reconnect_thread_ = std::thread([this]() {
        int attempts = 0;
        const int max_attempts = 10;

        while (should_run_ && auto_reconnect_ && !connected_ && attempts < max_attempts) {
            attempts++;
            std::cout << "Reconnection attempt " << attempts << "/" << max_attempts
                      << " using URI: " << current_uri_ << std::endl;

            std::this_thread::sleep_for(reconnect_delay_);

            if (connect(current_uri_)) {
                std::cout << "Reconnected successfully!" << std::endl;
                break;
            }
        }

        if (!connected_ && attempts >= max_attempts) {
            std::cerr << "Failed to reconnect after " << max_attempts << " attempts" << std::endl;
        }
    });

    reconnect_thread_.detach();
}

void WebSocketClient::run_heartbeat() {
    while (should_run_) {
        std::this_thread::sleep_for(std::chrono::seconds(15));

        if (!connected_ || !should_run_) {
            break;
        }

        // Check if we received any message in the last 30 seconds
        auto now = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_msg_time;
        {
            std::lock_guard<std::mutex> lock(last_message_mutex_);
            last_msg_time = last_message_time_;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_msg_time).count();

        if (elapsed > 30) {
            std::cerr << "No message received for " << elapsed << " seconds - connection appears dead" << std::endl;
            disconnect();
            break;
        }

        // Send ping to keep connection alive
        send_ping();
    }
}

void WebSocketClient::send_ping() {
    if (!connected_ || !pImpl->ssl) {
        return;
    }

    // Create ping frame
    unsigned char ping_frame[6];
    ping_frame[0] = 0x89;  // FIN=1, opcode=9 (ping)
    ping_frame[1] = 0x80;  // Masked, payload length = 0

    // Add mask (required for client-to-server)
    ping_frame[2] = 0x00;
    ping_frame[3] = 0x00;
    ping_frame[4] = 0x00;
    ping_frame[5] = 0x00;

    int sent = SSL_write(pImpl->ssl, ping_frame, 6);
    if (sent <= 0) {
        std::cerr << "Failed to send ping frame" << std::endl;
    }
}

} // namespace MarketMaker