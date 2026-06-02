module;
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
export module cc.services.plugins.installation_manager;

export namespace cc::services::plugins {

// Plugin installation and lifecycle manager
class PluginInstallationManager {
public:
    // Install a plugin by ID
    auto install(std::string_view plugin_id) -> std::expected<void, std::string> {
        std::lock_guard lock(mutex_);
        if (is_installed_impl(plugin_id)) {
            return std::unexpected("Plugin already installed: " + std::string(plugin_id));
        }
        // Track installation locally when no plugin marketplace transport is configured.
        installed_.emplace_back(plugin_id);
        return {};
    }

    // Uninstall a plugin by ID
    auto uninstall(std::string_view plugin_id) -> std::expected<void, std::string> {
        std::lock_guard lock(mutex_);
        if (!is_installed_impl(plugin_id)) {
            return std::unexpected("Plugin not installed: " + std::string(plugin_id));
        }
        std::erase_if(installed_, [&](const auto& id) { return id == plugin_id; });
        return {};
    }

    // Update a plugin to latest version
    auto update(std::string_view plugin_id) -> std::expected<void, std::string> {
        std::lock_guard lock(mutex_);
        if (!is_installed_impl(plugin_id)) {
            return std::unexpected("Plugin not installed: " + std::string(plugin_id));
        }
        // Local installation state has no versioned update source.
        return {};
    }

    // Get all installed plugin IDs
    auto get_installed() -> std::vector<std::string> {
        std::lock_guard lock(mutex_);
        return installed_;
    }

    // Check if a plugin is installed
    auto is_installed(std::string_view plugin_id) -> bool {
        std::lock_guard lock(mutex_);
        return is_installed_impl(plugin_id);
    }

private:
    auto is_installed_impl(std::string_view plugin_id) const -> bool {
        for (const auto& id : installed_) {
            if (id == plugin_id) return true;
        }
        return false;
    }

    std::mutex mutex_;
    std::vector<std::string> installed_;
};

} // namespace cc::services::plugins
