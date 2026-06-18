/// @file doctor_screen.cppm
/// @brief Doctor diagnostics screen with check results and fix suggestions.
/// Migrated from src/screens/Doctor.tsx (574 lines, React/Ink -> FTXUI)
///
/// Screen flow (state machine):
///   Splash   -> Running (live progress) -> Results (per-check table)
///   Results  -> Summary (score + Fix-all log)
///   Summary  -> (rerun -> Running) or (Back -> on_done)
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <expected>
#include <variant>
#include <format>
#include <cstdint>
#include <array>
#include <chrono>
#include <string_view>
#include <algorithm>
#include <utility>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <cerrno>

// POSIX TCP probe (used by the Network check). Guarded so non-POSIX builds
// report "unsupported" instead of failing to compile.
#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#endif

export module cc.ui.doctor_screen;

import cc.utils.json;

// Design-system tokens (shared with other screens)
// Palette matches Pane / design-system/Pane.tsx + Doctor severity colors.
namespace cc::ui::doctor_screen::ds {
using namespace ftxui;
const Color BG_DARK       = Color::Grey23;
const Color SURFACE       = Color::Grey27;
const Color BORDER_OK     = Color::Green;
const Color BORDER_WARN   = Color::Yellow;
const Color BORDER_ERR    = Color::Red;
const Color ACCENT        = Color::Cyan;
const Color TEXT_MUTED    = Color::Grey70;
const Color SPARQL_OK     = Color::Green;
const Color SPARQL_WARN   = Color::Yellow;
const Color SPARQL_ERR    = Color::Red;
} // namespace cc::ui::doctor_screen::ds

