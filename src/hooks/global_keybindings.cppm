module;
#include <string>
#include <functional>
#include <optional>
#include <vector>
#include <unordered_map>
#include <set>

export module cc.hooks.global_keybindings;

import cc.state.app_state;

export namespace cc::hooks::global_keybindings {

enum class KeyContext { global, input, chat, diff_view, settings };

struct KeyCombo {
    std::string key;
    bool ctrl{false};
    bool alt{false};
    bool shift{false};
    bool meta{false};
};

struct KeybindingEntry {
    std::string id;
    std::string description;
    KeyCombo combo;
    KeyContext context{KeyContext::global};
    bool enabled{true};
};

struct GlobalKeybindingsState {
    std::vector<KeybindingEntry> bindings;
    KeyContext active_context{KeyContext::global};
    std::optional<std::string> last_triggered;
};

struct GlobalKeybindingsOptions {
    bool enable_vim_mode{false};
};

class GlobalKeybindingsHook {
public:
    explicit GlobalKeybindingsHook(const GlobalKeybindingsOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const GlobalKeybindingsState& state() const { return state_; }

    void register_binding(KeybindingEntry entry) {
        actions_[entry.id] = nullptr;
        state_.bindings.push_back(std::move(entry));
        notify();
    }

    void set_action(std::string_view id, std::function<void()> action) {
        actions_[std::string(id)] = std::move(action);
    }

    void unregister(std::string_view id) {
        std::erase_if(state_.bindings, [&](const auto& b) { return b.id == id; });
        actions_.erase(std::string(id));
        notify();
    }

    void set_context(KeyContext ctx) {
        state_.active_context = ctx;
        notify();
    }

    /// Handle a key event. Returns true if a binding matched.
    bool handle_key(const KeyCombo& combo) {
        for (const auto& binding : state_.bindings) {
            if (!binding.enabled) continue;
            if (binding.context != KeyContext::global &&
                binding.context != state_.active_context) continue;
            if (matches(binding.combo, combo)) {
                state_.last_triggered = binding.id;
                if (auto it = actions_.find(binding.id); it != actions_.end() && it->second) {
                    it->second();
                }
                notify();
                return true;
            }
        }
        return false;
    }

    void on_change(std::function<void(const GlobalKeybindingsState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    static bool matches(const KeyCombo& a, const KeyCombo& b) {
        return a.key == b.key && a.ctrl == b.ctrl &&
               a.alt == b.alt && a.shift == b.shift && a.meta == b.meta;
    }

    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    GlobalKeybindingsState state_;
    GlobalKeybindingsOptions options_;
    std::unordered_map<std::string, std::function<void()>> actions_;
    std::vector<std::function<void(const GlobalKeybindingsState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::global_keybindings
