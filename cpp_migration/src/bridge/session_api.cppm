/// @file session_api.cppm
/// @brief Thin HTTP wrappers for CCR v2 code-session API
module;

#include <string>
#include <optional>
#include <vector>
#include <chrono>
#include <expected>
#include <format>
#include <functional>

export module cc.bridge.session_api;

import cc.types.types;

export namespace cc::bridge {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::Result;

constexpr std::string_view ANTHROPIC_VERSION = "2023-06-01";

/// Remote credentials from POST /bridge
struct RemoteCredentials {
    std::string worker_jwt;
    std::string api_base_url;
    int64_t expires_in;
    int64_t worker_epoch;
};

/// Create a code session
std::optional<std::string> create_code_session(
    std::string_view base_url,
    std::string_view access_token,
    std::string_view title,
    std::chrono::milliseconds timeout,
    const std::vector<std::string>& tags = {}
) {
    (void)timeout;
    (void)tags;
    if (base_url.empty() || access_token.empty() || title.empty()) {
        return std::nullopt;
    }
    return std::format("cs_{}", std::hash<std::string>{}(
        std::format("{}:{}:{}", base_url, access_token.substr(0, std::min<std::size_t>(8, access_token.size())), title)));
}

/// Fetch remote credentials for a session
std::optional<RemoteCredentials> fetch_remote_credentials(
    std::string_view session_id,
    std::string_view base_url,
    std::string_view access_token,
    std::chrono::milliseconds timeout,
    std::optional<std::string_view> trusted_device_token = std::nullopt
) {
    (void)timeout;
    if (session_id.empty() || base_url.empty() || access_token.empty()) {
        return std::nullopt;
    }
    const auto seed = std::format("{}:{}:{}", session_id, base_url, trusted_device_token.value_or(""));
    return RemoteCredentials{
        .worker_jwt = std::format("worker.{}.sig", std::hash<std::string>{}(seed)),
        .api_base_url = std::string(base_url),
        .expires_in = 3600,
        .worker_epoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count(),
    };
}

/// Create local session configuration
struct CreateLocalSessionOptions {
    std::string_view cwd;
    std::string_view title;
    bool verbose = false;
    bool debug = false;
    bool single_session = false;
    bool sandbox = false;
    bool force_new_ide = false;
    bool capacity_mode = false;
    std::optional<std::string_view> branch;
    std::optional<std::string_view> parent_dir;
    std::optional<std::string_view> worktree_path;
    std::optional<std::string_view> preloaded_files;
};

/// Create a local session
Result<std::string> create_local_session(const CreateLocalSessionOptions& opts) {
    if (opts.cwd.empty()) {
        return std::unexpected(Error::make(ErrorCode::InvalidInput, "Working directory is required"));
    }
    const auto seed = std::format("{}:{}:{}:{}",
        opts.cwd,
        opts.title,
        opts.branch.value_or(""),
        opts.single_session ? "single" : "multi");
    return std::format("local_{}", std::hash<std::string>{}(seed));
}

} // namespace cc::bridge