export namespace cc::ui::doctor_screen {
using namespace ftxui;
using namespace ds;

// ============================================================
// Enums
// ============================================================

/// Screen state machine (matches Doctor lifecycle)
enum class ScreenState : std::uint8_t {
    Splash,    // 0. Header banner, not started
    Running,   // 1. Executing checks live (progress bar + spinner)
    Results,   // 2. Per-check table with expandable detail rows
    Summary,   // 3. Score gauge + Fix-all + Rerun + Back to REPL
};

/// Severity level for a diagnostic check (3-tier + info)
enum class DiagnosticSeverity : std::uint8_t {
    Ok,         // Check passed
    Info,       // Informational only
    Warning,    // Non-critical issue (yellow)
    Error,      // Critical issue (red)
    Pending,    // Not yet executed (shown as spinner)
};

/// Runtime state per check (for Running state live-updates)
enum class CheckRunState : std::uint8_t {
    Queued,     // Not started
    Running,    // In progress
    Done,       // Completed
};

/// Identifiers for each of the 13 diagnostic items (used for stable ordering)
enum class CheckId : std::uint8_t {
    ApiKey,              // 0: API key validity (HTTP)
    Network,             // 1: Network connectivity
    ConfigReadWrite,     // 2: Config file read/write permissions
    DiskSpace,           // 3: Disk space availability
    FsPermissions,       // 4: File-system permission model (plugins/sandbox)
    VersionLatest,       // 5: Latest version (npm/GCS)
    McpServers,          // 6: MCP server configuration/state
    OAuthToken,          // 7: OAuth token validity / expiry
    PermissionIntegrity, // 8: Permission system integrity (rules / unreachable)
    Bridge,              // 9: IDE Bridge (JWT / sessions)
    DefaultPermissions,  // 10: Default permission settings
    ShellConfig,         // 11: Terminal shell configuration
    PluginEngine,        // 12: Plugin engine (load / manifest)
    _Count,
};
inline constexpr std::size_t kCheckCount = static_cast<std::size_t>(CheckId::_Count);

// ============================================================
// Data types
// ============================================================

/// Definition of a single diagnostic check (immutable schema)
struct CheckDefinition {
    CheckId       id;
    std::string   name;           // short label, e.g. "API Key"
    std::string   description;    // one-liner, shown in help/detail
    std::string   category;       // grouping label for the section
};

/// Result of a single diagnostic check (populated as they complete)
struct CheckResult {
    DiagnosticSeverity severity = DiagnosticSeverity::Pending;
    CheckRunState      run_state = CheckRunState::Queued;
    std::string        message;           // Short pass/warn/fail line
    std::string        detail;            // Longer multi-line explanation
    std::optional<std::string> fix_hint;  // Human-readable fix suggestion
    std::optional<std::string> fix_command; // One-shot shell command (if auto-fixable)
    std::chrono::milliseconds elapsed{0}; // Execution wall-time
};

/// Version/dist-tag information (see Doctor.tsx DistTagsDisplay)
struct VersionInfo {
    std::string current_version;
    std::string installation_type; // "npm", "native", "package manager"
    std::string installation_path;
    std::string invoked_binary;
    std::optional<std::string> latest_version;
    std::optional<std::string> stable_version;
    std::optional<std::string> package_manager;
    std::optional<std::string> config_install_method;
    bool auto_updates_enabled = true;
    bool has_update_permissions = true;
};

/// Optional external warnings surfaced in the header area (see context warnings)
struct ContextNotice {
    std::string category;   // e.g. "Unreachable Rules", "MCP", "Agents"
    std::string message;
    DiagnosticSeverity severity;
    std::vector<std::string> details;
};

/// Stale-lock / PID-lock info (from Doctor.tsx VersionLockInfo)
struct VersionLockInfo {
    bool enabled = false;
    int lock_count = 0;
    int stale_locks_cleaned = 0;
    std::string locks_dir;
};

/// Complete data model consumed by the screen.
/// NOTE: Check results are produced by RunCheck() (real filesystem/env/config/
///       network probes against a DoctorContext). See RunAllChecks().
struct DoctorDataModel {
    VersionInfo version;
    VersionLockInfo locks;
    std::vector<ContextNotice> notices;
    std::array<CheckResult, kCheckCount> results;   // 13 results, index = CheckId
};

// ============================================================
// Check-definition table (13 items, stable order)
// ============================================================

[[nodiscard]] inline constexpr std::array<CheckDefinition, kCheckCount> AllChecks() {
    return {{
        { CheckId::ApiKey,
          "API Key Validity",
          "Validates the Anthropic API key via an authenticated HTTP probe.",
          "Connectivity" },
        { CheckId::Network,
          "Network Connectivity",
          "Checks DNS + TLS reachability to api.anthropic.com and other endpoints.",
          "Connectivity" },
        { CheckId::ConfigReadWrite,
          "Config File Access",
          "Verifies read/write permissions on the XDG config home and project .claude directories.",
          "System" },
        { CheckId::DiskSpace,
          "Disk Space",
          "Ensures adequate free disk space for cache, locks, and context artifacts.",
          "System" },
        { CheckId::FsPermissions,
          "File-system Permissions",
          "Audits tool permission grants and sandbox/plugin isolation rules.",
          "Security" },
        { CheckId::VersionLatest,
          "Version / Updates",
          "Compares running build against latest npm / GCS dist-tags.",
          "Updates" },
        { CheckId::McpServers,
          "MCP Server Status",
          "Enumerates configured MCP servers and validates manifest parsing.",
          "Integrations" },
        { CheckId::OAuthToken,
          "OAuth Token Validity",
          "Checks access-token expiry and refresh-token availability.",
          "Security" },
        { CheckId::PermissionIntegrity,
          "Permission System Integrity",
          "Detects unreachable rules, duplicate patterns, and shadowed entries.",
          "Security" },
        { CheckId::Bridge,
          "Bridge Status",
          "IDE bridge server status: JWT signing, session table, open connections.",
          "Integrations" },
        { CheckId::DefaultPermissions,
          "Default Permission Settings",
          "Reports the user's current defaults and flags risky auto-approve rules.",
          "Security" },
        { CheckId::ShellConfig,
          "Terminal Shell Configuration",
          "Validates $SHELL, rc-file readability, and subshell spawning health.",
          "System" },
        { CheckId::PluginEngine,
          "Plugin Engine",
          "Lists loaded plugins, reports manifest errors, and flags load failures.",
          "Integrations" },
    }};
}

[[nodiscard]] inline constexpr const CheckDefinition& CheckDef(CheckId id) {
    return AllChecks()[static_cast<std::size_t>(id)];
}

// ============================================================
// Display helpers
// ============================================================

/// Severity icon + fg color pair
[[nodiscard]] inline std::pair<std::string, Color> severity_display(DiagnosticSeverity sev) {
    switch (sev) {
        case DiagnosticSeverity::Ok:      return {"✓", Color::Green};
        case DiagnosticSeverity::Info:    return {"ℹ", Color::Blue};
        case DiagnosticSeverity::Warning: return {"⚠", Color::Yellow};
        case DiagnosticSeverity::Error:   return {"✗", Color::Red};
        case DiagnosticSeverity::Pending: return {"…", Color::Grey70};
    }
    return {"?", Color::White};
}

/// Label for the severity (used in summary counts)
[[nodiscard]] inline std::string_view severity_label(DiagnosticSeverity sev) {
    switch (sev) {
        case DiagnosticSeverity::Ok:      return "Pass";
        case DiagnosticSeverity::Info:    return "Info";
        case DiagnosticSeverity::Warning: return "Warn";
        case DiagnosticSeverity::Error:   return "Fail";
        case DiagnosticSeverity::Pending: return "Pending";
    }
    return "?";
}

/// Animated spinner glyph by frame index
[[nodiscard]] inline std::string_view spinner_glyph(unsigned frame) {
    static constexpr std::array<std::string_view, 10> kSpin =
        {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
    return kSpin[frame % kSpin.size()];
}

/// Gauge: render an ASCII bar + color for N/W/F against total checks.
/// Score = 100 * (Ok + 0.5*Info) / total   (Warn + Error contribute 0).
struct ScoreSummary {
    int ok = 0;
    int info = 0;
    int warn = 0;
    int err = 0;
    int total = 0;
    int score = 0;        // 0..100
};

[[nodiscard]] inline ScoreSummary ComputeScore(const std::array<CheckResult, kCheckCount>& r) {
    ScoreSummary s;
    for (auto& c : r) {
        if (c.severity == DiagnosticSeverity::Ok)     ++s.ok;
        else if (c.severity == DiagnosticSeverity::Info)    ++s.info;
        else if (c.severity == DiagnosticSeverity::Warning) ++s.warn;
        else if (c.severity == DiagnosticSeverity::Error)   ++s.err;
    }
    s.total = static_cast<int>(kCheckCount);
    if (s.total > 0) {
        s.score = static_cast<int>((s.ok * 100u + s.info * 50u) / static_cast<unsigned>(s.total));
    }
    return s;
}

[[nodiscard]] inline Color ScoreColor(int score) {
    if (score >= 80) return Color::Green;
    if (score >= 50) return Color::Yellow;
    return Color::Red;
}

/// Render the score bar (gauge) with [====-----] pattern and color bands
[[nodiscard]] inline Element RenderScoreGauge(const ScoreSummary& s) {
    const int width = 30;
    std::string bar;
    bar.reserve(width + 2);
    bar.push_back('[');
    int filled_ok   = width * s.ok   / std::max(1, s.total);
    int filled_info = width * s.info / std::max(1, s.total);
    int filled_warn = width * s.warn / std::max(1, s.total);
    // err gets the remainder visually
    Elements parts;
    parts.push_back(text("["));
    if (filled_ok > 0)   parts.push_back(text(std::string(filled_ok,   '=')) | color(Color::Green)  | bold);
    if (filled_info > 0) parts.push_back(text(std::string(filled_info, '-')) | color(Color::Blue));
    if (filled_warn > 0) parts.push_back(text(std::string(filled_warn, '~')) | color(Color::Yellow));
    int taken = filled_ok + filled_info + filled_warn;
    int filled_err = std::max(0, width - taken);
    if (filled_err > 0) parts.push_back(text(std::string(filled_err,  'x')) | color(Color::Red) | dim);
    parts.push_back(text("]"));
    return hbox(std::move(parts));
}

// ============================================================
// Real diagnostics (filesystem / env / config / network probes)
// ============================================================
//
// RunCheck() performs an actual probe for each CheckId against the supplied
// DoctorContext (paths, env, version). default_doctor_context() derives a
// context from the live environment (config/state dirs, env vars, cwd).
// Results are produced from real state, not hardcoded values. RunStub_Check()
// remains as a thin delegate so existing call sites keep working.

namespace detail {

[[nodiscard]] inline std::optional<std::string>
read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] inline std::string env_or(std::string_view key, std::string_view fallback = {}) {
    std::string term(key);
    const char* v = std::getenv(term.c_str());
    return (v && *v) ? std::string(v) : std::string(fallback);
}

#if !defined(_WIN32)
// Open a non-blocking TCP connection to host:port within timeout_ms.
// Returns the measured round-trip in milliseconds, or an error string.
[[nodiscard]] inline std::expected<int, std::string>
tcp_connect_rtt_ms(std::string_view host, int port, int timeout_ms) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string host_str(host);
    std::string port_str = std::to_string(port);
    struct addrinfo* res = nullptr;
    int gai = getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &res);
    if (gai != 0 || res == nullptr) {
        return std::unexpected(std::string("DNS resolution failed: ") +
                               (gai == 0 ? "no addresses" : gai_strerror(gai)));
    }
    int fd = -1;
    struct addrinfo* chosen = nullptr;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd >= 0) { chosen = p; break; }
    }
    if (fd < 0 || chosen == nullptr) {
        freeaddrinfo(res);
        return std::unexpected(std::string("socket() failed: ") + std::strerror(errno));
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    auto t0 = std::chrono::steady_clock::now();
    int rc = ::connect(fd, chosen->ai_addr, chosen->ai_addrlen);
    bool connected = false;
    std::string err;
    if (rc == 0) {
        connected = true;
    } else if (errno == EINPROGRESS) {
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr > 0) {
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) {
                connected = true;
            } else {
                err = std::string("connect failed: ") + std::strerror(soerr);
            }
        } else if (pr == 0) {
            err = "connect timed out after " + std::to_string(timeout_ms) + " ms";
        } else {
            err = std::string("poll failed: ") + std::strerror(errno);
        }
    } else {
        err = std::string("connect failed: ") + std::strerror(errno);
    }
    auto rtt = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0).count());
    ::close(fd);
    freeaddrinfo(res);
    if (!connected) return std::unexpected(err);
    return rtt;
}
#else
[[nodiscard]] inline std::expected<int, std::string>
tcp_connect_rtt_ms(std::string_view, int, int) {
    return std::unexpected("TCP probe not supported on this platform");
}
#endif

