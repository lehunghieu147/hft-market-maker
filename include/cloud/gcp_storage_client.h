#pragma once

#include "core/gcp_auth_provider.h"
#include <string>

namespace MarketMaker {

class GcpStorageClient {
public:
    GcpStorageClient(GcpAuthProvider& auth, const std::string& bucket);
    ~GcpStorageClient() = default;

    // Upload string content to GCS (synchronous, not on hot path)
    bool upload(const std::string& object_path, const std::string& content,
                const std::string& content_type = "text/plain");

    // Upload file from disk
    bool upload_file(const std::string& object_path, const std::string& local_path);

private:
    GcpAuthProvider& auth_;
    std::string bucket_;
};

} // namespace MarketMaker
