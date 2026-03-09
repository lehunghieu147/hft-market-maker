#include "cloud/gcp_storage_client.h"
#include "core/app_logger.h"

#include <curl/curl.h>
#include <fstream>
#include <sstream>

namespace {
    quill::Logger* get_logger() {
        static quill::Logger* logger = MarketMaker::AppLogger::get("core");
        return logger;
    }

    size_t discard_write(char*, size_t size, size_t nmemb, void*) {
        return size * nmemb;
    }
} // anonymous namespace

namespace MarketMaker {

GcpStorageClient::GcpStorageClient(GcpAuthProvider& auth, const std::string& bucket)
    : auth_(auth), bucket_(bucket) {}

bool GcpStorageClient::upload(const std::string& object_path, const std::string& content,
                              const std::string& content_type) {
    std::string token = auth_.get_access_token();
    if (token.empty()) {
        LOG_ERROR(get_logger(), "{}", "No access token for GCS upload");
        return false;
    }

    // URL-encode the object path
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    char* escaped_path = curl_easy_escape(curl, object_path.c_str(),
                                          static_cast<int>(object_path.size()));
    std::string url = "https://storage.googleapis.com/upload/storage/v1/b/" +
                      bucket_ + "/o?uploadType=media&name=" + std::string(escaped_path);
    curl_free(escaped_path);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, content.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(content.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    long http_code = 0;
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        LOG_ERROR(get_logger(), "GCS upload failed: curl={} http={} path={}",
                  curl_easy_strerror(res), http_code, object_path);
        return false;
    }

    LOG_INFO(get_logger(), "GCS upload OK: gs://{}/{}", bucket_, object_path);
    return true;
}

bool GcpStorageClient::upload_file(const std::string& object_path, const std::string& local_path) {
    std::ifstream file(local_path, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR(get_logger(), "Cannot open file for GCS upload: {}", local_path);
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return upload(object_path, ss.str(), "application/octet-stream");
}

} // namespace MarketMaker
