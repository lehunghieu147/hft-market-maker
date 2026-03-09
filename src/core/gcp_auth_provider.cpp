// System OpenSSL headers MUST come before any gRPC/BoringSSL headers
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#include "core/gcp_auth_provider.h"
#include "core/base64.h"
#include "core/app_logger.h"

#include <json/json.h>
#include <curl/curl.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {
    quill::Logger* get_logger() {
        static quill::Logger* logger = MarketMaker::AppLogger::get("core");
        return logger;
    }

    size_t gcp_write_cb(char* ptr, size_t size, size_t nmemb, std::string* data) {
        data->append(ptr, size * nmemb);
        return size * nmemb;
    }
} // anonymous namespace

namespace MarketMaker {

GcpAuthProvider::GcpAuthProvider(const std::string& service_account_path) {
    load_service_account(service_account_path);
}

GcpAuthProvider::~GcpAuthProvider() = default;

void GcpAuthProvider::load_service_account(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_ERROR(get_logger(), "Cannot open service account file: {}", path);
        return;
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(file, root)) {
        LOG_ERROR(get_logger(), "Failed to parse service account JSON: {}", path);
        return;
    }

    client_email_ = root["client_email"].asString();
    private_key_pem_ = root["private_key"].asString();
    project_id_ = root["project_id"].asString();
    token_uri_ = root.get("token_uri", "https://oauth2.googleapis.com/token").asString();

    if (client_email_.empty() || private_key_pem_.empty() || project_id_.empty()) {
        LOG_ERROR(get_logger(), "{}", "Service account JSON missing required fields");
        return;
    }

    loaded_ = true;
    LOG_INFO(get_logger(), "GCP service account loaded: {} (project: {})", client_email_, project_id_);
}

std::string GcpAuthProvider::create_jwt() const {
    auto now = std::chrono::system_clock::now();
    auto iat = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    auto exp = iat + 3600;

    // Header
    std::string header = R"({"alg":"RS256","typ":"JWT"})";
    std::string header_b64 = base64url_encode(header);

    // Payload
    Json::Value payload;
    payload["iss"] = client_email_;
    payload["scope"] = "https://www.googleapis.com/auth/cloud-platform";
    payload["aud"] = token_uri_;
    payload["iat"] = static_cast<Json::Int64>(iat);
    payload["exp"] = static_cast<Json::Int64>(exp);

    Json::FastWriter writer;
    std::string payload_str = writer.write(payload);
    // Remove trailing newline from FastWriter
    if (!payload_str.empty() && payload_str.back() == '\n') {
        payload_str.pop_back();
    }
    std::string payload_b64 = base64url_encode(payload_str);

    // Sign with RSA-SHA256
    std::string signing_input = header_b64 + "." + payload_b64;

    BIO* bio = BIO_new_mem_buf(private_key_pem_.data(), static_cast<int>(private_key_pem_.size()));
    if (!bio) {
        LOG_ERROR(get_logger(), "{}", "Failed to create BIO for private key");
        return {};
    }

    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        LOG_ERROR(get_logger(), "{}", "Failed to read private key from PEM");
        return {};
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    size_t sig_len = 0;
    std::string signature;

    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
        EVP_DigestSignUpdate(ctx, signing_input.data(), signing_input.size()) == 1 &&
        EVP_DigestSignFinal(ctx, nullptr, &sig_len) == 1) {
        signature.resize(sig_len);
        if (EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char*>(signature.data()), &sig_len) != 1) {
            signature.clear();
        }
        signature.resize(sig_len);
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    if (signature.empty()) {
        LOG_ERROR(get_logger(), "{}", "JWT signing failed");
        return {};
    }

    std::string sig_b64 = base64url_encode(signature);
    return signing_input + "." + sig_b64;
}

std::string GcpAuthProvider::exchange_jwt_for_token(const std::string& jwt) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR(get_logger(), "{}", "Failed to init CURL for token exchange");
        return {};
    }

    std::string post_data = "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=" + jwt;
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, token_uri_.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, gcp_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR(get_logger(), "Token exchange failed: {}", curl_easy_strerror(res));
        return {};
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(response, root)) {
        LOG_ERROR(get_logger(), "{}", "Failed to parse token response");
        return {};
    }

    if (!root.isMember("access_token")) {
        LOG_ERROR(get_logger(), "Token response error: {}", response);
        return {};
    }

    std::string token = root["access_token"].asString();
    int expires_in = root.get("expires_in", 3600).asInt();

    // Cache with 5-minute buffer
    cached_token_ = token;
    token_expiry_ = std::chrono::system_clock::now() + std::chrono::seconds(expires_in - 300);

    LOG_INFO(get_logger(), "GCP access token acquired (expires in {}s)", expires_in);
    return token;
}

std::string GcpAuthProvider::get_access_token() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_) {
        LOG_ERROR(get_logger(), "{}", "Service account not loaded");
        return {};
    }

    // Return cached token if still valid
    if (!cached_token_.empty() && std::chrono::system_clock::now() < token_expiry_) {
        return cached_token_;
    }

    // Refresh token
    std::string jwt = create_jwt();
    if (jwt.empty()) return {};

    return exchange_jwt_for_token(jwt);
}

} // namespace MarketMaker
