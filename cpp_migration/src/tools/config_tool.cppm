// ConfigTool - Reading and writing CLI configuration at runtime
module;
#include <array>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.config;


export namespace cc::tools {

// 配置操作类型
enum class ConfigAction {
    Get,
    Set,
    List,
    Delete,
};

constexpr auto config_action_name(ConfigAction a) -> std::string_view {
    switch (a) {
        case ConfigAction::Get:    return "get";
        case ConfigAction::Set:    return "set";
        case ConfigAction::List:   return "list";
        case ConfigAction::Delete: return "delete";
        default:                   return "unknown";
    }
}

// 配置作用域
enum class ConfigScope {
    Global,   // 全局配置 (~/.config/cc/config.json)
    Project,  // 项目级配置 (.cc/config.json)
};

constexpr auto scope_name(ConfigScope s) -> std::string_view {
    switch (s) {
        case ConfigScope::Global:  return "global";
        case ConfigScope::Project: return "project";
        default:                   return "unknown";
    }
}

// 配置错误类型
enum class ConfigError {
    KeyEmpty,
    KeyInvalid,
    KeyNotFound,
    ValueEmpty,
    ScopeInvalid,
    ReadFailed,
    WriteFailed,
    ParseFailed,
    PermissionDenied,
};

constexpr auto format_error(ConfigError err) -> std::string_view {
    switch (err) {
        case ConfigError::KeyEmpty:         return "Configuration key is empty";
        case ConfigError::KeyInvalid:       return "Configuration key contains invalid characters";
        case ConfigError::KeyNotFound:      return "Configuration key not found";
        case ConfigError::ValueEmpty:       return "Configuration value is empty";
        case ConfigError::ScopeInvalid:     return "Invalid configuration scope";
        case ConfigError::ReadFailed:       return "Failed to read configuration file";
        case ConfigError::WriteFailed:      return "Failed to write configuration file";
        case ConfigError::ParseFailed:      return "Failed to parse configuration file";
        case ConfigError::PermissionDenied: return "Permission denied for config operation";
        default:                            return "Unknown config error";
    }
}

// 已知的有效配置键
inline constexpr std::array kValidConfigKeys = {
    std::string_view{"model"},
    std::string_view{"api_key"},
    std::string_view{"base_url"},
    std::string_view{"temperature"},
    std::string_view{"max_tokens"},
    std::string_view{"timeout"},
    std::string_view{"theme"},
    std::string_view{"editor"},
    std::string_view{"shell"},
    std::string_view{"auto_approve"},
    std::string_view{"verbose"},
    std::string_view{"log_level"},
    std::string_view{"proxy"},
    std::string_view{"working_directory"},
};

// 配置请求
struct ConfigRequest {
    ConfigAction action;
    std::optional<std::string> key;
    std::optional<std::string> value;
    ConfigScope scope{ConfigScope::Global};
};

// 配置条目
struct ConfigEntry {
    std::string key;
    std::string value;
    ConfigScope scope;
};

// 配置存储器
class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path global_path = default_global_path(),
                         std::filesystem::path project_path = default_project_path())
        : global_path_(std::move(global_path))
        , project_path_(std::move(project_path)) {}

    // 获取配置值 (project 优先于 global)
    auto get(std::string_view key, ConfigScope scope) const
        -> std::expected<std::string, ConfigError>
    {
        const auto& store = (scope == ConfigScope::Project) ? project_store_ : global_store_;
        auto it = store.find(std::string(key));
        if (it == store.end()) return std::unexpected(ConfigError::KeyNotFound);
        return it->second;
    }

    // 设置配置值
    auto set(std::string key, std::string value, ConfigScope scope)
        -> std::expected<void, ConfigError>
    {
        auto& store = (scope == ConfigScope::Project) ? project_store_ : global_store_;
        store[std::move(key)] = std::move(value);
        return persist(scope);
    }

    // 删除配置值
    auto remove(std::string_view key, ConfigScope scope)
        -> std::expected<void, ConfigError>
    {
        auto& store = (scope == ConfigScope::Project) ? project_store_ : global_store_;
        auto it = store.find(std::string(key));
        if (it == store.end()) return std::unexpected(ConfigError::KeyNotFound);
        store.erase(it);
        return persist(scope);
    }

    // 列出所有配置
    auto list(ConfigScope scope) const -> std::vector<ConfigEntry> {
        std::vector<ConfigEntry> entries;
        const auto& store = (scope == ConfigScope::Project) ? project_store_ : global_store_;
        for (const auto& [key, value] : store) {
            entries.push_back(ConfigEntry{.key = key, .value = value, .scope = scope});
        }
        return entries;
    }