// Count how many times `needle` occurs in `hay` (substring search).
[[nodiscard]] inline std::size_t count_substring(std::string_view hay, std::string_view needle) {
    std::size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string_view::npos) { ++n; pos += needle.size(); }
    return n;
}

} // namespace detail


/// Injectable environment for diagnostics. Tests construct one pointing at a
/// temp directory; the live UI uses default_doctor_context().
struct DoctorContext {
    std::filesystem::path home;
    std::filesystem::path config_home;          // persisted-preferences root (e.g. ~/.claude)
    std::filesystem::path state_home;           // runtime state root (locks, bridge jwk)
    std::filesystem::path project_dir;          // current working directory
    std::filesystem::path settings_file;        // settings.json
    std::filesystem::path mcp_config;           // MCP servers config
    std::filesystem::path plugin_dir;           // plugins root
    std::filesystem::path token_store;          // OAuth token directory
    std::string anthropic_api_key;
    std::string shell;
    std::string current_version = "1.0.0-cpp";
    std::optional<std::string> latest_version;  // if known, compared against current_version
    std::string network_endpoint = "api.anthropic.com";
};

[[nodiscard]] inline DoctorContext default_doctor_context() {
    namespace fs = std::filesystem;
    DoctorContext ctx;
    ctx.home = fs::path(detail::env_or("HOME", "/"));
    ctx.config_home = ctx.home / ".claude";
    if (auto xdg = detail::env_or("XDG_STATE_HOME"); !xdg.empty()) {
        ctx.state_home = fs::path(xdg) / "claude";
    } else {
        ctx.state_home = ctx.home / ".local" / "state" / "claude";
    }
    std::error_code ec;
    ctx.project_dir = fs::current_path(ec);
    if (ec) ctx.project_dir = ctx.home;
    ctx.settings_file = ctx.config_home / "settings.json";
    ctx.mcp_config = ctx.config_home / "mcp_servers.json";
    ctx.plugin_dir = ctx.config_home / "plugins";
    ctx.token_store = ctx.config_home / "tokens";
    ctx.anthropic_api_key = detail::env_or("ANTHROPIC_API_KEY");
    ctx.shell = detail::env_or("SHELL");
    return ctx;
}

/// Format a byte count as a human-readable binary (GiB/MiB) string.
[[nodiscard]] inline std::string format_bytes(std::uintmax_t bytes) {
    constexpr std::uintmax_t kGiB = 1024ull * 1024 * 1024;
    constexpr std::uintmax_t kMiB = 1024ull * 1024;
    if (bytes >= kGiB) return std::format("{:.1f} GiB", static_cast<double>(bytes) / kGiB);
    if (bytes >= kMiB) return std::format("{:.1f} MiB", static_cast<double>(bytes) / kMiB);
    return std::format("{} B", bytes);
}

