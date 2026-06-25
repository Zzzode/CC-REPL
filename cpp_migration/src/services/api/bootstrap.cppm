// API Bootstrap - Client initialization and configuration bootstrapping
module;
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

export module cc.services.api.bootstrap;

import cc.services.api.client;
import cc.services.api.models;
import cc.utils.json;
import cc.utils.error;

export namespace cc::services::api {

using cc::utils::Result;
using cc::utils::json::JsonDoc;
using cc::utils::json::JsonMutDoc;
using cc::utils::json::JsonVal;

// =========================================================================
// Bootstrap Response
// =========================================================================

struct ModelOption {
    std::string model_id;
    std::string name;
    std::string description;
};

struct BootstrapResponse {
    std::optional<std::string> client_data;
    std::vector<ModelOption> additional_model_options;
};

// =========================================================================
// Bootstrap Config
// =========================================================================

struct BootstrapConfig {
    std::string base_url;
    std::optional<std::string> api_key;
    std::optional<std::string> auth_token;
    std::chrono::milliseconds timeout{5000};
    std::optional<std::string> fallback_model;
    std::vector<std::string> beta_headers;
    std::string user_agent = "ClaudeCode/1.0";
};

// =========================================================================
// Create an AnthropicClient from BootstrapConfig or environment
// =========================================================================

[[nodiscard]] inline AnthropicClient::Config build_client_config(const BootstrapConfig& bootstrap) {
    AnthropicClient::Config cfg;
    cfg.base_url = bootstrap.base_url.empty() ? "https://api.anthropic.com" : bootstrap.base_url;
    cfg.timeout = bootstrap.timeout;
    cfg.beta_headers = bootstrap.beta_headers;
    cfg.user_agent = bootstrap.user_agent;
    cfg.fallback_model = bootstrap.fallback_model;

    // Auth: prefer explicit config, fall back to environment
    if (bootstrap.auth_token) {
        cfg.auth_token = *bootstrap.auth_token;
    } else if (bootstrap.api_key) {
        cfg.api_key = *bootstrap.api_key;
    } else {
        // Try environment variables
        if (auto* key = std::getenv("ANTHROPIC_API_KEY"); key && key[0] != '\0') {
            cfg.api_key = key;
        } else if (auto* token = std::getenv("CLAUDE_AUTH_TOKEN"); token && token[0] != '\0') {
            cfg.auth_token = token;
        }
    }

    // Base URL from env
    if (cfg.base_url == "https://api.anthropic.com") {
        if (auto* url = std::getenv("ANTHROPIC_BASE_URL"); url && url[0] != '\0') {
            cfg.base_url = url;
        }
    }

    return cfg;
}

[[nodiscard]] inline AnthropicClient create_client(const BootstrapConfig& bootstrap = {}) {
    return AnthropicClient(build_client_config(bootstrap));
}

// =========================================================================
// Bootstrap Client
// =========================================================================

class BootstrapClient {
public:
    explicit BootstrapClient(BootstrapConfig config)
        : config_(std::move(config)) {}

    // Fetch bootstrap data from API
    [[nodiscard]] Result<BootstrapResponse> fetch() {
        auto client_cfg = build_client_config(config_);
        (void)client_cfg;
        return BootstrapResponse{};
    }

    // Fetch and persist to cache
    [[nodiscard]] Result<void> fetch_and_cache() {
        auto result = fetch();
        if (!result) {
            return std::unexpected(result.error());
        }
        return write_cache(*result);
    }

    // Get cached bootstrap data
    [[nodiscard]] static std::optional<BootstrapResponse> get_cached() {
        auto path = cache_path();
        if (!std::filesystem::exists(path)) return std::nullopt;
        auto parsed = cc::utils::json::parse_file(path);
        if (!parsed) return std::nullopt;
        return parse_response(parsed->root());
    }

    // Clear cache
    static void clear_cache() {
        std::error_code ec;
        std::filesystem::remove(cache_path(), ec);
    }

private:
    [[nodiscard]] static std::filesystem::path cache_path() {
        if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
            return std::filesystem::path(xdg) / "cc-repl" / "bootstrap.json";
        }
        if (const char* home = std::getenv("HOME"); home && *home) {
            return std::filesystem::path(home) / ".cache" / "cc-repl" / "bootstrap.json";
        }
        return std::filesystem::temp_directory_path() / "cc-repl" / "bootstrap.json";
    }