private:
    std::filesystem::path global_path_;
    std::filesystem::path project_path_;
    std::unordered_map<std::string, std::string> global_store_;
    std::unordered_map<std::string, std::string> project_store_;

    // 持久化配置到磁盘
    auto persist(ConfigScope scope) -> std::expected<void, ConfigError> {
        const auto& path = (scope == ConfigScope::Project) ? project_path_ : global_path_;
        const auto& store = (scope == ConfigScope::Project) ? project_store_ : global_store_;

        // 确保目录存在
        auto parent = path.parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
        }

        std::ofstream file(path);
        if (!file) return std::unexpected(ConfigError::WriteFailed);

        // 简单 JSON 序列化
        file << "{\n";
        bool first = true;
        for (const auto& [key, value] : store) {
            if (!first) file << ",\n";
            file << std::format("  \"{}\": \"{}\"", key, value);
            first = false;
        }
        file << "\n}\n";
        return {};
    }

    static auto default_global_path() -> std::filesystem::path {
        auto home = std::getenv("HOME");
        if (home) return std::filesystem::path(home) / ".config" / "cc" / "config.json";
        return "~/.config/cc/config.json";
    }

    static auto default_project_path() -> std::filesystem::path {
        return std::filesystem::current_path() / ".cc" / "config.json";
    }
};

// ConfigTool - 读写 CLI 配置
class ConfigTool {
public:
    static constexpr std::string_view name = "config";
    static constexpr std::string_view description = "Read and write CLI configuration at runtime";

    auto validate(const ConfigRequest& request) const -> std::expected<void, ConfigError> {
        if (request.action == ConfigAction::Get || request.action == ConfigAction::Set ||
            request.action == ConfigAction::Delete) {
            if (!request.key || request.key->empty()) {
                return std::unexpected(ConfigError::KeyEmpty);
            }
            if (!is_valid_key(*request.key)) {
                return std::unexpected(ConfigError::KeyInvalid);
            }
        }
        if (request.action == ConfigAction::Set) {
            if (!request.value || request.value->empty()) {
                return std::unexpected(ConfigError::ValueEmpty);
            }
        }
        return {};
    }

    auto execute(ConfigRequest request) -> std::expected<std::string, ConfigError> {
        if (auto v = validate(request); !v) return std::unexpected(v.error());

        switch (request.action) {
            case ConfigAction::Get: {
                auto value = store_.get(*request.key, request.scope);
                if (!value) return std::unexpected(value.error());
                return std::format("{} = {}", *request.key, *value);
            }
            case ConfigAction::Set: {
                auto result = store_.set(*request.key, *request.value, request.scope);
                if (!result) return std::unexpected(result.error());
                return std::format("Set {} = {} [{}]", *request.key, *request.value,
                    scope_name(request.scope));
            }
            case ConfigAction::Delete: {
                auto result = store_.remove(*request.key, request.scope);
                if (!result) return std::unexpected(result.error());
                return std::format("Deleted key '{}' from {} config", *request.key,
                    scope_name(request.scope));
            }
            case ConfigAction::List: {
                auto entries = store_.list(request.scope);
                std::string output = std::format("Configuration [{}]:\n", scope_name(request.scope));
                for (const auto& entry : entries) {
                    output += std::format("  {} = {}\n", entry.key, entry.value);
                }
                return output;
            }
            default:
                return std::unexpected(ConfigError::KeyInvalid);
        }
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "action": {{ "type": "string", "enum": ["get", "set", "list", "delete"], "description": "Config operation" }},
      "key": {{ "type": "string", "description": "Configuration key" }},
      "value": {{ "type": "string", "description": "Configuration value (for set)" }},
      "scope": {{ "type": "string", "enum": ["global", "project"], "description": "Config scope (default: global)" }}
    }},
    "required": ["action"]
  }}
}})json", name, description);
    }

private:
    ConfigStore store_;

    // 验证配置键是否合法 (只允许字母、数字、下划线、点号)
    static auto is_valid_key(std::string_view key) -> bool {
        return !key.empty() && std::all_of(key.begin(), key.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
        });
    }
};

} // namespace cc::tools
