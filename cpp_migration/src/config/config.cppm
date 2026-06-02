/// @file config.cppm
/// @brief Configuration module for the Claude Code CLI.
/// Manages hierarchical settings (global -> project -> CLI flags),
/// environment variable integration, feature flags, and JSON persistence.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <algorithm>
#include <ranges>

export module cc.config.config;

import cc.types.types;
import cc.utils.json;

export namespace cc::core {

// ============================================================
// Feature Flags
// ============================================================

/// Named feature flags that can be toggled at build or runtime
enum class FeatureFlag : std::uint32_t {
    Proactive          = 1 << 0,   // Proactive suggestions
    BridgeMode         = 1 << 1,   // IDE bridge integration
    VoiceMode          = 1 << 2,   // Voice input support
    Daemon             = 1 << 3,   // Background daemon mode
    AgentTriggers      = 1 << 4,   // Automatic agent triggering
    MonitorTool        = 1 << 5,   // System monitoring tool
    Templates          = 1 << 6,   // Template system
    BackgroundSessions = 1 << 7,   // Background session support
    ExtendedThinking   = 1 << 8,   // Extended thinking mode
    MultiAgent         = 1 << 9,   // Multi-agent orchestration
    SkillSystem        = 1 << 10,  // Skill loading system
};

/// Bitwise operations for combining feature flags
[[nodiscard]] constexpr FeatureFlag operator|(FeatureFlag a, FeatureFlag b) noexcept {
    return static_cast<FeatureFlag>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b)
    );
}

[[nodiscard]] constexpr FeatureFlag operator&(FeatureFlag a, FeatureFlag b) noexcept {
    return static_cast<FeatureFlag>(
        static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b)
    );
}

/// Feature flag set with runtime toggle support
class FeatureFlags {
    std::uint32_t flags_ = 0;

public:
    FeatureFlags() = default;
    explicit FeatureFlags(std::uint32_t raw) : flags_(raw) {}

    /// Check if a flag is enabled
    [[nodiscard]] bool is_enabled(FeatureFlag flag) const noexcept {
        return (flags_ & static_cast<std::uint32_t>(flag)) != 0;
    }

    /// Enable a feature flag
    void enable(FeatureFlag flag) noexcept {
        flags_ |= static_cast<std::uint32_t>(flag);
    }

    /// Disable a feature flag
    void disable(FeatureFlag flag) noexcept {
        flags_ &= ~static_cast<std::uint32_t>(flag);
    }

    /// Set flag state explicitly
    void set(FeatureFlag flag, bool enabled) noexcept {
        if (enabled) enable(flag); else disable(flag);
    }

    /// Get raw flag bits
    [[nodiscard]] std::uint32_t raw() const noexcept { return flags_; }
};

// ============================================================
// Settings structure
// ============================================================

/// Model-specific settings
struct ModelSettings {
    std::string default_model = "claude-sonnet-4-20250514";
    std::uint32_t max_output_tokens = 16384;
    std::optional<double> temperature;
    bool extended_thinking = false;
    std::optional<std::uint32_t> thinking_budget;
    std::uint32_t context_window_size = 200000;
};

/// Permission settings for tool execution
struct PermissionSettings {
    bool allow_bash = true;                    // Allow bash command execution
    bool allow_file_write = true;              // Allow file modifications
    bool allow_network = true;                 // Allow network requests
    std::vector<std::string> allowed_paths;    // Whitelisted file paths
    std::vector<std::string> denied_paths;     // Blacklisted file paths
    std::vector<std::string> allowed_commands; // Whitelisted shell commands
};

/// Display and UI settings
struct DisplaySettings {
    bool show_thinking = true;                 // Display thinking blocks
    bool show_token_usage = false;             // Show token counters
    bool compact_mode = false;                 // Minimal output formatting
    std::optional<std::uint32_t> line_width;   // Terminal line width override
    std::string theme = "auto";               // Color theme (auto, dark, light)
};

/// Network and API connection settings
struct NetworkSettings {
    std::optional<std::string> api_key;            // Anthropic API key
    std::optional<std::string> base_url;           // Custom API endpoint
    std::optional<std::string> proxy;              // HTTP proxy URL
    std::uint32_t timeout_seconds = 120;           // Request timeout
    std::uint32_t max_retries = 3;                 // Retry attempts
    bool verify_ssl = true;                        // SSL certificate verification
};

/// MCP (Model Context Protocol) server configuration
struct McpServerConfig {
    std::string name;                              // Server identifier
    std::string command;                           // Launch command
    std::vector<std::string> args;                 // Command arguments
    std::unordered_map<std::string, std::string> env;  // Environment variables
};

