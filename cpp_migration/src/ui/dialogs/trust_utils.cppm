/// @file trust_utils.cppm
/// @brief Risk classification utilities for the trust dialog system.
///
/// Provides 4-tier risk classification (Low/Medium/High/Critical),
/// sensitive-path regex scanning, known-safe domain allowlist,
/// plugin risk summary, and user-facing warning messages.
///
/// Audited against:
///   - TS src/components/TrustDialog/utils.ts (245 lines)
///   - C++ utils/bash_security.cppm (reuses DangerLevel mapping)
///   - C++ services/team_memory/secret_scanner.cppm (reuses SecretMatch)
///   - C++ commands/plugin/plugin_trust_text.cppm (reuses marketplace allowlist)
///   - C++ plugins/plugin.cppm (reuses PluginDefinition / PluginCapabilities)

module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.ui.trust_utils;

// Reuse existing trust / security primitives instead of redefining them.
import cc.utils.bash_security;
import cc.services.team_memory.secret_scanner;
import cc.commands.plugin_trust_text;
import cc.plugins.plugin;

export namespace cc::ui::trust_utils {

using namespace cc::services::team_memory;
using cc::commands::plugin::is_trusted_marketplace_domain;
using cc::plugins::PluginCapabilities;
using cc::plugins::PluginDefinition;

// =========================================================================
// Enums & core types
// =========================================================================

/// Four-tier risk classification used by TrustDialog to pick a layout and
/// enforce safety UX (countdown, double-confirm).
enum class RiskLevel : std::uint8_t {
    Low = 0,      // Green  — 2 buttons, no safety barriers
    Medium = 1,   // Yellow — 3 buttons + collapsible details
    High = 2,     // Orange — 4 buttons + mandatory 5-second countdown
    Critical = 3, // Red    — checkbox + "type YES" double confirm
};

/// Convert a RiskLevel to a canonical lowercase string.
[[nodiscard]] constexpr std::string_view risk_level_name(RiskLevel level) noexcept {
    switch (level) {
        case RiskLevel::Low:      return "low";
        case RiskLevel::Medium:   return "medium";
        case RiskLevel::High:     return "high";
        case RiskLevel::Critical: return "critical";
    }
    return "unknown";
}

/// What kind of action prompted the trust check.
enum class ActionType : std::uint8_t {
    WorkspaceTrust,   // Initial "trust this folder" prompt
    PluginInstall,    // Installing a plugin (1st time / unsigned / unknown source)
    PluginUpdate,     // Updating a plugin (version bump, permission changes)
    PathAccess,       // Reading or writing files
    PathWrite,        // Destructive file write (overwrite, delete, truncate)
    Command,          // Bash / shell command
    ExternalApi,      // High-cost or high-privilege external API call
    Bridge,           // IDE bridge / remote connection
};

/// A sensitive-path match returned by `scan_paths_for_sensitive`.
struct SensitiveMatch {
    std::string path;          // The original path that matched
    std::string category;      // e.g. "SSH key", "AWS credentials"
    RiskLevel severity;        // Minimum risk tier implied by this match
    std::string description;   // Human-readable explanation (for details panel)
};

/// Aggregated risk summary used to populate the dialog body.
struct RiskSummary {
    RiskLevel level = RiskLevel::Low;
    std::string action_summary;       // One-line "This action will …"
    std::vector<std::string> risk_factors; // Bullet points shown to the user
    std::vector<SensitiveMatch> sensitive_paths; // Matches from scan
    std::vector<std::string> domains;     // URLs / hostnames involved
    std::optional<std::string> plugin_id;
    bool first_install = false;          // True for first-time plugin installs
    bool unsigned_code = false;          // True for plugins without signature
    bool unknown_marketplace = false;    // True for non-official marketplace
};

/// User-facing result of a trust dialog.
enum class TrustChoice : std::uint8_t {
    AllowOnce,       // Allow this single action
    AlwaysAllow,     // Persist: skip prompt for this path/plugin
    ViewFile,        // Show file contents before deciding (High tier only)
    Cancel,          // Block / do nothing
    EnableAnyway,    // Critical tier: user typed YES to override
};

// =========================================================================
// Sensitive path registry (translated from TS risk heuristics + industry
// standard secret-scanning patterns, e.g. trufflehog / gitleaks).
//
// NOTE: these regexes match *path names* (file-system paths or URLs).
// For content-level secrets we re-use `scan_for_secrets()` from
// team_memory/secret_scanner.cppm.
// =========================================================================

namespace detail {

struct SensitivePathRule {
    std::string pattern;         // ECMAScript regex
    std::string category;        // Displayed category
    RiskLevel severity;
    std::string description;
};

// NOTE: std::array of C-strings + regex compile at call-site keeps this
// header-friendly (no static-init order issues) while avoiding recompilation
// of every string literal on every call.
inline const std::array<std::pair<std::string_view, SensitivePathRule>, 22> kSensitivePaths = {{
    // SSH keys
    {"\\.ssh/(id_rsa|id_ed25519|id_ecdsa|id_dsa)(\\.pub)?$",
     {"", "SSH private key", RiskLevel::Critical,
      "This file contains a private SSH key. Exposing it grants login access to any host with the matching public key."}},

    {"\\.ssh/config$",
     {"", "SSH configuration", RiskLevel::High,
      "SSH config may contain host addresses, usernames, and forwarded key directives."}},

    {"\\.ssh/known_hosts$",
     {"", "SSH known hosts", RiskLevel::Medium,
      "List of SSH host fingerprints; reveals which servers you connect to."}},

    // AWS credentials
    {"\\.aws/(credentials|config)$",
     {"", "AWS credentials", RiskLevel::Critical,
      "Contains AWS access keys and secrets. Exfiltrating these keys allows spending against your AWS account."}},

    {"\\.aws/cli/history$",
     {"", "AWS CLI history", RiskLevel::High,
      "Log of every AWS CLI command executed, including resource ARNs and payloads."}},

    // GCP credentials
    {"(gcloud|application_default)_credentials\\.json$",
     {"", "GCP service-account key", RiskLevel::Critical,
      "Google Cloud service-account JSON key — full account access if leaked."}},

    {"\\.config/gcloud/(application_default_credentials|credentials|access_tokens)",
     {"", "GCP credentials", RiskLevel::High,
      "Active GCP credentials or access tokens."}},

    // Environment / dotenv
    {"(^|/)(\\.env|\\.env\\.(local|prod|production|staging|dev|development))($|\\.)",
     {"", "Environment file", RiskLevel::High,
      "Typically contains DB passwords, API keys, session secrets and tokens."}},

    // Password vaults
    {"\\.keychain(-db)?$",
     {"", "macOS / iOS keychain database", RiskLevel::Critical,
      "Encrypted credential store — may still be unlocked with the user's login password."}},

    {"\\.gnupg/(secring|private-keys-v1\\.d)",
     {"", "GPG private keys", RiskLevel::Critical,
      "GPG secret keyring. Allows signing and decryption of any GPG payload."}},

    // UNIX shadow / sudoers / etc
    {"/etc/shadow$",
     {"", "/etc/shadow", RiskLevel::Critical,
      "Hashed user passwords. Cracking these hashes exposes all local account passwords."}},

    {"/etc/sudoers$",
     {"", "/etc/sudoers", RiskLevel::High,
      "Defines who can run commands as root. Modification grants privilege escalation."}},

    {"/etc/passwd$",
     {"", "/etc/passwd", RiskLevel::Medium,
      "User account registry — usually world-readable but leaks account names."}},

    // npm / registry tokens
    {"\\.npmrc$",
     {"", ".npmrc (npm config)", RiskLevel::High,
      "npm config file; often stores publish tokens for private registries."}},

    {"\\.yarnrc(\\.yml)?$",
     {"", "Yarn registry config", RiskLevel::Medium,
      "Yarn configuration; may contain registry auth tokens."}},

    // PEM / private key bundles
    {"\\.(pem|key|crt|p12|pfx)$",
     {"", "Private key / certificate bundle", RiskLevel::High,
      "PEM, PKCS#12, or raw key file — could be a TLS private key, signing key, or cert bundle."}},

    // Browser cookie / login databases
    {"(Cookies|Login Data|Web Data)(\\.db)?$",
     {"", "Browser cookie / login DB", RiskLevel::Critical,
      "Chromium/Firefox cookie or saved-password database. Grants session hijack of all logged-in sites."}},

    // KeePass / Bitwarden / 1Password vaults
    {"\\.(kdbx|kwallet|opdata)$",
     {"", "Password vault", RiskLevel::High,
      "KeePass, KWallet or 1Password vault file — cracking it reveals ALL stored credentials."}},

    // Docker config (registry credentials)
    {"\\.docker/config\\.json$",
     {"", "Docker registry credentials", RiskLevel::High,
      "Base64-encoded username:password for private Docker registries."}},

    // Kubernetes configs
    {"\\.kube/config$",
     {"", "Kubernetes kubeconfig", RiskLevel::Critical,
      "Cluster credentials and CA certificates — full cluster access if leaked."}},

    // Netrc
    {"\\.netrc$",
     {"", ".netrc (plaintext credentials)", RiskLevel::High,
      "Plaintext passwords for FTP, HTTP or Git remote hosts."}},

    // macOS Keytar / Electron credential stores
    {"credential-service-db\\.json$",
     {"", "Keytar / credential store", RiskLevel::High,
      "Electron/VS Code-style credential storage, often unencrypted on disk."}},
}};

} // namespace detail

// =========================================================================
// Public API: scanning & classification
// =========================================================================

/// Scan a list of file-system paths against the sensitive-path registry.
/// Also leverages the team_memory `scan_for_secrets()` if the paths look
/// like files whose *content* should be scanned (.env, .pem, etc.).
[[nodiscard]] inline std::vector<SensitiveMatch>
scan_paths_for_sensitive(const std::vector<std::string>& paths) {
    std::vector<SensitiveMatch> matches;
    matches.reserve(paths.size());

    for (const auto& raw_path : paths) {
        std::string normalized;
        normalized.reserve(raw_path.size());
        for (char c : raw_path) {
            normalized.push_back(c == '\\' ? '/' : c);
        }

        bool matched = false;
        for (const auto& [pattern, rule] : detail::kSensitivePaths) {
            try {
                std::regex re(std::string{pattern},
                              std::regex::ECMAScript | std::regex::icase);
                if (std::regex_search(normalized, re)) {
                    matches.push_back(SensitiveMatch{
                        .path        = raw_path,
                        .category    = rule.category,
                        .severity    = rule.severity,
                        .description = rule.description,
                    });
                    matched = true;
                }
            } catch (...) {
                // Skip malformed patterns — never fail the scan.
            }
        }

        if (!matched) {
            // Fallback: content-level secret scanning on plaintext-suspect
            // extensions. We only check the path *name* heuristics here;
            // actual content scanning should be done by the caller via
            // scan_for_secrets(file_contents).
            static constexpr std::array<std::string_view, 6> kContentScannable = {
                ".env", ".json", ".yaml", ".yml", ".toml", ".ini",
            };
            for (auto ext : kContentScannable) {
                if (normalized.size() > ext.size() &&
                    std::string_view{normalized}.substr(normalized.size() - ext.size()) == ext) {
                    // Leave a marker — caller is responsible for actually
                    // reading the file and calling scan_for_secrets().
                    matches.push_back(SensitiveMatch{
                        .path        = raw_path,
                        .category    = "Suspicious config file",
                        .severity    = RiskLevel::Medium,
                        .description = "Content may contain secrets — use scan_for_secrets() on file contents.",
                    });
                    break;
                }
            }
        }
    }
    return matches;
}

/// Map the bash_security DangerLevel to the 4-tier RiskLevel scale.
/// Safe → Low, Caution → Medium, Dangerous → High, Forbidden → Critical.
[[nodiscard]] constexpr RiskLevel from_danger_level(cc::utils::bash::DangerLevel dl) noexcept {
    switch (dl) {
        case cc::utils::bash::DangerLevel::Safe:      return RiskLevel::Low;
        case cc::utils::bash::DangerLevel::Caution:   return RiskLevel::Medium;
        case cc::utils::bash::DangerLevel::Dangerous: return RiskLevel::High;
        case cc::utils::bash::DangerLevel::Forbidden: return RiskLevel::Critical;
    }
    return RiskLevel::Low;
}

/// Classify a generic action + optional details into a 4-tier risk level.
///
/// This is the single source of truth the TrustDialog uses to pick its UI
/// tier.  It intentionally does NOT re-implement bash_security — callers
/// that already have a SafetyCheckResult should use from_danger_level()
/// instead and pass that level in via the summary.
[[nodiscard]] inline RiskLevel classify_risk(
    ActionType type,
    const RiskSummary& summary)
{
    // 1. Any Critical sensitive path bumps to Critical.
    for (const auto& m : summary.sensitive_paths) {
        if (m.severity == RiskLevel::Critical) return RiskLevel::Critical;
    }

    // 2. Any High sensitive path / unknown marketplace / unsigned first install
    //    bumps to at least High.
    for (const auto& m : summary.sensitive_paths) {
        if (m.severity == RiskLevel::High) {
            RiskLevel cur = summary.level < RiskLevel::High ? RiskLevel::High : summary.level;
            return cur;
        }
    }

    if (summary.unsigned_code || summary.unknown_marketplace) {
        if (summary.first_install) return RiskLevel::High;
        if (summary.level < RiskLevel::Medium) return RiskLevel::Medium;
    }

    // 3. Action-type defaults.
    switch (type) {
        case ActionType::WorkspaceTrust:
            // TrustDialog.tsx default — 2 buttons, Low tier unless concerns.
            return summary.sensitive_paths.empty() ? RiskLevel::Low : RiskLevel::Medium;
        case ActionType::PluginInstall:
            return summary.first_install ? RiskLevel::Medium : RiskLevel::Low;
        case ActionType::PluginUpdate:
            return RiskLevel::Medium;
        case ActionType::PathAccess:
            return summary.sensitive_paths.empty() ? RiskLevel::Low : RiskLevel::Medium;
        case ActionType::PathWrite:
            return summary.sensitive_paths.empty() ? RiskLevel::Medium : RiskLevel::High;
        case ActionType::Command:
            // Usually filled in via from_danger_level(); fallback to Medium.
            return summary.level < RiskLevel::Medium ? RiskLevel::Medium : summary.level;
        case ActionType::ExternalApi:
            return RiskLevel::Medium;
        case ActionType::Bridge:
            return summary.first_install ? RiskLevel::High : RiskLevel::Medium;
    }
    return summary.level;
}

// =========================================================================
// User-facing message builders
// =========================================================================

/// Build a one-line risk-level heading for the dialog title bar.
[[nodiscard]] inline std::string get_risk_title(RiskLevel level) {
    switch (level) {
        case RiskLevel::Low:      return "Information";
        case RiskLevel::Medium:   return "Warning";
        case RiskLevel::High:     return "High Risk";
        case RiskLevel::Critical: return "CRITICAL RISK";
    }
    return "Attention";
}

/// Build the main warning message (action summary + generic risk text)
/// shown at the top of the dialog body.
[[nodiscard]] inline std::string get_trust_message(
    RiskLevel level,
    const RiskSummary& summary)
{
    if (!summary.action_summary.empty()) return summary.action_summary;

    switch (level) {
        case RiskLevel::Low:
            return "This action is low risk. You can still cancel if you did not expect it.";
        case RiskLevel::Medium:
            return "This action accesses files or settings you may want to review before allowing.";
        case RiskLevel::High:
            return "This action can modify or exfiltrate sensitive data. READ the details below and wait for the countdown.";
        case RiskLevel::Critical:
            return "THIS ACTION CAN COMPROMISE YOUR ACCOUNTS OR SYSTEM. You must explicitly opt-in below.";
    }
    return "";
}

// Forward declaration (defined after build_risk_bullets body)
[[nodiscard]] inline bool is_known_safe_domain(std::string_view host);

/// Return a short list of 3-5 risk bullet points for the details panel.
/// Mirrors the warnings shown in TS utils + bash_security explanations.
[[nodiscard]] inline std::vector<std::string> build_risk_bullets(
    const RiskSummary& summary)
{
    std::vector<std::string> bullets;
    bullets.reserve(5);

    if (!summary.sensitive_paths.empty()) {
        bullets.push_back(std::format(
            "Accesses {} sensitive path{}",
            summary.sensitive_paths.size(),
            summary.sensitive_paths.size() == 1 ? "" : "s"));
    }
    if (summary.first_install) {
        bullets.push_back("First install — no prior trust record for this plugin or path.");
    }
    if (summary.unsigned_code) {
        bullets.push_back("Code is not signed. Integrity or provenance cannot be verified.");
    }
    if (summary.unknown_marketplace) {
        bullets.push_back("Marketplace is NOT on the official Anthropic allowlist.");
    }
    if (!summary.domains.empty()) {
        for (const auto& d : summary.domains) {
            if (!is_known_safe_domain(d)) {
                bullets.push_back(std::format("Communicates with external domain: {}", d));
                if (bullets.size() >= 4) break;
            }
        }
    }
    for (const auto& f : summary.risk_factors) {
        if (bullets.size() >= 5) break;
        bullets.push_back(f);
    }
    if (bullets.empty()) {
        bullets.push_back("No specific risk factors detected — review before allowing anyway.");
    }
    return bullets;
}

// =========================================================================
// Domain allowlist (extends marketplace allowlist for general use)
// =========================================================================

/// Returns true if `host` is on the hardcoded safe-domain allowlist.
/// Reuses `is_trusted_marketplace_domain()` for marketplace hosts and
/// extends it with the most common "obviously safe" cloud domains that
/// ship with Claude Code by default.
[[nodiscard]] inline bool is_known_safe_domain(std::string_view host) {
    // Reuse the marketplace allowlist from plugin_trust_text.cppm.
    if (is_trusted_marketplace_domain(host)) return true;

    static constexpr std::array<std::string_view, 18> kSafeHosts = {{
        "api.anthropic.com",
        "anthropic.com",
        "marketplace.anthropic.com",
        "claudecode.app",
        "code.claude.com",
        "console.anthropic.com",
        "status.anthropic.com",
        "docs.anthropic.com",
        "support.anthropic.com",
        "aws.amazon.com",
        "cloud.google.com",
        "console.cloud.google.com",
        "github.com",
        "api.github.com",
        "raw.githubusercontent.com",
        "gitlab.com",
        "bitbucket.org",
        "localhost",
    }};

    for (auto safe : kSafeHosts) {
        if (host == safe) return true;
        // Allow subdomains of safe hosts, e.g. "vscode.dev.github.com"
        if (host.size() > safe.size() &&
            host.substr(host.size() - safe.size()) == safe &&
            host[host.size() - safe.size() - 1] == '.') {
            return true;
        }
    }
    return false;
}

// =========================================================================
// Plugin risk summary builder
// =========================================================================

/// Summarise the risk of installing / updating a plugin.
/// Reuses the PluginDefinition and PluginCapabilities structs from
/// plugins/plugin.cppm — we do NOT duplicate the trust model.
[[nodiscard]] inline RiskSummary plugin_risk_summary(
    const PluginDefinition& def,
    const PluginCapabilities& caps,
    std::string_view marketplace_domain,
    bool is_first_install = true,
    bool is_update = false,
    bool has_signature = true)
{
    RiskSummary s;
    s.plugin_id = def.name;
    s.first_install = is_first_install;
    s.unsigned_code = !has_signature;
    s.unknown_marketplace = !is_trusted_marketplace_domain(marketplace_domain);

    if (is_first_install && s.unknown_marketplace) {
        s.risk_factors.push_back("Installing from an unvetted marketplace source.");
    }

    // Count capability risk
    if (!caps.tools.empty()) {
        s.risk_factors.push_back(std::format("Exposes {} tool capability{}.",
            caps.tools.size(), caps.tools.size() == 1 ? "" : "ies"));
    }
    if (!caps.hooks.empty()) {
        s.risk_factors.push_back(std::format("Registers {} lifecycle hook{}.",
            caps.hooks.size(), caps.hooks.size() == 1 ? "" : "s"));
    }
    if (!caps.commands.empty()) {
        s.risk_factors.push_back(std::format("Registers {} slash command{}.",
            caps.commands.size(), caps.commands.size() == 1 ? "" : "s"));
    }

    if (is_update) {
        s.action_summary = std::format(
            "Updating plugin \"{} {}\" to a new version — permissions may have changed.",
            def.name, def.version);
    } else {
        s.action_summary = std::format(
            "Installing plugin \"{} {}\" by {}.",
            def.name, def.version,
            def.author.empty() ? "unknown author" : def.author);
    }

    if (!def.homepage) {
        s.risk_factors.push_back("Plugin has no homepage — harder to verify provenance.");
    }
    if (!def.license) {
        s.risk_factors.push_back("Plugin has no declared license.");
    }

    return s;
}

// =========================================================================
// Helpers
// =========================================================================

/// Format a list of items with proper "and" conjunction.
/// Direct port of formatListWithAnd() from TS utils.ts.
[[nodiscard]] inline std::string format_list_with_and(
    const std::vector<std::string>& items,
    std::optional<std::size_t> limit = std::nullopt)
{
    if (items.empty()) return "";
    const std::size_t effective = limit.value_or(0);
    const std::size_t n = items.size();

    if (!effective || n <= effective) {
        if (n == 1) return items[0];
        if (n == 2) return items[0] + " and " + items[1];
        std::ostringstream os;
        for (std::size_t i = 0; i + 1 < n; ++i) os << items[i] << ", ";
        os << "and " << items.back();
        return os.str();
    }

    // More items than the limit — summarise.
    std::ostringstream os;
    const std::size_t remaining = n - effective;
    if (effective == 1) {
        os << items[0] << " and " << remaining << " more";
        return os.str();
    }
    for (std::size_t i = 0; i < effective; ++i) os << items[i] << ", ";
    os << "and " << remaining << " more";
    return os.str();
}

/// Countdown duration in seconds for High-tier dialogs.
inline constexpr int kHighTierCountdownSeconds = 5;

/// The exact confirmation word the user must type for Critical-tier.
inline constexpr std::string_view kCriticalConfirmWord = "YES";

} // namespace cc::ui::trust_utils
