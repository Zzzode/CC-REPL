/// @file resolver.cppm
/// @brief Keybinding resolver - matches key events to commands.
/// Migrated from src/keybindings/resolver.ts, validate.ts, loadUserBindings.ts
module;

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <functional>
#include <filesystem>
#include <fstream>

export module cc.keybindings.resolver;

import cc.keybindings.schema;
import cc.keybindings.defaults;

export namespace cc::keybindings {

/// Validation error for a user keybinding
struct ValidationError {
    std::string binding_id;
    std::string message;
};

/// Validate a set of user keybindings
[[nodiscard]] inline std::vector<ValidationError> validate_bindings(
    const std::vector<Keybinding>& bindings
) {
    std::vector<ValidationError> errors;
    
    for (const auto& binding : bindings) {
        if (binding.keys.empty()) {
            errors.push_back({binding.id, "Keybinding must have at least one key chord"});
            continue;
        }
        
        // Check for reserved shortcuts
        for (const auto& chord : binding.keys) {
            std::string repr = chord.key;
            if (chord.modifiers.ctrl) repr = "ctrl+" + repr;
            if (chord.modifiers.alt) repr = "alt+" + repr;
            if (chord.modifiers.shift) repr = "shift+" + repr;
            if (chord.modifiers.meta) repr = "meta+" + repr;
            
            if (is_reserved(repr)) {
                errors.push_back({binding.id, "Cannot override reserved shortcut: " + repr});
            }
        }
    }
    
    return errors;
}

/// Keybinding resolver - merges default and user bindings, resolves conflicts
class KeybindingResolver {
    std::vector<Keybinding> bindings_;
    std::unordered_map<std::string, std::size_t> id_index_;

public:
    KeybindingResolver() {
        // Start with defaults
        bindings_ = get_default_bindings();
        rebuild_index();
    }
    
    /// Load and merge user bindings (from keybindings.json)
    void load_user_bindings(const std::vector<Keybinding>& user_bindings) {
        for (const auto& binding : user_bindings) {
            if (auto it = id_index_.find(binding.id); it != id_index_.end()) {
                // Override existing binding
                bindings_[it->second] = binding;
            } else {
                // Add new binding
                bindings_.push_back(binding);
            }
        }
        rebuild_index();
    }
    
    /// Resolve a key event to a command
    [[nodiscard]] std::optional<std::string> resolve(const KeyChord& event) const {
        for (const auto& binding : bindings_) {
            if (!binding.keys.empty() && matches_chord(binding.keys[0], event)) {
                return binding.command;
            }
        }
        return std::nullopt;
    }
    
    /// Get all bindings
    [[nodiscard]] const std::vector<Keybinding>& all_bindings() const { return bindings_; }

private:
    void rebuild_index() {
        id_index_.clear();
        for (std::size_t i = 0; i < bindings_.size(); ++i) {
            id_index_[bindings_[i].id] = i;
        }
    }
};

} // namespace cc::keybindings