/// Top-level settings aggregating all configuration sections
struct Settings {
    ModelSettings model;
    PermissionSettings permissions;
    DisplaySettings display;
    NetworkSettings network;
    FeatureFlags features;
    std::vector<McpServerConfig> mcp_servers;      // Configured MCP servers
    std::optional<std::string> system_prompt;      // Custom system prompt override
    std::vector<std::string> custom_instructions;  // Additional context instructions
};

// ============================================================
// Configuration source hierarchy
// ============================================================

/// Configuration source priority (lower value = higher priority)
enum class ConfigSource : std::uint8_t {
    CliFlags = 0,      // Command-line arguments (highest priority)
    EnvVars = 1,       // Environment variables
    ProjectConfig = 2, // .claude/config.json in project root
    GlobalConfig = 3,  // ~/.config/claude/config.json
    Defaults = 4,      // Built-in defaults (lowest priority)
};

// ============================================================
// Config Manager
// ============================================================

/// Manages loading, merging, and persisting configuration from multiple sources
class ConfigManager {
    Settings settings_;                     // Resolved effective settings
    std::filesystem::path global_path_;     // Path to global config file
    std::filesystem::path project_path_;    // Path to project config file
    bool dirty_ = false;                   // Whether unsaved changes exist

public:
    /// Initialize with default settings
    ConfigManager()
        : global_path_(default_global_config_path())
        , project_path_(default_project_config_path()) {}

    /// Initialize with explicit paths (for testing)
    explicit ConfigManager(std::filesystem::path global, std::filesystem::path project)
        : global_path_(std::move(global))
        , project_path_(std::move(project)) {}

    /// Load configuration from all sources, merging by priority
    [[nodiscard]] Result<void> load() {
        // Start with defaults
        settings_ = Settings{};

        // Layer 1: Global config file
        if (auto result = load_from_file(global_path_); !result && 
            result.error().code != ErrorCode::ConfigNotFound) {
            return std::unexpected(result.error());
        }

        // Layer 2: Project config file (overrides global)
        if (auto result = load_from_file(project_path_); !result &&
            result.error().code != ErrorCode::ConfigNotFound) {
            return std::unexpected(result.error());
        }

        // Layer 3: Environment variables (override file configs)
        apply_environment_variables();

        dirty_ = false;
        return {};
    }

    /// Save current settings to the specified config file
    [[nodiscard]] VoidResult save(ConfigSource target = ConfigSource::ProjectConfig) {
        const auto& path = (target == ConfigSource::GlobalConfig) ? global_path_ : project_path_;

        // Ensure parent directory exists
        auto parent = path.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                return std::unexpected(Error::make(
                    ErrorCode::ConfigWriteError,
                    std::format("Failed to create config directory: {}", parent.string())
                ));
            }
        }

        // Serialize and write
        auto json = serialize_settings();
        std::ofstream file(path);
        if (!file.is_open()) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Cannot open config file for writing: {}", path.string())
            ));
        }

        file << json;
        dirty_ = false;
        return {};
    }

    /// Get the current effective settings (read-only)
    [[nodiscard]] const Settings& settings() const noexcept { return settings_; }

    /// Get mutable settings reference for modification
    [[nodiscard]] Settings& settings_mut() noexcept {
        dirty_ = true;
        return settings_;
    }

    /// Check if a feature flag is enabled
    [[nodiscard]] bool is_feature_enabled(FeatureFlag flag) const noexcept {
        return settings_.features.is_enabled(flag);
    }

    /// Set a feature flag at runtime
    void set_feature(FeatureFlag flag, bool enabled) {
        settings_.features.set(flag, enabled);
        dirty_ = true;
    }

    /// Get the API key (from settings or environment)
    [[nodiscard]] std::optional<std::string> api_key() const {
        if (settings_.network.api_key) return settings_.network.api_key;
        // Fallback: check environment (already applied during load)
        return std::nullopt;
    }

    /// Check if there are unsaved changes
    [[nodiscard]] bool is_dirty() const noexcept { return dirty_; }

    /// Get path to the global config file
    [[nodiscard]] const std::filesystem::path& global_config_path() const noexcept {
        return global_path_;
    }

    /// Get path to the project config file
    [[nodiscard]] const std::filesystem::path& project_config_path() const noexcept {
        return project_path_;
    }

