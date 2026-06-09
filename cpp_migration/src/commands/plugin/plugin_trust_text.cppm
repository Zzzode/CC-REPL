/// @file plugin_trust_text.cppm
/// @brief Trust-warning text builder for plugin installation screens.
///
/// Extracted from src/commands/plugin/PluginTrustWarning.tsx.
/// The React Box/Text JSX rendering is deferred to Phase 4 (FTXUI).
/// This module provides the pure text assembly used by both the warning
/// banner and the headless CLI installation path.

module;

#include <string>
#include <string_view>
#include <optional>
#include <format>

export module cc.commands.plugin_trust_text;

export namespace cc::commands::plugin {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Core trust disclaimer shown before any install / update / enable action.
/// Mirrors the static body of TS: PluginTrustWarning (the italic Text block).
constexpr std::string_view kTrustDisclaimerBody =
    "Make sure you trust a plugin before installing, updating, or using it. "
    "Anthropic does not control what MCP servers, files, or other software "
    "are included in plugins and cannot verify that they will work as "
    "intended or that they won't change. See each plugin's homepage for "
    "more information.";

/// Short prefix appended to the disclaimer when the marketplace supplies
/// additional context (e.g. enterprise policy messaging).
constexpr std::string_view kTrustDisclaimerPrefix = " ";

// ---------------------------------------------------------------------------
// Pure text builders
// ---------------------------------------------------------------------------

/// Build the complete trust-warning text without any styling.
/// If `custom_message` is provided by marketplace config, it is appended.
/// Mirrors the assembly inside the JSX in PluginTrustWarning.tsx:
///   disclaimter-body + (customMessage ? ` ${customMessage}` : "")
[[nodiscard]] inline std::string build_trust_warning_text(
    std::optional<std::string_view> custom_message = std::nullopt)
{
    std::string text{kTrustDisclaimerBody};
    if (custom_message.has_value() && !custom_message->empty()) {
        text.reserve(text.size() + 1 + custom_message->size());
        text.append(kTrustDisclaimerPrefix);
        text.append(*custom_message);
    }
    return text;
}

/// Build the one-line "header" text that precedes the disclaimer.
/// Used by CLI output (Phase 2) and as an accessible label for the warning
/// icon in FTXUI (Phase 4).
[[nodiscard]] inline std::string_view trust_warning_header() noexcept
{
    return "Plugin trust reminder";
}

/// Returns true when a given marketplace domain is on the hardcoded
/// trust-allowlist. Anything not on the list will show an extra
/// "unofficial source" line. Exposed here because PluginTrustWarning.tsx
/// imports `getPluginTrustMessage()` from marketplaceHelpers.ts, which
/// ultimately decides whether to add custom text.
[[nodiscard]] constexpr bool is_trusted_marketplace_domain(std::string_view domain) noexcept
{
    // Anthropic / Claude official domains.
    if (domain == "marketplace.anthropic.com") return true;
    if (domain == "api.anthropic.com")         return true;
    if (domain == "claudecode.app")            return true;
    if (domain == "anthropic.com")             return true;
    return false;
}

/// Build a short "source" line shown under the disclaimer for untrusted
/// marketplace domains. Returns empty string if the domain is trusted.
[[nodiscard]] inline std::optional<std::string> build_marketplace_source_note(
    std::string_view marketplace_name,
    std::string_view marketplace_domain)
{
    if (is_trusted_marketplace_domain(marketplace_domain)) {
        return std::nullopt;
    }
    return std::format("This plugin is sourced from \"{}\" ({}), which is not an official Anthropic marketplace.",
                       marketplace_name,
                       marketplace_domain.empty() ? std::string_view{"unknown domain"} : marketplace_domain);
}

} // namespace cc::commands::plugin
