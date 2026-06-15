/// @file pointer.cppm
/// @brief Crash recovery pointer for Remote Control sessions
module;

#include <string>
#include <optional>
#include <expected>
#include <format>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

export module cc.bridge.pointer;

import cc.types.types;
import cc.bridge.session_id_compat;
import cc.utils.bash_execution;

export namespace cc::bridge {

/// Bridge pointer data structure
struct BridgePointer {
    std::string session_id;
    std::string environment_id;
    std::string source; // "standalone" or "repl"
};

/// Bridge pointer with age information
struct BridgePointerWithAge {
    BridgePointer pointer;
    int64_t age_ms;
};

/// Maximum age for a bridge pointer (4 hours in milliseconds)
constexpr int64_t BRIDGE_POINTER_TTL_MS = 4 * 60 * 60 * 1000;

namespace detail {

/// Global bridge handle pointer — set when a REPL bridge session is active,
/// cleared on teardown. Mirrors the TS `handle` in replBridgeHandle.ts.
BridgePointer* g_bridge_handle = nullptr;

/// Convert a session ID to the compat format (cse_* -> session_*).
[[nodiscard]] auto to_compat_session_id(std::string_view id) -> std::string {
    if (id.starts_with("cse_")) {
        return std::format("session_{}", id.substr(4));
    }
    return std::string(id);
}

} // namespace detail

/// Set the global bridge handle pointer (called during bridge init/teardown).
void set_bridge_handle(BridgePointer* handle) {
    detail::g_bridge_handle = handle;
}

/// Get the global bridge handle pointer.
[[nodiscard]] BridgePointer* get_bridge_handle() {
    return detail::g_bridge_handle;
}

namespace detail {

[[nodiscard]] auto json_escape(std::string_view value) -> std::string {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

[[nodiscard]] auto json_string(std::string_view json, std::string_view key) -> std::optional<std::string> {
    auto marker = '"' + std::string(key) + '"';
    auto pos = json.find(marker);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find('"', pos + 1);
    if (pos == std::string_view::npos) return std::nullopt;

    std::string out;
    bool escaping = false;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        char c = json[i];
        if (escaping) {
            switch (c) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                default: out.push_back(c); break;
            }
            escaping = false;
        } else if (c == '\\') {
            escaping = true;
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto safe_component(std::string_view value) -> bool {
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':')) return false;
    }
    return true;
}

[[nodiscard]] auto read_text(std::string_view path) -> std::optional<std::string> {
    std::ifstream file{std::filesystem::path(std::string(path))};
    if (!file.is_open()) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

[[nodiscard]] auto run_command(std::string_view command) -> std::vector<std::string> {
    std::vector<std::string> lines;
    FILE* pipe = cc::utils::bash::popen_spawn(std::string(command).c_str());
    if (!pipe) return lines;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    cc::utils::bash::pclose_spawn(pipe);
    return lines;
}

[[nodiscard]] auto shell_quote(std::string_view value) -> std::string {
    std::string out{"'"};
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

} // namespace detail

/// Clear (delete) a bridge pointer
void clear_bridge_pointer(std::string_view dir);

/// Get the path for a bridge pointer file
std::string get_bridge_pointer_path(std::string_view dir) {
    auto base = std::filesystem::path(std::string(dir));
    return (base / ".cc-repl" / "bridge-pointer.json").string();
}

/// Write a bridge pointer to disk
void write_bridge_pointer(std::string_view dir, const BridgePointer& pointer) {
    try {
        auto path = get_bridge_pointer_path(dir);
        auto parent_path = std::filesystem::path(path).parent_path();
        if (!std::filesystem::exists(parent_path)) {
            std::filesystem::create_directories(parent_path);
        }
        
        std::ofstream file(path);
        if (file.is_open()) {
            file << std::format(
                R"({{"session_id":"{}","environment_id":"{}","source":"{}"}})",
                detail::json_escape(pointer.session_id),
                detail::json_escape(pointer.environment_id),
                detail::json_escape(pointer.source));
            file.close();
        }
    } catch (const std::exception& e) {
        // Best effort - don't crash
    }
}

/// Read a bridge pointer from disk, checking age
std::optional<BridgePointerWithAge> read_bridge_pointer(std::string_view dir) {
    try {
        auto path = get_bridge_pointer_path(dir);
        if (!std::filesystem::exists(path)) {
            return std::nullopt;
        }
        
        // Get file modification time for age calculation
        auto mtime = std::filesystem::last_write_time(path);
        auto now = std::chrono::system_clock::now();
        auto mtime_sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            mtime - std::filesystem::file_time_type::clock::now() + now);
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - mtime_sys).count();
        
        // Check if pointer is too old
        if (age > BRIDGE_POINTER_TTL_MS) {
            clear_bridge_pointer(dir);
            return std::nullopt;
        }
        
        auto content = detail::read_text(path);
        if (!content) return std::nullopt;
        
        auto session_id = detail::json_string(*content, "session_id");
        auto environment_id = detail::json_string(*content, "environment_id");
        auto source = detail::json_string(*content, "source");
        if (!session_id || !environment_id || !source ||
            !detail::safe_component(*session_id) || !detail::safe_component(*environment_id) ||
            (*source != "standalone" && *source != "repl")) {
            clear_bridge_pointer(dir);
            return std::nullopt;
        }
        
        return BridgePointerWithAge{
            .pointer = BridgePointer{
                .session_id = *session_id,
                .environment_id = *environment_id,
                .source = *source
            },
            .age_ms = age
        };
    } catch (...) {
        return std::nullopt;
    }
}

/// Read bridge pointer, checking across worktrees
std::optional<std::pair<BridgePointerWithAge, std::string>> 
read_bridge_pointer_across_worktrees(std::string_view dir) {
    // First check current directory
    auto here = read_bridge_pointer(dir);
    if (here) {
        return std::make_pair(*here, std::string(dir));
    }
    
    auto command = std::format("git -C {} worktree list --porcelain 2>/dev/null", detail::shell_quote(dir));
    for (const auto& line : detail::run_command(command)) {
        constexpr std::string_view prefix = "worktree ";
        if (!line.starts_with(prefix)) continue;
        auto worktree = line.substr(prefix.size());
        if (worktree == dir) continue;
        if (auto pointer = read_bridge_pointer(worktree)) {
            return std::make_pair(*pointer, worktree);
        }
    }
    return std::nullopt;
}

/// Clear (delete) a bridge pointer
void clear_bridge_pointer(std::string_view dir) {
    try {
        auto path = get_bridge_pointer_path(dir);
        if (std::filesystem::exists(path)) {
            std::filesystem::remove(path);
        }
    } catch (...) {
        // Best effort - don't crash
    }
}

/// Get this bridge's own session ID in the session_* compat format used by
/// the v1 API (/v1/sessions responses). Returns std::nullopt when no bridge
/// handle is active. Mirrors the TS getSelfBridgeCompatId() in replBridgeHandle.ts.
[[nodiscard]] auto get_self_bridge_compat_id() -> std::optional<std::string> {
    auto* handle = get_bridge_handle();
    if (!handle) return std::nullopt;
    return detail::to_compat_session_id(handle->session_id);
}

} // namespace cc::bridge