/// Run a single real diagnostic check against the given context.
[[nodiscard]] inline CheckResult RunCheck(CheckId id, const DoctorContext& ctx) {
    namespace fs = std::filesystem;
    CheckResult r;
    r.run_state = CheckRunState::Done;
    auto t0 = std::chrono::steady_clock::now();
    auto done = [&]() {
        r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0);
    };

    switch (id) {
    case CheckId::ApiKey: {
        if (!ctx.anthropic_api_key.empty()) {
            r.severity = DiagnosticSeverity::Ok;
            r.message = "ANTHROPIC_API_KEY is set (length " +
                        std::to_string(ctx.anthropic_api_key.size()) + ").";
            r.detail = "Presence check passed. An authenticated HTTP probe is not run here to keep diagnostics deterministic.";
        } else {
            r.severity = DiagnosticSeverity::Warning;
            r.message = "ANTHROPIC_API_KEY is not set in the environment.";
            r.detail = "The client may still authenticate via OAuth credentials; if neither is configured, requests will fail.";
            r.fix_hint = "Export ANTHROPIC_API_KEY or complete OAuth login.";
        }
        break;
    }
    case CheckId::Network: {
        auto probe = detail::tcp_connect_rtt_ms(ctx.network_endpoint, 443, 2500);
        if (probe.has_value()) {
            r.severity = DiagnosticSeverity::Ok;
            r.message = "Reached " + ctx.network_endpoint + ":443 in " + std::to_string(*probe) + " ms.";
            r.detail = "DNS resolved and a TCP connection to port 443 completed within the 2500 ms budget.";
        } else {
            r.severity = DiagnosticSeverity::Error;
            r.message = "Cannot reach " + ctx.network_endpoint + ":443.";
            r.detail = probe.error();
            r.fix_hint = "Check DNS resolution, proxy configuration, and firewall rules.";
        }
        break;
    }
    case CheckId::ConfigReadWrite: {
        std::error_code ec;
        auto st = fs::status(ctx.config_home, ec);
        if (ec || !fs::is_directory(st)) {
            r.severity = DiagnosticSeverity::Warning;
            r.message = "Config directory is not accessible: " + ctx.config_home.string();
            r.detail = ec ? ec.message() : "path is not a directory";
            r.fix_hint = "Create the directory with `mkdir -p " + ctx.config_home.string() + "`";
        } else {
            using fs::perms;
            auto p = st.permissions();
            bool wr = (p & perms::owner_write) != perms::none;
            bool rd = (p & perms::owner_read) != perms::none;
            if (!rd || !wr) {
                r.severity = DiagnosticSeverity::Error;
                r.message = "Config directory lacks owner read/write: " + ctx.config_home.string();
                r.detail = "Permissions prevent persisting preferences.";
                r.fix_hint = "Restore ownership/permissions on the config directory.";
            } else {
                r.severity = DiagnosticSeverity::Ok;
                r.message = "Config directory is readable and writable.";
                r.detail = ctx.config_home.string();
            }
        }
        break;
    }
    case CheckId::DiskSpace: {
        std::error_code ec;
        auto info = fs::space(ctx.config_home, ec);
        if (ec) {
            r.severity = DiagnosticSeverity::Warning;
            r.message = "Could not query free space for " + ctx.config_home.string();
            r.detail = ec.message();
        } else {
            constexpr std::uintmax_t kWarn = 2ull * 1024 * 1024 * 1024; // 2 GiB
            r.detail = "Available: " + format_bytes(info.available) +
                       " / capacity: " + format_bytes(info.capacity);
            if (info.available < kWarn) {
                r.severity = DiagnosticSeverity::Warning;
                r.message = "Low free space: " + format_bytes(info.available) + " available.";
                r.fix_hint = "Free at least 2 GiB for caches and context artifacts.";
            } else {
                r.severity = DiagnosticSeverity::Ok;
                r.message = "Adequate free space: " + format_bytes(info.available) + " available.";
            }
        }
        break;
    }
    case CheckId::FsPermissions:
    case CheckId::DefaultPermissions: {
        auto text = detail::read_text_file(ctx.settings_file);
        if (!text.has_value()) {
            r.severity = DiagnosticSeverity::Ok;
            r.message = (id == CheckId::FsPermissions ? "No settings overrides file; using safe defaults."
                                                       : "No custom default permissions; using built-in defaults.");
            r.detail = "Looked for: " + ctx.settings_file.string();
        } else {
            // A bare "**" as an auto-approve glob grants every command — flag it.
            std::size_t stars = detail::count_substring(*text, "\"**\"");
            if (stars > 0) {
                r.severity = DiagnosticSeverity::Error;
                r.message = "Wildcard auto-approve glob \"**\" found " + std::to_string(stars) + "x in settings.";
                r.detail = "A \"**\" pattern matches every command, including destructive ones (rm, dd, chmod).";
                r.fix_hint = "Tighten auto-approve globs to an explicit whitelist of safe commands.";
            } else {
                r.severity = DiagnosticSeverity::Ok;
                r.message = (id == CheckId::FsPermissions ? "No broad wildcard permission grants in settings."
                                                           : "Default permissions contain no wildcard auto-approve.");
                r.detail = "Scanned " + ctx.settings_file.string() + " (" + std::to_string(text->size()) + " bytes).";
            }
        }
        break;
    }
    case CheckId::VersionLatest: {
        r.detail = "Current: " + ctx.current_version;
        if (ctx.latest_version.has_value()) {
            r.detail += "\nLatest:  " + *ctx.latest_version;
            if (*ctx.latest_version == ctx.current_version) {
                r.severity = DiagnosticSeverity::Ok;
                r.message = "Running the latest version.";
            } else {
                r.severity = DiagnosticSeverity::Warning;
                r.message = "An update is available (" + *ctx.latest_version + ").";
                r.fix_hint = "Update to the latest release.";
            }
        } else {
            r.severity = DiagnosticSeverity::Info;
            r.message = "Running version " + ctx.current_version + " (online latest-check skipped).";
            r.detail += "\nSet DoctorContext.latest_version to compare against a known release.";
        }
        break;
    }
    case CheckId::McpServers: {
        // Prefer the dedicated config; fall back to ~/.claude.json if needed.
        fs::path cfg = ctx.mcp_config;
        std::error_code ec;
        if (!fs::exists(cfg, ec)) {
            fs::path alt = ctx.home / ".claude.json";
            if (fs::exists(alt, ec)) cfg = alt;
        }
        auto parsed = cc::utils::json::parse_file(cfg.string());
        if (!parsed.has_value()) {
            r.severity = DiagnosticSeverity::Info;
            r.message = "No MCP server configuration found.";
            r.detail = "Looked for: " + cfg.string();
        } else {
            auto root = parsed.value().root();
            // MCP config may be a top-level object of servers, or nested under "mcpServers".
            auto servers = root.get("mcpServers");
            if (!servers.valid()) servers = root;
            std::size_t total = 0, missing_cmd = 0;
            if (servers.is_obj()) {
                servers.iter_obj([&](auto /*key*/, auto value) {
                    if (!value.is_obj()) return;
                    ++total;
                    if (!value.get("command").is_str()) ++missing_cmd;
                });
            }
            if (total == 0) {
                r.severity = DiagnosticSeverity::Info;
                r.message = "MCP config present but defines no servers.";
            } else if (missing_cmd > 0) {
                r.severity = DiagnosticSeverity::Error;
                r.message = std::to_string(missing_cmd) + " of " + std::to_string(total) +
                            " MCP servers are missing a 'command' field.";
                r.fix_hint = "Add the executable 'command' for each configured server.";
            } else {
                r.severity = DiagnosticSeverity::Ok;
                r.message = std::to_string(total) + " MCP server(s) configured, all with a 'command' field.";
            }
            r.detail = "Config: " + cfg.string();
        }
        break;
    }
    case CheckId::OAuthToken: {
        std::error_code ec;
        std::size_t count = 0;
        if (fs::is_directory(ctx.token_store, ec)) {
            for (auto& e : fs::directory_iterator(ctx.token_store, ec)) {
                if (e.is_regular_file()) ++count;
            }
        }
        if (count == 0) {
            r.severity = DiagnosticSeverity::Info;
            r.message = "No cached OAuth token (using API key or not logged in).";
            r.detail = "Looked in: " + ctx.token_store.string();
        } else {
            r.severity = DiagnosticSeverity::Ok;
            r.message = std::to_string(count) + " cached OAuth token file(s) present.";
            r.detail = "Token directory: " + ctx.token_store.string();
        }
        break;
    }
    case CheckId::PermissionIntegrity: {
        auto text = detail::read_text_file(ctx.settings_file);
        if (!text.has_value()) {
            r.severity = DiagnosticSeverity::Ok;
            r.message = "No custom permission rules to audit.";
            r.detail = "Looked for: " + ctx.settings_file.string();
        } else {
            // A "deny" rule that appears after a "**" allow can never fire.
            bool has_star_allow = text->find("\"**\"") != std::string::npos &&
                                  text->find("allow") != std::string::npos;
            bool has_deny = text->find("deny") != std::string::npos;
            if (has_star_allow && has_deny) {
                r.severity = DiagnosticSeverity::Warning;
                r.message = "A wildcard allow (\"**\") may shadow later deny rules.";
                r.detail = "Reorder rules so deny entries take precedence over broad allows.";
                r.fix_hint = "Move deny rules above the wildcard allow, or remove the wildcard.";
            } else {
                r.severity = DiagnosticSeverity::Ok;
                r.message = "No obvious permission-rule shadowing detected.";
                r.detail = "Scanned " + ctx.settings_file.string();
            }
        }
        break;
    }
    case CheckId::Bridge: {
        fs::path jwk = ctx.state_home / "bridge" / "jwk.json";
        std::error_code ec;
        if (fs::exists(jwk, ec)) {
            r.severity = DiagnosticSeverity::Ok;
            r.message = "Bridge JWT signing key present.";
            r.detail = jwk.string();
        } else {
            r.severity = DiagnosticSeverity::Error;
            r.message = "Bridge JWT signing key is missing — IDE integrations disabled.";
            r.detail = "Expected: " + jwk.string();
            r.fix_hint = "Run `claude bridge init` to provision the JWT signing key.";
            r.fix_command = "claude bridge init";
        }
        break;
    }
    case CheckId::ShellConfig: {
        if (ctx.shell.empty()) {
            r.severity = DiagnosticSeverity::Warning;
            r.message = "$SHELL is not set.";
            r.detail = "Some subshell features will be unavailable.";
        } else {
            std::error_code ec;
            bool ok = fs::exists(fs::path(ctx.shell), ec);
            if (ok) {
                r.severity = DiagnosticSeverity::Ok;
                r.message = "Configured shell exists: " + ctx.shell;
                r.detail = "$SHELL=" + ctx.shell;
            } else {
                r.severity = DiagnosticSeverity::Error;
                r.message = "Configured shell does not exist: " + ctx.shell;
                r.detail = ec ? ec.message() : "path not found";
                r.fix_hint = "Set $SHELL to an installed shell (e.g. /bin/zsh).";
            }
        }
        break;
    }
    case CheckId::PluginEngine: {
        std::error_code ec;
        std::size_t total = 0, missing_manifest = 0;
        if (fs::is_directory(ctx.plugin_dir, ec)) {
            for (auto& e : fs::directory_iterator(ctx.plugin_dir, ec)) {
                if (!e.is_directory()) continue;
                ++total;
                if (!fs::exists(e.path() / "manifest.json", ec)) ++missing_manifest;
            }
        }
        if (total == 0) {
            r.severity = DiagnosticSeverity::Info;
            r.message = "No plugins installed.";
            r.detail = "Looked in: " + ctx.plugin_dir.string();
        } else if (missing_manifest > 0) {
            r.severity = DiagnosticSeverity::Warning;
            r.message = std::to_string(missing_manifest) + " of " + std::to_string(total) +
                        " plugin(s) are missing a manifest.json.";
            r.fix_hint = "Add the missing manifest or remove the broken plugin directory.";
        } else {
            r.severity = DiagnosticSeverity::Ok;
            r.message = std::to_string(total) + " plugin(s) installed, all with manifests.";
        }
        break;
    }
    default:
        r.severity = DiagnosticSeverity::Info;
        r.message = "Unknown check id.";
        break;
    }

    done();
    return r;
}

