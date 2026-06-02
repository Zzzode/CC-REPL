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

// 远程环境信息
struct RemoteEnvInfo {
    std::string provider;                        // 云服务提供商（AWS/GCP/Azure 等）
    std::string region;                          // 区域
    std::string instance_id;                     // 实例 ID
    std::map<std::string, std::string> metadata; // 额外元数据
};

inline std::optional<RemoteEnvInfo> configured_remote_env;

// 自动检测当前是否运行在远程环境中
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

// 根据检测到的远程环境配置 CLI 行为
auto configure_for_remote(RemoteEnvInfo env) -> void {
    configured_remote_env = std::move(env);
}

// 获取远程环境的可读摘要
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
