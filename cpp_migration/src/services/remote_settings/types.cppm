module;
#include <chrono>
#include <string>
#include <vector>
export module cc.services.remote_settings.types;

export namespace cc::services::remote_settings {

using time_point = std::chrono::system_clock::time_point;

// A single remote setting entry
struct RemoteSetting {
    std::string key;
    std::string value;
    std::string source;
    bool locked{false};
    time_point updated_at{};
};

// Response from remote settings server
struct RemoteSettingsResponse {
    std::vector<RemoteSetting> settings;
    std::string etag;
    time_point expires{};
};

} // namespace cc::services::remote_settings
