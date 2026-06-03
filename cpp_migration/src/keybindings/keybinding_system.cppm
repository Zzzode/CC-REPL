module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.keybindings.keybinding_system;


export namespace cc::core {


struct KeyModifiers {
    bool ctrl{false};
    bool alt{false};
    bool shift{false};
    bool meta{false};   // Cmd on macOS
    
    auto operator<=>(const KeyModifiers&) const = default;
};


struct KeyCombo {
    std::string key;       // "a", "Enter", "Tab", "F1", "Up", etc.
    KeyModifiers modifiers;
    
    auto operator<=>(const KeyCombo&) const = default;
};


enum class KeybindingContext { 
    global, insert, normal, visual, command, search, dialog 
};


struct KeybindingAction {
    std::string action_id;
    std::string description;
    KeyCombo default_combo;
    KeybindingContext context{KeybindingContext::global};
};


struct KeybindingEntry {
    KeyCombo combo;
    std::string action_id;
    KeybindingContext context;
    enum class Source { builtin, user, plugin } source{Source::builtin};
};


struct KeybindingConflict {
    KeyCombo combo;
    std::string existing_action;
    std::string new_action;
    KeybindingContext context;
};


class KeybindingSystem {
    std::vector<KeybindingEntry> bindings_;
    std::vector<KeybindingAction> actions_;
    std::vector<KeyCombo> reserved_;

public:
    KeybindingSystem() { init_defaults(); }


    [[nodiscard]] static auto parse_combo(std::string_view text) -> std::expected<KeyCombo, std::string> {
        KeyCombo result;
        std::string remaining(text);
        

        auto consume = [&](std::string_view prefix, bool& flag) {
            if (remaining.starts_with(prefix)) {
                flag = true;
                remaining = remaining.substr(prefix.size());
                return true;
            }
            return false;
        };
        
        while (true) {
            if (consume("Ctrl+", result.modifiers.ctrl)) continue;
            if (consume("Alt+", result.modifiers.alt)) continue;
            if (consume("Shift+", result.modifiers.shift)) continue;
            if (consume("Meta+", result.modifiers.meta)) continue;
            if (consume("Cmd+", result.modifiers.meta)) continue;
            break;
        }
        
        if (remaining.empty())
            return std::unexpected("缺少按键名称");
        
        result.key = remaining;
        return result;
    }


    [[nodiscard]] auto resolve(const KeyCombo& combo, KeybindingContext context) const 
        -> std::optional<std::string> {

        for (const auto& entry : bindings_) {
            if (entry.combo == combo && entry.context == context)
                return entry.action_id;
        }

        if (context != KeybindingContext::global) {
            for (const auto& entry : bindings_) {
                if (entry.combo == combo && entry.context == KeybindingContext::global)
                    return entry.action_id;
            }
        }
        return std::nullopt;
    }


    void bind(KeyCombo combo, std::string action_id, KeybindingContext context, 
              KeybindingEntry::Source source = KeybindingEntry::Source::user) {
        bindings_.push_back({.combo = std::move(combo), .action_id = std::move(action_id),
                            .context = context, .source = source});
    }

    void unbind(const KeyCombo& combo, KeybindingContext context) {
        std::erase_if(bindings_, [&](const auto& e) {
            return e.combo == combo && e.context == context;
        });
    }


    [[nodiscard]] auto get_bindings_for_action(std::string_view action_id) const 
        -> std::vector<KeybindingEntry> {
        std::vector<KeybindingEntry> result;
        for (const auto& e : bindings_) {
            if (e.action_id == action_id) result.push_back(e);
        }
        return result;
    }


    [[nodiscard]] auto load_user_bindings(std::string_view path) -> std::expected<void, std::string> {
        if (path.empty()) return std::unexpected("Keybinding config path is empty");
        return {};
    }


    [[nodiscard]] auto get_conflicts() const -> std::vector<KeybindingConflict> {
        std::vector<KeybindingConflict> conflicts;
        for (size_t i = 0; i < bindings_.size(); ++i) {
            for (size_t j = i + 1; j < bindings_.size(); ++j) {
                if (bindings_[i].combo == bindings_[j].combo &&
                    bindings_[i].context == bindings_[j].context) {
                    conflicts.push_back({
                        .combo = bindings_[i].combo,
                        .existing_action = bindings_[i].action_id,
                        .new_action = bindings_[j].action_id,
                        .context = bindings_[i].context
                    });
                }
            }
        }
        return conflicts;
    }


    [[nodiscard]] static auto format_combo(const KeyCombo& combo, bool is_macos = true) -> std::string {
        std::string result;
        if (combo.modifiers.ctrl) result += is_macos ? "⌃" : "Ctrl+";
        if (combo.modifiers.alt) result += is_macos ? "⌥" : "Alt+";
        if (combo.modifiers.shift) result += is_macos ? "⇧" : "Shift+";
        if (combo.modifiers.meta) result += is_macos ? "⌘" : "Meta+";
        result += combo.key;
        return result;
    }


    [[nodiscard]] auto is_reserved(const KeyCombo& combo) const -> bool {
        return std::any_of(reserved_.begin(), reserved_.end(),
            [&](const auto& r) { return r == combo; });
    }

    [[nodiscard]] auto get_all_actions() const -> const std::vector<KeybindingAction>& { return actions_; }

    void reset_to_defaults() {
        bindings_.clear();
        init_defaults();
    }

private:
    void init_defaults() {

        reserved_ = {
            {.key = "c", .modifiers = {.ctrl = true}},
            {.key = "z", .modifiers = {.ctrl = true}},
        };


        actions_ = {
            {"submit", "提交输入", {.key = "Enter"}, KeybindingContext::insert},
            {"newline", "插入换行", {.key = "Enter", .modifiers = {.shift = true}}, KeybindingContext::insert},
            {"cancel", "取消当前操作", {.key = "c", .modifiers = {.ctrl = true}}, KeybindingContext::global},
            {"clear", "清屏", {.key = "l", .modifiers = {.ctrl = true}}, KeybindingContext::global},
            {"search", "搜索", {.key = "r", .modifiers = {.ctrl = true}}, KeybindingContext::insert},
            {"compact", "压缩对话", {.key = "k", .modifiers = {.ctrl = true, .shift = true}}, KeybindingContext::global},
            {"copy", "复制选中", {.key = "c", .modifiers = {.meta = true}}, KeybindingContext::global},
            {"paste", "粘贴", {.key = "v", .modifiers = {.meta = true}}, KeybindingContext::insert},
        };

        for (const auto& action : actions_) {
            bindings_.push_back({
                .combo = action.default_combo, .action_id = action.action_id,
                .context = action.context, .source = KeybindingEntry::Source::builtin
            });
        }
    }
};

} // namespace cc::core
