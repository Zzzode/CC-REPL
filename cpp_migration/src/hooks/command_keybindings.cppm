module;
#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.command_keybindings;

export namespace cc::hooks::command_keybindings {

// 按键绑定：将快捷键映射到命令
struct KeyBinding {
    std::string key_combo;
    std::string command;
    std::optional<std::string> when_clause;
    int priority{0};
};

// 按键匹配结果
struct KeyBindingMatch {
    std::string command;
    std::vector<std::string> args;
};

namespace detail {

struct KeyBindingRegistry {
    std::mutex mutex;
    std::vector<KeyBinding> bindings;
};

inline auto get_registry() -> KeyBindingRegistry& {
    static KeyBindingRegistry registry;
    return registry;
}

/// Default keybindings loaded on reset
inline auto get_default_bindings() -> std::vector<KeyBinding> {
    return {
        {"ctrl+c", "interrupt", std::nullopt, 100},
        {"ctrl+d", "exit", std::nullopt, 100},
        {"ctrl+l", "clear", std::nullopt, 50},
        {"ctrl+r", "history_search", std::nullopt, 50},
        {"tab", "complete", std::nullopt, 50},
    };
}

} // namespace detail

// 注册一个命令快捷键绑定
inline void register_command_keybinding(KeyBinding binding) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    // Replace existing binding for the same key_combo, or add new
    for (auto& existing : reg.bindings) {
        if (existing.key_combo == binding.key_combo) {
            existing = std::move(binding);
            return;
        }
    }
    reg.bindings.push_back(std::move(binding));
}

// 注销指定快捷键组合的绑定
inline void unregister_keybinding(std::string_view key_combo) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    std::erase_if(reg.bindings, [key_combo](const KeyBinding& b) {
        return b.key_combo == key_combo;
    });
}

// 匹配输入的按键组合，返回对应的命令（如果匹配）
inline std::optional<KeyBindingMatch> match_keybinding(std::string_view input) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);

    const KeyBinding* best = nullptr;
    for (const auto& binding : reg.bindings) {
        if (binding.key_combo == input) {
            if (!best || binding.priority > best->priority) {
                best = &binding;
            }
        }
    }

    if (best) {
        return KeyBindingMatch{.command = best->command, .args = {}};
    }
    return std::nullopt;
}

// 获取所有已注册的快捷键绑定
inline std::vector<KeyBinding> get_all_keybindings() {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    return reg.bindings;
}

// 重置所有快捷键绑定到默认状态
inline void reset_keybindings() {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    reg.bindings = detail::get_default_bindings();
}

} // namespace cc::hooks::command_keybindings