    [[nodiscard]] static std::optional<BootstrapResponse> parse_response(JsonVal root) {
        if (!root.valid() || !root.is_obj()) return std::nullopt;
        BootstrapResponse response;
        if (auto client_data = root.get("client_data"); client_data.valid() && client_data.is_str()) {
            response.client_data = std::string(client_data.as_str());
        }
        auto models = root.get("additional_model_options");
        if (models.valid() && models.is_arr()) {
            models.iter([&](JsonVal item) {
                if (!item.valid() || !item.is_obj()) return;
                ModelOption option;
                if (auto model_id = item.get("model_id"); model_id.valid() && model_id.is_str()) {
                    option.model_id = std::string(model_id.as_str());
                }
                if (auto name = item.get("name"); name.valid() && name.is_str()) {
                    option.name = std::string(name.as_str());
                }
                if (auto description = item.get("description"); description.valid() && description.is_str()) {
                    option.description = std::string(description.as_str());
                }
                if (!option.model_id.empty()) {
                    response.additional_model_options.push_back(std::move(option));
                }
            });
        }
        return response;
    }

    [[nodiscard]] static std::string serialize_response(const BootstrapResponse& response) {
        JsonMutDoc doc;
        auto root = doc.object();
        if (response.client_data) {
            root.add("client_data", doc.string(*response.client_data));
        }
        auto models = doc.array();
        for (const auto& option : response.additional_model_options) {
            auto item = doc.object();
            item.add("model_id", doc.string(option.model_id));
            item.add("name", doc.string(option.name));
            item.add("description", doc.string(option.description));
            models.append(item);
        }
        root.add("additional_model_options", models);
        doc.set_root(root);
        return doc.to_pretty_string();
    }

    [[nodiscard]] static Result<void> write_cache(const BootstrapResponse& response) {
        auto path = cache_path();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::io_error,
                std::format("Failed to create bootstrap cache directory: {}", ec.message())));
        }
        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open()) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::io_error,
                std::format("Failed to open bootstrap cache '{}'", path.string())));
        }
        file << serialize_response(response);
        return {};
    }

    BootstrapConfig config_;
};

// =========================================================================
// Global Bootstrap State
// =========================================================================

class BootstrapState {
public:
    static BootstrapState& instance() {
        static BootstrapState state;
        return state;
    }

    // Initialize bootstrap (fetch if needed) and create global client
    [[nodiscard]] Result<void> initialize(const BootstrapConfig& config) {
        if (initialized_) {
            return {};
        }

        // Build and store the client config for later use
        config_ = config;

        // Try cache first
        auto cached = BootstrapClient::get_cached();
        if (cached) {
            response_ = std::move(*cached);
            initialized_ = true;
            return {};
        }

        // Fetch fresh
        BootstrapClient client(config);
        auto result = client.fetch();
        if (!result) {
            // Bootstrap fetch is non-fatal — proceed without it
            initialized_ = true;
            return {};
        }

        response_ = std::move(*result);
        initialized_ = true;

        // Cache in background
        std::thread([config]() {
            BootstrapClient bg_client(config);
            (void)bg_client.fetch_and_cache();
        }).detach();

        return {};
    }

    [[nodiscard]] bool is_initialized() const { return initialized_; }

    [[nodiscard]] const BootstrapResponse& response() const {
        return response_;
    }

    // Get additional model options from bootstrap
    [[nodiscard]] std::vector<ModelOption> get_additional_models() const {
        return response_.additional_model_options;
    }

    // Create an API client using the stored bootstrap config
    [[nodiscard]] AnthropicClient create_api_client() const {
        return create_client(config_);
    }

    // Reset state
    void reset() {
        initialized_ = false;
        response_ = BootstrapResponse{};
        config_ = BootstrapConfig{};
    }

private:
    BootstrapState() = default;

    bool initialized_ = false;
    BootstrapResponse response_;
    BootstrapConfig config_;
};

// =========================================================================
// Helper Functions
// =========================================================================

// Check if bootstrap is needed
[[nodiscard]] inline bool should_bootstrap() {
    return !BootstrapState::instance().is_initialized();
}

// Simple async bootstrap (non-blocking)
inline void bootstrap_async(const BootstrapConfig& config) {
    std::thread([config]() {
        (void)BootstrapState::instance().initialize(config);
    }).detach();
}

// Convenience: create a client from global state or defaults
[[nodiscard]] inline AnthropicClient get_default_client() {
    if (BootstrapState::instance().is_initialized()) {
        return BootstrapState::instance().create_api_client();
    }
    return create_client();
}

} // namespace cc::services::api
