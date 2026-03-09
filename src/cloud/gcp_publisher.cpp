#include "cloud/gcp_publisher.h"
#include "core/base64.h"
#include "core/app_logger.h"

#include <json/json.h>
#include <curl/curl.h>

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

GcpPublisher::GcpPublisher(GcpAuthProvider& auth, const std::string& project_id,
                           const std::string& topic)
    : auth_(auth), project_id_(project_id), topic_(topic) {
    publish_url_ = "https://pubsub.googleapis.com/v1/projects/" +
                   project_id_ + "/topics/" + topic_ + ":publish";
}

GcpPublisher::~GcpPublisher() {
    stop();
}

void GcpPublisher::publish(const std::string& event_json) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.size() >= MAX_QUEUE_SIZE) {
        queue_.pop(); // Drop oldest
        LOG_WARNING(get_logger(), "{}", "Pub/Sub queue full, dropping oldest message");
    }
    queue_.push(event_json);
    queue_cv_.notify_one();
}

void GcpPublisher::start() {
    running_ = true;
    worker_ = std::thread([this]() { worker_loop(); });
    LOG_INFO(get_logger(), "GCP Publisher started (topic: {})", topic_);
}

void GcpPublisher::stop() {
    if (!running_.exchange(false)) return;
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    LOG_INFO(get_logger(), "GCP Publisher stopped (sent={} failed={})",
             sent_count_.load(), fail_count_.load());
}

void GcpPublisher::worker_loop() {
    while (running_) {
        std::vector<std::string> batch;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return !queue_.empty() || !running_;
            });
            if (!running_ && queue_.empty()) break;

            // Dequeue batch (up to 10)
            for (int i = 0; i < 10 && !queue_.empty(); ++i) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop();
            }
        }

        for (const auto& msg : batch) {
            if (send_to_pubsub(msg)) {
                sent_count_++;
            } else {
                fail_count_++;
            }
        }
    }
}

bool GcpPublisher::send_to_pubsub(const std::string& message_data) {
    std::string token = auth_.get_access_token();
    if (token.empty()) {
        LOG_ERROR(get_logger(), "{}", "No access token for Pub/Sub publish");
        return false;
    }

    std::string encoded = base64_encode(message_data);

    Json::Value root;
    Json::Value msg;
    msg["data"] = encoded;
    root["messages"].append(msg);

    Json::FastWriter writer;
    std::string body = writer.write(root);

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, publish_url_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    long http_code = 0;
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        LOG_ERROR(get_logger(), "Pub/Sub publish failed: curl={} http={}",
                  curl_easy_strerror(res), http_code);
        return false;
    }

    return true;
}

} // namespace MarketMaker