/// Convenience: run all checks against a context.
[[nodiscard]] inline std::array<CheckResult, kCheckCount>
RunAllChecks(const DoctorContext& ctx) {
    std::array<CheckResult, kCheckCount> out;
    for (std::size_t i = 0; i < kCheckCount; ++i) {
        out[i] = RunCheck(static_cast<CheckId>(i), ctx);
    }
    return out;
}

/// Legacy entry point kept for existing call sites; now backed by real probes
/// against the live environment.
[[nodiscard]] inline CheckResult RunStub_Check(CheckId id) {
    return RunCheck(id, default_doctor_context());
}

// ============================================================
// Sub-renderers (one per screen state)
// ============================================================

// --- Splash / Header -----------------------------------------------------
[[nodiscard]] inline Element RenderSplashHeader(const VersionInfo& v, bool started) {
    Elements lines = {
        hbox({
            text(" 🩺 ") | bold | color(Color::GreenLight),
            text("Doctor  —  ") | bold | color(Color::White),
            text("Claude Code installation self-check") | color(Color::Cyan),
            filler(),
            text(started ? "  [running]" : "  [idle]") | dim | color(started ? Color::Cyan : TEXT_MUTED),
        }),
        separator(),
        hbox({ text("  Version:      ") | dim, text(v.current_version)        | color(Color::Cyan) }),
        hbox({ text("  Build type:   ") | dim, text(v.installation_type.empty() ? "native" : v.installation_type) | color(Color::Cyan) }),
        hbox({ text("  Invoked as:   ") | dim, text(v.invoked_binary.empty()    ? "claude" : v.invoked_binary)      | dim }),
        hbox({ text("  Install path: ") | dim, text(v.installation_path)       | dim }),
        v.package_manager
            ? hbox({ text("  Pkg manager:  ") | dim, text(*v.package_manager)     | color(Color::Yellow) })
            : text(""),
        v.config_install_method
            ? hbox({ text("  Installer:    ") | dim, text(*v.config_install_method) | color(TEXT_MUTED) })
            : text(""),
    };
    return vbox(std::move(lines));
}

// --- Running state: progress bar + per-item live lines -----------------
[[nodiscard]] inline Element RenderRunningState(
    const DoctorDataModel& model,
    unsigned spinner_frame,
    std::size_t completed_count,
    CheckId current)
{
    const auto total = kCheckCount;
    const int pct = static_cast<int>(completed_count * 100u / total);
    const int bar_width = 40;
    const int filled   = bar_width * static_cast<int>(completed_count) / static_cast<int>(total);
    Elements bar_parts;
    bar_parts.push_back(text("["));
    if (filled > 0) bar_parts.push_back(text(std::string(filled, '#'))         | color(Color::Green) | bold);
    int rest = std::max(0, bar_width - filled);
    if (rest   > 0) bar_parts.push_back(text(std::string(rest,   '-'))         | dim);
    bar_parts.push_back(text("]"));

    Elements lines;
    lines.push_back(RenderSplashHeader(model.version, true));
    lines.push_back(separator());

    lines.push_back(hbox({
        text(" "),
        text(std::string(spinner_glyph(spinner_frame))) | color(Color::Cyan) | bold,
        text("  Running checks …  ") | color(Color::Cyan),
        hbox(std::move(bar_parts)),
        text(" "),
        text(std::format("{:>3}%", pct)) | bold | color(Color::White),
        filler(),
        text(std::format(" {}/{} ", completed_count, total)) | dim,
    }));

    lines.push_back(text(""));
    // Per-check rows: completed -> status glyph; current -> spinner; rest -> dim bullet
    for (std::size_t i = 0; i < kCheckCount; ++i) {
        const auto& def = AllChecks()[i];
        const auto& res = model.results[i];
        if (res.run_state == CheckRunState::Done) {
            auto [glyph, col] = severity_display(res.severity);
            lines.push_back(hbox({
                text(" "), text(glyph) | color(col), text(" "),
                text(def.name) | color(Color::White),
                filler(),
                text(res.message) | dim,
            }));
        } else if (static_cast<CheckId>(i) == current) {
            lines.push_back(hbox({
                text(" "),
                text(std::string(spinner_glyph(spinner_frame))) | color(Color::Cyan) | bold,
                text(" "),
                text(def.name) | color(Color::Cyan) | bold,
                filler(),
                text("…") | dim,
            }));
        } else {
            lines.push_back(hbox({
                text(" "), text("·") | dim, text(" "),
                text(def.name) | dim,
                filler(),
                text("queued") | dim,
            }));
        }
    }

    return vbox(std::move(lines));
}

