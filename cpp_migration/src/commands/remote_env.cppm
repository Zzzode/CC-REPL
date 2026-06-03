module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <cstdlib>
#include <utility>

export module cc.commands.remote_env;

export namespace cc::commands {


struct RemoteEnvInfo {
    std::string provider;
    std::string region;
    std::string instance_id;
    std::map<std::string, std::string> metadata;
};

inline std::optional<RemoteEnvInfo> configured_remote_env;


auto detect_remote_env() -> std::optional<RemoteEnvInfo> {
    if (configured_remote_env) return configured_remote_env;
    const char* provider = std::getenv("CC_REPL_REMOTE_PROVIDER");
    if (provider == nullptr || std::string_view{provider}.empty()) return std::nullopt;
    RemoteEnvInfo info{.provider = provider, .region = {}, .instance_id = {}, .metadata = {}};
    if (const char* region = std::getenv("CC_REPL_REMOTE_REGION")) info.region = region;
    if (const char* instance = std::getenv("CC_REPL_REMOTE_INSTANCE_ID")) info.instance_id = instance;
    info.metadata["source"] = "environment";
    return info;
}


auto configure_for_remote(RemoteEnvInfo env) -> void {
    configured_remote_env = std::move(env);
}


auto get_remote_env_summary() -> std::string {
    auto env = detect_remote_env();
    if (!env) {
        return "Running locally (no remote environment detected)";
    }

    std::string summary = "Remote Environment:\n";
    summary += "  Provider: " + env->provider + "\n";
    summary += "  Region: " + env->region + "\n";
    summary += "  Instance: " + env->instance_id + "\n";

    if (!env->metadata.empty()) {
        summary += "  Metadata:\n";
        for (const auto& [key, value] : env->metadata) {
            summary += "    " + key + ": " + value + "\n";
        }
    }

    return summary;
}

} // namespace cc::commands
