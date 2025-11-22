#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include "config.h"
#include <string>
#include <optional>

namespace MarketMaker {

class ConfigLoader {
public:
    // Load configuration from JSON file
    static std::optional<Config> load_from_file(const std::string& filename);

    // Save configuration to JSON file
    static bool save_to_file(const Config& config, const std::string& filename);

    // Validate configuration
    static bool validate(const Config& config);

    // Merge with environment variables (env vars take priority)
    static void merge_with_env(Config& config);

private:
    static std::string mask_secret(const std::string& secret);
};

} // namespace MarketMaker

#endif // CONFIG_LOADER_H