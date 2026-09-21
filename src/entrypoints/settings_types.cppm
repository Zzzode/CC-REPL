/// @file settings_types.cppm
/// @brief Generated settings types for the SDK configuration system.
/// Migrated from src/entrypoints/sdk/settingsTypes.generated.ts
///
/// Provides the Settings type representing merged configuration.
/// In the TypeScript source this is generated from Zod schemas;
/// here we define the core structure directly.
module;

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <unordered_map>

export module cc.entrypoints.settings_types;

import cc.entrypoints.sandbox_types;

export namespace cc::entrypoints::settings {

// ============================================================================
// Settings Type
// ============================================================================

/// Represents the merged/effective settings object.
/// In TypeScript this is `Record<string, unknown>` (a loose bag).
/// In C++ we define known fields explicitly for type safety,
/// with an escape hatch for extension fields.
struct Settings {
    // Model configuration
    std::optional<std::string> model;
    std::optional<std::string> small_fast_model;

    // Permission mode
    std::optional<std::string> permission_mode;  // matches PermissionMode values

    // API configuration
    std::optional<std::string> api_key;
    std::optional<std::string> api_base_url;
    std::optional<std::string> api_provider;

    // Sandbox configuration
    std::optional<sandbox::SandboxSettings> sandbox;

    // Custom instructions
    std::optional<std::string> system_prompt;
    std::optional<std::string> append_system_prompt;

    // Tool permissions (allow/deny rules)
    std::optional<std::vector<std::string>> allowed_tools;
    std::optional<std::vector<std::string>> disallowed_tools;

    // MCP server configuration
    std::optional<std::unordered_map<std::string, std::string>> mcp_servers;

    // Agent definitions
    std::optional<std::unordered_map<std::string, std::string>> agents;

    // Hook configuration
    std::optional<std::unordered_map<std::string, std::string>> hooks;

    // Extension fields (for forward compatibility)
    std::unordered_map<std::string, std::string> extra_fields;

    /// Check if settings is empty (no fields set)
    [[nodiscard]] bool empty() const {
        return !model && !small_fast_model && !permission_mode &&
               !api_key && !api_base_url && !api_provider &&
               !sandbox && !system_prompt && !append_system_prompt &&
               !allowed_tools && !disallowed_tools &&
               !mcp_servers && !agents && !hooks &&
               extra_fields.empty();
    }
};

// ============================================================================
// Settings Sources
// ============================================================================

/// Source of a settings layer
enum class SettingsSource {
    UserSettings,
    ProjectSettings,
    LocalSettings,
    FlagSettings,
    PolicySettings,
};

/// A settings layer from a specific source
struct SettingsLayer {
    SettingsSource source;
    Settings settings;
};

/// Merged settings result with provenance tracking
struct MergedSettings {
    Settings effective;
    std::vector<SettingsLayer> sources;
};

} // namespace cc::entrypoints::settings
