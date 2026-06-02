/// @file setup.cppm
/// @brief Setup and configuration module for managing application settings,
/// configuration loading/saving, and first-run initialization.
/// Handles config files, user preferences, and bootstrap process.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <format>
#include <expected>
#include <cstdlib>
#include <unistd.h>

export module cc.bootstrap.setup;

import cc.types.types;
import cc.config.config;
import cc.config.feature_flags;
import cc.services.analytics.growthbook;
import cc.utils.json;

export namespace cc::core {

// ============================================================
// Application Settings
// ============================================================

/// User preferences and application settings
struct AppSettings {
    std::string default_model = "claude-3-5-sonnet-20241022";
    std::string theme = "dark";
    double temperature = 0.7;
    std::size_t max_tokens = 4096;
    bool enable_streaming = true;
    bool enable_telemetry = false;
    double budget_warning_threshold = 0.8;
    std::string preferred_language = "English";
    std::filesystem::path config_directory;
};

// ============================================================
// Setup Manager
// ============================================================

/// Manages application setup, initialization, and configuration
class SetupManager {
    AppSettings settings_;
    ConfigManager config_manager_;
    bool is_initialized_ = false;
    std::filesystem::path app_data_dir_;

public:
    SetupManager() {
        // Determine app data directory
        char* home = std::getenv("HOME");
        if (home) {
            app_data_dir_ = std::filesystem::path(home) / ".claude-code";
        } else {
            app_data_dir_ = std::filesystem::current_path() / ".claude-code";
        }
        settings_.config_directory = app_data_dir_;
    }

    /// Initialize the application
    [[nodiscard]] VoidResult initialize() {
        if (is_initialized_) return {};
        
        // Create app data directory if it doesn't exist
        if (!std::filesystem::exists(app_data_dir_)) {
            std::filesystem::create_directories(app_data_dir_);
        }
        
        // Load config if available
        auto load_result = load_config();
        if (!load_result && load_result.error().code != ErrorCode::ConfigNotFound) {
            return load_result;
        }

        // Initialize feature flags from environment
        cc::core::flags::global_flags().set_from_env();

        // Sync feature flags from GrowthBook (best-effort, non-blocking)
        sync_feature_flags_from_growthbook();
        
        is_initialized_ = true;
        return {};
    }

    /// Get current settings
    [[nodiscard]] const AppSettings& get_settings() const {
        return settings_;
    }

    /// Get mutable settings
    [[nodiscard]] AppSettings& get_settings_mut() {
        return settings_;
    }

    /// Update settings
    void update_settings(AppSettings settings) {
        settings_ = std::move(settings);
    }

