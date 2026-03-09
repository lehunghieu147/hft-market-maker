#pragma once

#include <string>
#include <cstdint>
#include <openssl/evp.h>

namespace MarketMaker {

// RFC 4648 base64 encode using OpenSSL
inline std::string base64_encode(const uint8_t* data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3) + 1;
    std::string result(out_len, '\0');
    int encoded = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(result.data()),
        data, static_cast<int>(len));
    result.resize(encoded);
    return result;
}

inline std::string base64_encode(const std::string& input) {
    return base64_encode(reinterpret_cast<const uint8_t*>(input.data()), input.size());
}

// URL-safe base64 (for JWT): replace +/ with -_, strip padding
inline std::string base64url_encode(const std::string& input) {
    std::string result = base64_encode(input);
    for (auto& c : result) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    // Strip trailing '='
    while (!result.empty() && result.back() == '=') {
        result.pop_back();
    }
    return result;
}

inline std::string base64_decode(const std::string& input) {
    size_t out_len = 3 * input.size() / 4 + 1;
    std::string result(out_len, '\0');
    int decoded = EVP_DecodeBlock(
        reinterpret_cast<unsigned char*>(result.data()),
        reinterpret_cast<const unsigned char*>(input.data()),
        static_cast<int>(input.size()));
    if (decoded < 0) return {};
    // Adjust for padding
    size_t padding = 0;
    if (input.size() >= 2 && input[input.size() - 1] == '=') padding++;
    if (input.size() >= 2 && input[input.size() - 2] == '=') padding++;
    result.resize(decoded - padding);
    return result;
}

} // namespace MarketMaker
