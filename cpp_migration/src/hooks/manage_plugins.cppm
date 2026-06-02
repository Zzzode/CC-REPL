module;
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

export module cc.hooks.manage_plugins;

export namespace cc::hooks {

enum class PluginState {
  disabled,
  enabled,
  error
};

struct PluginInfo {
  std::string id;
  std::string name;
  std::string version;
  std::string description;
  PluginState state = PluginState::disabled;
  std::string error_message;
};

using PluginChangeCallback = std::function<void(std::string_view, PluginState)>;

class ManagePluginsHook {
public:
  ManagePluginsHook() = default;

  auto load_plugins() -> void {
    needs_refresh_ = false;
  }

  auto enable_plugin(std::string_view id) -> bool {
    if (auto it = plugins_.find(std::string(id)); it != plugins_.end()) {
      it->second.state = PluginState::enabled;
      if (change_callback_) {
        change_callback_(id, PluginState::enabled);
      }
      return true;
    }
    return false;
  }

  auto disable_plugin(std::string_view id) -> bool {
    if (auto it = plugins_.find(std::string(id)); it != plugins_.end()) {
      it->second.state = PluginState::disabled;
      if (change_callback_) {
        change_callback_(id, PluginState::disabled);
      }
      return true;
    }
    return false;
  }

  auto get_plugins() const -> std::vector<PluginInfo> {
    std::vector<PluginInfo> result;
    result.reserve(plugins_.size());
    for (const auto& [id, info] : plugins_) {
      result.push_back(info);
    }
    return result;
  }

  auto get_enabled_plugins() const -> std::vector<PluginInfo> {
    std::vector<PluginInfo> result;
    for (const auto& [id, info] : plugins_) {
      if (info.state == PluginState::enabled) {
        result.push_back(info);
      }
    }
    return result;
  }

  auto needs_refresh() const -> bool {
    return needs_refresh_;
  }

  auto set_needs_refresh(bool needs) -> void {
    needs_refresh_ = needs;
  }

  auto on_plugin_change(PluginChangeCallback callback) -> void {
    change_callback_ = std::move(callback);
  }

  auto add_plugin(PluginInfo plugin) -> void {
    plugins_[plugin.id] = std::move(plugin);
  }

  auto remove_plugin(std::string_view id) -> void {
    plugins_.erase(std::string(id));
  }

private:
  std::unordered_map<std::string, PluginInfo> plugins_;
  bool needs_refresh_ = false;
  PluginChangeCallback change_callback_;
};

}