    /// Save config
    [[nodiscard]] VoidResult save_config() const {
        std::error_code ec;
        std::filesystem::create_directories(app_data_dir_, ec);
        if (ec) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Failed to create config directory: {}", app_data_dir_.string())
            ));
        }

        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();
        root.add("default_model", doc.string(settings_.default_model));
        root.add("theme", doc.string(settings_.theme));
        root.add("temperature", doc.number(settings_.temperature));
        root.add("max_tokens", doc.number(static_cast<int64_t>(settings_.max_tokens)));
        root.add("enable_streaming", doc.boolean(settings_.enable_streaming));
        root.add("enable_telemetry", doc.boolean(settings_.enable_telemetry));
        root.add("budget_warning_threshold", doc.number(settings_.budget_warning_threshold));
        root.add("preferred_language", doc.string(settings_.preferred_language));
        root.add("config_directory", doc.string(settings_.config_directory.string()));
        doc.set_root(root);

        std::ofstream file(config_file_path());
        if (!file.is_open()) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Failed to open config file for writing: {}", config_file_path().string())
            ));
        }
        file << doc.to_pretty_string();
        if (!file.good()) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Failed to write config file: {}", config_file_path().string())
            ));
        }
        return {};
    }

    /// Load config
    [[nodiscard]] VoidResult load_config() {
        auto path = config_file_path();
        if (!std::filesystem::exists(path)) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigNotFound,
                std::format("Config file not found: {}", path.string())
            ));
        }

        auto doc_result = cc::utils::json::parse_file(path);
        if (!doc_result) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseError,
                std::format("Failed to parse config file: {}", doc_result.error().message())
            ));
        }

        auto root = doc_result->root();
        if (!root || !root.is_obj()) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigParseError,
                "Config root must be a JSON object"
            ));
        }

        if (auto v = root.get("default_model"); v && v.is_str()) settings_.default_model = std::string(v.as_str());
        if (auto v = root.get("theme"); v && v.is_str()) settings_.theme = std::string(v.as_str());
        if (auto v = root.get("temperature"); v && v.is_num()) settings_.temperature = v.as_double();
        if (auto v = root.get("max_tokens"); v && v.is_num()) settings_.max_tokens = static_cast<std::size_t>(v.as_int());
        if (auto v = root.get("enable_streaming"); v && v.is_bool()) settings_.enable_streaming = v.as_bool();
        if (auto v = root.get("enable_telemetry"); v && v.is_bool()) settings_.enable_telemetry = v.as_bool();
        if (auto v = root.get("budget_warning_threshold"); v && v.is_num()) settings_.budget_warning_threshold = v.as_double();
        if (auto v = root.get("preferred_language"); v && v.is_str()) settings_.preferred_language = std::string(v.as_str());
        if (auto v = root.get("config_directory"); v && v.is_str()) settings_.config_directory = std::filesystem::path(std::string(v.as_str()));
        return {};
    }

    /// Check if it's the first run
    [[nodiscard]] bool is_first_run() const {
        return !std::filesystem::exists(app_data_dir_ / ".initialized");
    }

    /// Mark as initialized
    VoidResult mark_initialized() {
        std::ofstream file(app_data_dir_ / ".initialized");
        if (file) {
            file << "initialized";
            return {};
        }
        return std::unexpected(Error::make(ErrorCode::ConfigWriteError, "Failed to create initialized file"));
    }

    /// Get app data directory
    [[nodiscard]] const std::filesystem::path& get_app_data_dir() const {
        return app_data_dir_;
    }

    /// Get config manager
    [[nodiscard]] ConfigManager& get_config_manager() {
        return config_manager_;
    }

    /// Get config manager (const)
    [[nodiscard]] const ConfigManager& get_config_manager() const {
        return config_manager_;
    }

private:
    [[nodiscard]] std::filesystem::path config_file_path() const {
        return app_data_dir_ / "settings.json";
    }

    /// Best-effort sync of feature flags from GrowthBook remote source.
    /// Failures are silently ignored — flags retain env/config/default values.
    void sync_feature_flags_from_growthbook() {
        using namespace cc::services::analytics::growthbook;

        GrowthBookConfig gb_config;
        gb_config.enabled = settings_.enable_telemetry; // Respect telemetry preference

        GrowthBookClient gb_client(std::move(gb_config));

        UserAttributes attrs;
        attrs.id = get_machine_fingerprint();
        
        if (!gb_client.initialize(attrs)) {
            return; // Disabled or analytics opt-out
        }

        gb_client.refresh_features();

        // Bridge: for each registered feature flag, query GrowthBook
        cc::core::flags::global_flags().sync_from_remote(
            [&](std::string_view name) -> std::optional<bool> {
                // GrowthBook uses lowercase feature keys with dots
                // Our registry uses UPPER_SNAKE. Convert: VOICE_MODE -> voice_mode
                std::string gb_key;
                gb_key.reserve(name.size());
                for (char c : name) {
                    gb_key += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
                }
                
                // Only override if GrowthBook has an explicit value for this feature
                auto all = gb_client.get_all_features();
                if (all.find(gb_key) != all.end()) {
                    return gb_client.is_feature_enabled(gb_key);
                }
                return std::nullopt; // No remote value — keep local
            }
        );
    }

    /// Get a stable machine identifier for GrowthBook targeting
    [[nodiscard]] static std::string get_machine_fingerprint() {
        if (const char* id = std::getenv("CC_MACHINE_ID")) {
            return std::string(id);
        }
        // Fallback: use hostname
        char hostname[256] = {};
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            return std::string(hostname);
        }
        return "unknown";
    }
};

} // namespace cc::core
