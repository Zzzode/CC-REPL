module;
#include <algorithm>
#include <expected>
#include <string>
#include <string_view>
#include <vector>
export module cc.services.mcp.official_registry;

export namespace cc::services::mcp {

// Registry entry for an official MCP server
struct RegistryEntry {
    std::string name;
    std::string description;
    std::string url;
    std::string version;
};

// Fetch the full official registry
auto fetch_registry() -> std::expected<std::vector<RegistryEntry>, std::string> {
    // Registry transport is not configured; return an empty registry.
    return std::vector<RegistryEntry>{};
}

// Search the registry by query string
auto search_registry(std::string_view query) -> std::vector<RegistryEntry> {
    auto result = fetch_registry();
    if (!result) {
        return {};
    }
    std::vector<RegistryEntry> matches;
    for (const auto& entry : *result) {
        // Simple substring match on name and description
        if (entry.name.find(query) != std::string::npos ||
            entry.description.find(query) != std::string::npos) {
            matches.push_back(entry);
        }
    }
    return matches;
}

// Check if a server name matches an official registry entry
auto is_official_server(std::string_view name) -> bool {
    auto result = fetch_registry();
    if (!result) {
        return false;
    }
    return std::ranges::any_of(*result, [&](const auto& entry) {
        return entry.name == name;
    });
}

} // namespace cc::services::mcp