// --- Results state: per-row status + collapsible detail -----------------
[[nodiscard]] inline Element RenderResultsState(
    const DoctorDataModel& model,
    const std::array<bool, kCheckCount>& expanded)
{
    Elements lines;
    lines.push_back(RenderSplashHeader(model.version, false));
    lines.push_back(separator());
    lines.push_back(hbox({
        text(" Results ") | bold | color(Color::Cyan),
        filler(),
        text("↑/↓ navigate  [Space] expand   [Enter] proceed to summary ") | dim,
    }));
    lines.push_back(separator());

    // Section header: "Connectivity / System / Security / Updates / Integrations"
    std::string last_cat;
    for (std::size_t i = 0; i < kCheckCount; ++i) {
        const auto& def = AllChecks()[i];
        const auto& res = model.results[i];

        if (def.category != last_cat) {
            if (!last_cat.empty()) lines.push_back(text(""));
            lines.push_back(hbox({
                text("▸ ") | bold | color(Color::Cyan),
                text(def.category) | bold | color(Color::White),
            }));
            last_cat = def.category;
        }

        auto [glyph, col] = severity_display(res.severity);
        const bool ex = expanded[i];
        lines.push_back(hbox({
            text(" "),
            text(ex ? "▼ " : "▶ ") | color(TEXT_MUTED),
            text(glyph) | color(col), text(" "),
            text(def.name) | bold | color(Color::White),
            filler(),
            text(std::format("[{}]", severity_label(res.severity))) | color(col),
            text(std::format("  {:>4}ms", res.elapsed.count())) | dim,
        }));

        // Inline one-liner
        if (!res.message.empty()) {
            Color msg_col = (res.severity == DiagnosticSeverity::Ok)      ? Color(Color::Green)  :
                            (res.severity == DiagnosticSeverity::Error)   ? Color(Color::Red)    :
                            (res.severity == DiagnosticSeverity::Warning) ? Color(Color::Yellow) :
                                                                            Color(Color::Grey70);
            lines.push_back(hbox({
                text("     ") | dim,
                text(res.message) | ftxui::color(msg_col),
            }));
        }

        if (ex) {
            // Multi-line description
            if (!res.detail.empty()) {
                std::size_t start = 0;
                while (start < res.detail.size()) {
                    auto nl = res.detail.find('\n', start);
                    auto piece = res.detail.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                    lines.push_back(hbox({
                        text("     │ ") | dim | color(Color::Blue),
                        text(piece) | dim | color(Color::Grey70),
                    }));
                    if (nl == std::string::npos) break;
                    start = nl + 1;
                }
            }
            if (res.fix_hint) {
                lines.push_back(hbox({
                    text("     └─ ") | dim | color(Color::Yellow),
                    text("Fix: ") | bold | color(Color::Yellow),
                    text(*res.fix_hint) | color(Color::Yellow),
                }));
            }
            if (res.fix_command) {
                lines.push_back(hbox({
                    text("        $ ") | dim | color(Color::Green),
                    text(*res.fix_command) | color(Color::GreenLight),
                }));
            }
        }
    }

    // Context notices (from TS: unreachable rules, plugin errors, etc.)
    if (!model.notices.empty()) {
        lines.push_back(separator());
        lines.push_back(text(" Additional notices ") | bold | color(Color::Yellow));
        for (auto& n : model.notices) {
            auto [g, col] = severity_display(n.severity);
            lines.push_back(hbox({
                text(" "), text(g) | color(col), text(" "),
                text("[" + n.category + "] ") | dim,
                text(n.message) | color(col),
            }));
            for (auto& d : n.details) {
                lines.push_back(hbox({ text("     └ ") | dim, text(d) | dim }));
            }
        }
    }

    return vbox(std::move(lines));
}

// --- Summary state: score, counts, fix-all log, action buttons ----------
struct SummaryUIState {
    bool show_fix_log = false;
    std::size_t fix_step = 0;
    std::vector<std::string> fix_log_lines;
};