private:
    /// Load and merge settings from a JSON config file
    [[nodiscard]] VoidResult load_from_file(const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigNotFound,
                std::format("Config file not found: {}", path.string())
            ));
        }

        std::ifstream file(path);
        if (!file.is_open()) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseError,
                std::format("Cannot open config file: {}", path.string())
            ));
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        return parse_and_merge(content);
    }

    /// Parse JSON content and merge into current settings.
    /// Uses yyjson via cc::utils::json to deserialize fields.
    [[nodiscard]] VoidResult parse_and_merge(std::string_view json_content) {
        auto doc_result = cc::utils::json::parse(json_content);
        if (!doc_result) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseError,
                "Failed to parse config JSON: " + doc_result.error().message()
            ));
        }

        auto root = doc_result->root();
        if (!root || !root.is_obj()) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseError, "Config JSON root must be an object"));
        }

        // Model settings
        if (auto model = root.get("model"); model.is_obj()) {
            if (auto v = model.get("default_model"); v.is_str()) {
                settings_.model.default_model = std::string(v.as_str());
            }
            if (auto v = model.get("max_output_tokens"); v.is_num()) {
                settings_.model.max_output_tokens = static_cast<std::uint32_t>(v.as_int());
            }
            if (auto v = model.get("temperature"); v.is_num()) {
                settings_.model.temperature = v.as_double();
            }
            if (auto v = model.get("extended_thinking"); v.is_bool()) {
                settings_.model.extended_thinking = v.as_bool();
            }
            if (auto v = model.get("thinking_budget"); v.is_num()) {
                settings_.model.thinking_budget = static_cast<std::uint32_t>(v.as_int());
            }
            if (auto v = model.get("context_window_size"); v.is_num()) {
                settings_.model.context_window_size = static_cast<std::uint32_t>(v.as_int());
            }
        }

        // Display settings
        if (auto display = root.get("display"); display.is_obj()) {
            if (auto v = display.get("show_thinking"); v.is_bool()) {
                settings_.display.show_thinking = v.as_bool();
            }
            if (auto v = display.get("show_token_usage"); v.is_bool()) {
                settings_.display.show_token_usage = v.as_bool();
            }
            if (auto v = display.get("compact_mode"); v.is_bool()) {
                settings_.display.compact_mode = v.as_bool();
            }
            if (auto v = display.get("line_width"); v.is_num()) {
                settings_.display.line_width = static_cast<std::uint32_t>(v.as_int());
            }
            if (auto v = display.get("theme"); v.is_str()) {
                settings_.display.theme = std::string(v.as_str());
            }
        }

        // Network settings
        if (auto network = root.get("network"); network.is_obj()) {
            if (auto v = network.get("api_key"); v.is_str()) {
                settings_.network.api_key = std::string(v.as_str());
            }
            if (auto v = network.get("base_url"); v.is_str()) {
                settings_.network.base_url = std::string(v.as_str());
            }
            if (auto v = network.get("proxy"); v.is_str()) {
                settings_.network.proxy = std::string(v.as_str());
            }
            if (auto v = network.get("timeout_seconds"); v.is_num()) {
                settings_.network.timeout_seconds = static_cast<std::uint32_t>(v.as_int());
            }
            if (auto v = network.get("max_retries"); v.is_num()) {
                settings_.network.max_retries = static_cast<std::uint32_t>(v.as_int());
            }
            if (auto v = network.get("verify_ssl"); v.is_bool()) {
                settings_.network.verify_ssl = v.as_bool();
            }
        }

        // Permission settings
        if (auto perms = root.get("permissions"); perms.is_obj()) {
            if (auto v = perms.get("allow_bash"); v.is_bool()) {
                settings_.permissions.allow_bash = v.as_bool();
            }
            if (auto v = perms.get("allow_file_write"); v.is_bool()) {
                settings_.permissions.allow_file_write = v.as_bool();
            }
            if (auto v = perms.get("allow_network"); v.is_bool()) {
                settings_.permissions.allow_network = v.as_bool();
            }
            if (auto arr = perms.get("allowed_paths"); arr.is_arr()) {
                settings_.permissions.allowed_paths.clear();
                for (std::size_t i = 0; i < arr.size(); ++i) {
                    if (auto item = arr.at(i); item.is_str()) {
                        settings_.permissions.allowed_paths.emplace_back(item.as_str());
                    }
                }
            }
            if (auto arr = perms.get("denied_paths"); arr.is_arr()) {
                settings_.permissions.denied_paths.clear();
                for (std::size_t i = 0; i < arr.size(); ++i) {
                    if (auto item = arr.at(i); item.is_str()) {
                        settings_.permissions.denied_paths.emplace_back(item.as_str());
                    }
                }
            }
            if (auto arr = perms.get("allowed_commands"); arr.is_arr()) {
                settings_.permissions.allowed_commands.clear();
                for (std::size_t i = 0; i < arr.size(); ++i) {
                    if (auto item = arr.at(i); item.is_str()) {
                        settings_.permissions.allowed_commands.emplace_back(item.as_str());
                    }
                }
            }
        }

        // MCP servers
        if (auto servers = root.get("mcpServers"); servers.is_obj()) {
            // mcpServers is an object where keys are server names
            settings_.mcp_servers.clear();
            servers.iter_obj([&](auto key, auto val) {
                if (!key.is_str() || !val.is_obj()) return;

                McpServerConfig cfg;
                cfg.name = std::string(key.as_str());
                if (auto cmd = val.get("command"); cmd.is_str()) {
                    cfg.command = std::string(cmd.as_str());
                }
                if (auto args = val.get("args"); args.is_arr()) {
                    for (std::size_t j = 0; j < args.size(); ++j) {
                        if (auto arg = args.at(j); arg.is_str()) {
                            cfg.args.emplace_back(arg.as_str());
                        }
                    }
                }
                if (auto env = val.get("env"); env.is_obj()) {
                    env.iter_obj([&](auto ek, auto ev) {
                        if (ek.is_str() && ev.is_str()) {
                            cfg.env[std::string(ek.as_str())] = std::string(ev.as_str());
                        }
                    });
                }
                settings_.mcp_servers.push_back(std::move(cfg));
            });
        }

        // System prompt
        if (auto v = root.get("systemPrompt"); v.is_str()) {
            settings_.system_prompt = std::string(v.as_str());
        }

        // Custom instructions
        if (auto arr = root.get("customInstructions"); arr.is_arr()) {
            settings_.custom_instructions.clear();
            for (std::size_t i = 0; i < arr.size(); ++i) {
                if (auto item = arr.at(i); item.is_str()) {
                    settings_.custom_instructions.emplace_back(item.as_str());
                }
            }
        }

        // Feature flags (as integer bitmask or object)
        if (auto v = root.get("features"); v.is_num()) {
            settings_.features = FeatureFlags(static_cast<std::uint32_t>(v.as_int()));
        }

        return {};
    }

    /// Apply environment variables to settings (highest override priority)
    void apply_environment_variables() {
        // ANTHROPIC_API_KEY -> network.api_key
        if (auto* val = std::getenv("ANTHROPIC_API_KEY")) {
            settings_.network.api_key = val;
        }

        // ANTHROPIC_BASE_URL -> network.base_url
        if (auto* val = std::getenv("ANTHROPIC_BASE_URL")) {
            settings_.network.base_url = val;
        }

        // CLAUDE_MODEL -> model.default_model
        if (auto* val = std::getenv("CLAUDE_MODEL")) {
            settings_.model.default_model = val;
        }

        // HTTPS_PROXY / HTTP_PROXY -> network.proxy
        if (auto* val = std::getenv("HTTPS_PROXY")) {
            settings_.network.proxy = val;
        } else if (auto* val2 = std::getenv("HTTP_PROXY")) {
            settings_.network.proxy = val2;
        }

        // CLAUDE_MAX_TOKENS -> model.max_output_tokens
        if (auto* val = std::getenv("CLAUDE_MAX_TOKENS")) {
            try {
                settings_.model.max_output_tokens = static_cast<std::uint32_t>(std::stoul(val));
            } catch (...) {
                // Ignore invalid values
            }
        }
    }

    /// Serialize settings to JSON string
    [[nodiscard]] std::string serialize_settings() const {
        // Simplified JSON serialization (production code uses a JSON library)
        return std::format(
            R"({{
  "model": {{
    "default_model": "{}",
    "max_output_tokens": {},
    "extended_thinking": {},
    "context_window_size": {}
  }},
  "display": {{
    "show_thinking": {},
    "show_token_usage": {},
    "compact_mode": {},
    "theme": "{}"
  }},
  "network": {{
    "timeout_seconds": {},
    "max_retries": {},
    "verify_ssl": {}
  }},
  "features": {}
}})",
            settings_.model.default_model,
            settings_.model.max_output_tokens,
            settings_.model.extended_thinking ? "true" : "false",
            settings_.model.context_window_size,
            settings_.display.show_thinking ? "true" : "false",
            settings_.display.show_token_usage ? "true" : "false",
            settings_.display.compact_mode ? "true" : "false",
            settings_.display.theme,
            settings_.network.timeout_seconds,
            settings_.network.max_retries,
            settings_.network.verify_ssl ? "true" : "false",
            settings_.features.raw()
        );
    }

    /// Get default global config path (~/.config/claude/config.json)
    [[nodiscard]] static std::filesystem::path default_global_config_path() {
        if (auto* home = std::getenv("HOME")) {
            return std::filesystem::path(home) / ".config" / "claude" / "config.json";
        }
        return "config.json";  // Fallback
    }

    /// Get default project config path (.claude/config.json in cwd)
    [[nodiscard]] static std::filesystem::path default_project_config_path() {
        return std::filesystem::current_path() / ".claude" / "config.json";
    }
};

} // namespace cc::core