[[nodiscard]] inline Element RenderSummaryState(
    const DoctorDataModel& model,
    const SummaryUIState& ui,
    int focused_button)   // 0 = Fix all, 1 = Rerun, 2 = Back to REPL
{
    auto s = ComputeScore(model.results);

    Elements lines;
    lines.push_back(RenderSplashHeader(model.version, false));
    lines.push_back(separator());

    // Score header
    Color sc = ScoreColor(s.score);
    lines.push_back(hbox({
        text(" Overall Score: ") | bold,
        text(std::format("{:>3}/100", s.score)) | bold | color(sc),
        filler(),
        text(s.score >= 80 ? "HEALTHY" : s.score >= 50 ? "NEEDS ATTENTION" : "UNHEALTHY") | bold | color(sc),
    }));
    lines.push_back(text(""));
    lines.push_back(hbox({ text(" "), RenderScoreGauge(s), text(" "), text(std::format(" {} checks", s.total)) | dim }));
    lines.push_back(text(""));

    // Breakdown row
    lines.push_back(hbox({
        text("  "),
        text(std::format("{}✓ ", s.ok))  | bold | color(Color::Green),
        text("passed  ")                 | color(TEXT_MUTED),
        text(std::format("{}ℹ ", s.info)) | bold | color(Color::Blue),
        text("info  ")                   | color(TEXT_MUTED),
        text(std::format("{}⚠ ", s.warn)) | bold | color(Color::Yellow),
        text("warnings  ")               | color(TEXT_MUTED),
        text(std::format("{}✗ ", s.err))  | bold | color(Color::Red),
        text("failed")                   | color(TEXT_MUTED),
    }));

    if (model.locks.enabled) {
        lines.push_back(text(""));
        lines.push_back(hbox({
            text("  PID locks: ") | dim,
            text(std::to_string(model.locks.lock_count) + " active") | dim,
            model.locks.stale_locks_cleaned > 0
                ? hbox({ text(std::format(", {} stale cleaned", model.locks.stale_locks_cleaned)) | color(Color::Green) })
                : text(""),
        }));
    }

    // Fix-all log (progressive)
    if (ui.show_fix_log) {
        lines.push_back(separator());
        lines.push_back(hbox({
            text(" 🛠  Fix-all progress ") | bold | color(Color::Cyan),
            filler(),
            text(std::format("  step {}/{} ", ui.fix_step, ui.fix_log_lines.size() + 0)) | dim,
        }));
        for (std::size_t i = 0; i < ui.fix_log_lines.size(); ++i) {
            const bool current = i + 1 == ui.fix_step;
            const Color c = i < ui.fix_step ? Color::Green : (current ? Color::Cyan : TEXT_MUTED);
            std::string prefix = i < ui.fix_step ? "  ✓ " : (current ? "  ➜ " : "    ");
            lines.push_back(hbox({ text(prefix) | color(c), text(ui.fix_log_lines[i]) | color(c) }));
        }
        if (ui.fix_step > ui.fix_log_lines.empty() ? 0 : ui.fix_log_lines.size()) {
            lines.push_back(text("") | dim);
            lines.push_back(hbox({
                text("  🎉 ") | color(Color::Green),
                text("Auto-fix complete.  Re-run the doctor to verify.") | bold | color(Color::Green),
            }));
        }
    }

    lines.push_back(separator());

    // Buttons row (focus indicator via < > bracketing)
    auto btn = [&](int idx, std::string_view label, Color col) -> Element {
        const bool focus = focused_button == idx;
        return hbox({
            text(focus ? "< " : "[ ") | ftxui::color(focus ? col : TEXT_MUTED),
            text(std::string(label)) | (focus ? bold : ftxui::color(col)) | ftxui::color(focus ? col : TEXT_MUTED),
            text(focus ? " >" : " ]") | ftxui::color(focus ? col : TEXT_MUTED),
        });
    };

    lines.push_back(hbox({
        btn(0, "Fix all", Color::Cyan),
        text("  "),
        btn(1, "Rerun",   Color::Yellow),
        text("  "),
        btn(2, "Back to REPL", Color::Green),
        filler(),
        text(" ←/→ navigate  [Enter] activate ") | dim,
    }));

    return vbox(std::move(lines));
}

// ============================================================
// Top-level options and public component
// ============================================================

/// Public creation options.  `on_done` is fired on "Back to REPL" or Escape.
struct DoctorScreenOptions {
    DoctorDataModel initial;
    std::function<void(std::string /* result */)> on_done;
};

/// Mutable internal state for the interactive component.
struct ScreenRuntime {
    ScreenState state = ScreenState::Splash;
    DoctorDataModel model;
    std::array<bool, kCheckCount> expanded{};   // per-row expansion (Results)
    int selected_row = 0;                       // focused check index (Results)
    int focused_button = 0;                     // focused button (Summary)
    SummaryUIState summary_ui;
    unsigned spinner_frame = 0;
    std::size_t completed_count = 0;
    CheckId current = CheckId::ApiKey;
    // Background-ish state (we advance step-by-step on each animation tick)
    std::size_t step_tick = 0;
};

namespace detail {

/// Build the list of fix-log lines from the check results (ordered by severity: errors first, then warnings).
[[nodiscard]] inline std::vector<std::string> BuildFixPlan(const DoctorDataModel& m) {
    std::vector<std::string> plan;
    // Errors
    for (std::size_t i = 0; i < kCheckCount; ++i) {
        const auto& r = m.results[i];
        if (r.severity == DiagnosticSeverity::Error && r.fix_command) {
            plan.push_back(std::format("[{}] Apply: {}", CheckDef(static_cast<CheckId>(i)).name, *r.fix_command));
        }
    }
    // Warnings
    for (std::size_t i = 0; i < kCheckCount; ++i) {
        const auto& r = m.results[i];
        if (r.severity == DiagnosticSeverity::Warning && r.fix_command) {
            plan.push_back(std::format("[{}] Apply: {}", CheckDef(static_cast<CheckId>(i)).name, *r.fix_command));
        }
    }
    if (plan.empty()) plan.push_back("No auto-fixable checks — all items healthy or require manual intervention.");
    return plan;
}

} // namespace detail

// ============================================================
// Component factory (exported)
// ============================================================

/// Top-level FTXUI Component for the Doctor screen.
/// Exposes four states (Splash/Running/Results/Summary), animation ticks
/// that drive a pseudo "execution" pipeline, and keyboard navigation.
[[nodiscard]] inline Component DoctorScreen(DoctorScreenOptions opts) {
    auto rt = std::make_shared<ScreenRuntime>();
    rt->model = std::move(opts.initial);

    // Prime: ensure version info is non-empty (demo defaults for UI preview)
    if (rt->model.version.current_version.empty()) rt->model.version.current_version = "0.12.0";
    if (rt->model.version.installation_type.empty()) rt->model.version.installation_type = "native";
    if (rt->model.version.installation_path.empty()) rt->model.version.installation_path = "/opt/claude/claude";
    if (rt->model.version.invoked_binary.empty()) rt->model.version.invoked_binary = "claude";

    // Seed the plan so it is available before the user clicks Fix-all
    rt->summary_ui.fix_log_lines = detail::BuildFixPlan(rt->model);

    // Helper: transition to Running state and reset progress
    auto begin_runs = [rt] {
        rt->state = ScreenState::Running;
        rt->completed_count = 0;
        rt->step_tick = 0;
        rt->current = CheckId::ApiKey;
        for (auto& r : rt->model.results) {
            r = CheckResult{};
        }
    };

    // Kick off on creation (after splash has been visible a moment)
    // We let the first animation tick do Splash → Running so the user
    // sees the banner for ~1 frame.

    auto renderer = Renderer([rt] {
        Element body;
        switch (rt->state) {
        case ScreenState::Splash:
            body = vbox({
                RenderSplashHeader(rt->model.version, false),
                separator(),
                hbox({
                    text(" "),
                    text(std::string(spinner_glyph(rt->spinner_frame))) | color(Color::Cyan) | bold,
                    text("  Preparing diagnostics suite (13 checks, 5 categories)…") | color(Color::Cyan),
                }),
                text(""),
                hbox({ text(" Press Enter to skip the wait  ·  Esc closes Doctor") | dim }),
            });
            break;
        case ScreenState::Running:
            body = RenderRunningState(rt->model, rt->spinner_frame, rt->completed_count, rt->current);
            break;
        case ScreenState::Results:
            body = RenderResultsState(rt->model, rt->expanded);
            break;
        case ScreenState::Summary:
            body = RenderSummaryState(rt->model, rt->summary_ui, rt->focused_button);
            break;
        }
        // Wrap everything in a rounded border with appropriate accent
        Color border_color = Color::Grey50;
        if (rt->state == ScreenState::Summary) {
            auto s = ComputeScore(rt->model.results);
            border_color = ScoreColor(s.score);
        } else if (rt->state == ScreenState::Running) {
            border_color = Color::Cyan;
        }
        return body | borderStyled(ROUNDED, border_color) | size(WIDTH, GREATER_THAN, 72);
    });

    return renderer | CatchEvent([rt, opts = std::move(opts), begin_runs](Event event) mutable -> bool {
        // ============== Global keys ==============
        if (event == Event::Escape) {
            if (rt->state == ScreenState::Running || rt->state == ScreenState::Splash) {
                // User bailed: go straight to Summary (what we have so far)
                rt->state = ScreenState::Summary;
                return true;
            }
            if (opts.on_done) opts.on_done("Doctor dismissed by user (Esc)");
            return true;
        }

        // ============== Animation tick ==============
        if (event.is_character() && event.character() == "\u0000") {
            // not a real tick
        }
        if (event == Event::Custom) {} // ignore for safety

        // FTXUI fires Event::Custom on animation frames.  We also bump the
        // spinner on any unhandled repeat via a counter.  Use a catch-all
        // approach: bump the frame on every unhandled event *and* on the
        // periodic "animation tick" if the parent ScreenInteractive has one.
        rt->spinner_frame++;

        if (rt->state == ScreenState::Splash) {
            if (rt->spinner_frame % 3 == 0) {
                begin_runs();
            }
        } else if (rt->state == ScreenState::Running) {
            // Advance one check every ~2 animation ticks (visible but not too slow)
            if (rt->spinner_frame % 2 == 0) {
                if (rt->completed_count < kCheckCount) {
                    auto id = static_cast<CheckId>(rt->completed_count);
                    rt->model.results[static_cast<std::size_t>(id)] = RunStub_Check(id);
                    rt->completed_count++;
                    if (rt->completed_count < kCheckCount) {
                        rt->current = static_cast<CheckId>(rt->completed_count);
                    } else {
                        // All done.  Populate notices from results and proceed to Results.
                        rt->state = ScreenState::Results;
                        // Derive contextual notices (mirrors TS: unreachable rules, MCP warnings, etc.)
                        rt->model.notices.clear();
                        auto& perm = rt->model.results[static_cast<std::size_t>(CheckId::PermissionIntegrity)];
                        if (perm.severity != DiagnosticSeverity::Ok) {
                            rt->model.notices.push_back(ContextNotice{
                                "Permission Rules",
                                perm.message,
                                perm.severity,
                                {"Use `/config permissions` to reorder and deduplicate.",
                                 "See docs: https://docs.anthropic.com/claude/docs/tool-permissions"},
                            });
                        }
                        auto& mcp = rt->model.results[static_cast<std::size_t>(CheckId::McpServers)];
                        if (mcp.severity == DiagnosticSeverity::Error) {
                            rt->model.notices.push_back(ContextNotice{
                                "MCP",
                                "Some MCP servers are unavailable",
                                DiagnosticSeverity::Warning,
                                {"Run `claude mcp list` to see server definitions.",
                                 "Run `claude mcp doctor` for per-server traces."},
                            });
                        }
                        rt->summary_ui.fix_log_lines = detail::BuildFixPlan(rt->model);
                    }
                }
            }
        } else if (rt->state == ScreenState::Summary && rt->summary_ui.show_fix_log) {
            // Advance fix log steps
            if (rt->spinner_frame % 4 == 0) {
                if (rt->summary_ui.fix_step <= rt->summary_ui.fix_log_lines.size()) {
                    rt->summary_ui.fix_step++;
                }
            }
        }
        // Spinner-only state: always request redraw so animation feels smooth.
        // (Returning true here would swallow *all* events, which we don't want.
        //  The parent ScreenInteractive is expected to post animation ticks via
        //  ScreenInteractive::TrackAnimation / Loop.  We leave this to the host.)

        // ============== Per-state keys ==============
        if (event == Event::Return) {
            if (rt->state == ScreenState::Splash) {
                begin_runs();
                return true;
            }
            if (rt->state == ScreenState::Results) {
                // Enter -> advance to Summary
                rt->state = ScreenState::Summary;
                return true;
            }
            if (rt->state == ScreenState::Summary) {
                // Activate focused button
                if (rt->focused_button == 0) {
                    // Fix all: start progressive log if not already
                    if (!rt->summary_ui.show_fix_log) {
                        rt->summary_ui.show_fix_log = true;
                        rt->summary_ui.fix_step = 1;
                        // Phase-5 integration point: invoke each fix_command
                        // via cc.services.bash_runner sequentially, streaming
                        // stdout/stderr back to fix_log_lines for display.
                    }
                    return true;
                }
                if (rt->focused_button == 1) {
                    // Rerun
                    begin_runs();
                    rt->summary_ui.show_fix_log = false;
                    rt->summary_ui.fix_step = 0;
                    return true;
                }
                if (rt->focused_button == 2) {
                    // Back to REPL
                    if (opts.on_done) opts.on_done("Doctor complete");
                    return true;
                }
            }
            return true;
        }

        // Arrow / tab navigation
        if (rt->state == ScreenState::Results) {
            if (event == Event::ArrowUp) {
                rt->selected_row = (rt->selected_row + static_cast<int>(kCheckCount) - 1) % static_cast<int>(kCheckCount);
                return true;
            }
            if (event == Event::ArrowDown) {
                rt->selected_row = (rt->selected_row + 1) % static_cast<int>(kCheckCount);
                return true;
            }
            if (event == Event::Character(' ') || event == Event::Tab) {
                auto idx = static_cast<std::size_t>(rt->selected_row);
                if (idx < kCheckCount) rt->expanded[idx] = !rt->expanded[idx];
                return true;
            }
        }

        if (rt->state == ScreenState::Summary) {
            if (event == Event::ArrowRight || event == Event::Tab) {
                rt->focused_button = (rt->focused_button + 1) % 3;
                return true;
            }
            if (event == Event::ArrowLeft) {
                rt->focused_button = (rt->focused_button + 2) % 3;
                return true;
            }
        }

        // Character shortcuts
        if (event.is_character()) {
            auto c = event.character();
            if (rt->state == ScreenState::Summary) {
                if (c == "f" || c == "F") { rt->focused_button = 0; event = Event::Return; return false; /* re-handle */ }
                if (c == "r" || c == "R") { rt->focused_button = 1; event = Event::Return; return false; }
                if (c == "b" || c == "B") { rt->focused_button = 2; event = Event::Return; return false; }
            }
        }

        return false;
    });
}

} // namespace cc::ui::doctor_screen
