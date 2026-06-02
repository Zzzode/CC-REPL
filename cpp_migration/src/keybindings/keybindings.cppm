// C++23 Module: Keybindings management
// Provides keybinding parsing, matching, and resolution
module;
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module cc.keybindings.keybindings;

export namespace cc::core::keybindings {

// ============================================================
// Keybinding Types
// ============================================================
enum class Modifier : uint8_t {
    None = 0,
    Ctrl = 1 << 0,
    Alt = 1 << 1,
    Shift = 1 << 2,
    Meta = 1 << 3,
    Cmd = 1 << 4, // Alias for Meta on macOS
};

constexpr Modifier operator|(Modifier a, Modifier b) {
    return static_cast<Modifier>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr Modifier operator&(Modifier a, Modifier b) {
    return static_cast<Modifier>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

constexpr bool has_modifier(Modifier modifiers, Modifier check) {
    return (modifiers & check) != Modifier::None;
}

struct KeyEvent {
    std::string key;
    Modifier modifiers = Modifier::None;
    bool is_repeat = false;
};

// ============================================================
// Keybinding Representation
// ============================================================
struct Keybinding {
    std::string key_sequence;
    Modifier modifiers = Modifier::None;
    std::string command;
    std::optional<std::string> description;
    std::optional<std::string> category;
    bool is_reserved = false;
};

// ============================================================
// Keybinding Parser
// ============================================================
class KeybindingParser {
public:
    static std::optional<Keybinding> parse(std::string_view binding_str) {
        Keybinding binding;
        binding.modifiers = Modifier::None;
        
        std::string remaining(binding_str);
        
        // Parse modifiers
        while (true) {
            size_t plus_pos = remaining.find('+');
            if (plus_pos == std::string::npos) break;
            
            std::string part = remaining.substr(0, plus_pos);
            remaining = remaining.substr(plus_pos + 1);
            
            if (part.empty()) continue;
            
            // Normalize modifier name
            std::string lower_part;
            lower_part.reserve(part.size());
            for (char c : part) {
                lower_part += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            
            if (lower_part == "ctrl" || lower_part == "control") {
                binding.modifiers = binding.modifiers | Modifier::Ctrl;
            } else if (lower_part == "alt" || lower_part == "option") {
                binding.modifiers = binding.modifiers | Modifier::Alt;
            } else if (lower_part == "shift") {
                binding.modifiers = binding.modifiers | Modifier::Shift;
            } else if (lower_part == "meta" || lower_part == "cmd" || lower_part == "command") {
                binding.modifiers = binding.modifiers | Modifier::Meta;
            } else {
                // Not a modifier, put back
                remaining = part + "+" + remaining;
                break;
            }
        }
        
        // Parse key
        if (remaining.empty()) {
            return std::nullopt;
        }
        
        binding.key_sequence = normalize_key(remaining);
        binding.key_sequence = remaining;
        
        return binding;
    }
    
private:
    static std::string normalize_key(std::string_view key) {
        std::string result;
        result.reserve(key.size());
        for (char c : key) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return result;
    }
};

// ============================================================
// Keybinding Matcher
// ============================================================
class KeybindingMatcher {
public:
    static bool matches(const Keybinding& binding, const KeyEvent& event) {
        if (normalize_key(binding.key_sequence) != normalize_key(event.key)) {
            return false;
        }
        return modifiers_match(binding.modifiers, event.modifiers);
    }
    
    static bool modifiers_match(Modifier binding_mods, Modifier event_mods) {
        // Check that all required modifiers are present
        if ((binding_mods & Modifier::Ctrl) != Modifier::None &&
            (event_mods & Modifier::Ctrl) == Modifier::None) {
            return false;
        }
        if ((binding_mods & Modifier::Alt) != Modifier::None &&
            (event_mods & Modifier::Alt) == Modifier::None) {
            return false;
        }
        if ((binding_mods & Modifier::Shift) != Modifier::None &&
            (event_mods & Modifier::Shift) == Modifier::None) {
            return false;
        }
        if ((binding_mods & Modifier::Meta) != Modifier::None &&
            (event_mods & Modifier::Meta) == Modifier::None) {
            return false;
        }
        
        return true;
    }
    
private:
    static std::string normalize_key(std::string_view key) {
        std::string result;
        result.reserve(key.size());
        for (char c : key) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return result;
    }
};

// ============================================================
// Keybinding Resolver
// ============================================================
class KeybindingResolver {
public:
    void add_binding(Keybinding binding) {
        m_bindings.push_back(std::move(binding));
        m_rebuild_index();
    }
    
    void remove_binding(std::string_view key_sequence) {
        std::erase_if(m_bindings, [&](const auto& b) {
            return b.key_sequence == key_sequence;
        });
        m_rebuild_index();
    }
    
    std::optional<Keybinding> resolve(const KeyEvent& event) const {
        // Look up by key first
        auto it = m_key_index.find(normalize_key(event.key));
        if (it == m_key_index.end()) {
            return std::nullopt;
        }
        
        // Check each candidate
        for (const auto* binding : it->second) {
            if (KeybindingMatcher::matches(*binding, event)) {
                return *binding;
            }
        }
        
        return std::nullopt;
    }
    
    std::vector<Keybinding> get_all() const {
        return m_bindings;
    }
    
    std::vector<Keybinding> get_by_category(std::string_view category) const {
        std::vector<Keybinding> result;
        for (const auto& b : m_bindings) {
            if (b.category == category) {
                result.push_back(b);
            }
        }
        return result;
    }
    
    void load_default_bindings() {
        // Navigation
        add_binding({"ctrl+c", Modifier::Ctrl, "cancel", "Cancel current operation", "Navigation"});
        add_binding({"ctrl+d", Modifier::Ctrl, "exit", "Exit application", "Navigation"});
        add_binding({"ctrl+l", Modifier::Ctrl, "clear_screen", "Clear the screen", "Navigation"});
        
        // Editing
        add_binding({"ctrl+a", Modifier::Ctrl, "move_to_start", "Move cursor to start", "Editing"});
        add_binding({"ctrl+e", Modifier::Ctrl, "move_to_end", "Move cursor to end", "Editing"});
        add_binding({"ctrl+w", Modifier::Ctrl, "delete_word", "Delete word", "Editing"});
        add_binding({"ctrl+u", Modifier::Ctrl, "delete_line", "Delete line", "Editing"});
        
        // History
        add_binding({"ctrl+p", Modifier::Ctrl, "history_previous", "Previous history entry", "History"});
        add_binding({"ctrl+n", Modifier::Ctrl, "history_next", "Next history entry", "History"});
        add_binding({"up", Modifier::None, "history_previous", "Previous history entry", "History", true});
        add_binding({"down", Modifier::None, "history_next", "Next history entry", "History", true});
        
        // Completion
        add_binding({"tab", Modifier::None, "complete", "Show completions", "Completion", true});
        add_binding({"shift+tab", Modifier::Shift, "complete_previous", "Previous completion", "Completion"});
        
        // Search
        add_binding({"ctrl+r", Modifier::Ctrl, "search_history", "Search history", "Search"});
        add_binding({"ctrl+s", Modifier::Ctrl, "search_history_forward", "Search history forward", "Search"});
    }
    
private:
    void m_rebuild_index() {
        m_key_index.clear();
        for (const auto& b : m_bindings) {
            m_key_index[normalize_key(b.key_sequence)].push_back(&b);
        }
    }
    
    static std::string normalize_key(std::string_view key) {
        std::string result;
        result.reserve(key.size());
        for (char c : key) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return result;
    }
    
    std::vector<Keybinding> m_bindings;
    std::unordered_map<std::string, std::vector<const Keybinding*>> m_key_index;
};

// ============================================================
// Keybinding Validator
// ============================================================
class KeybindingValidator {
public:
    struct ValidationResult {
        bool is_valid = false;
        std::string error_message;
        std::vector<std::string> warnings;
    };
    
    static ValidationResult validate(const Keybinding& binding, const KeybindingResolver& resolver) {
        ValidationResult result;
        result.is_valid = true;
        
        // Check if key sequence is empty
        if (binding.key_sequence.empty()) {
            result.is_valid = false;
            result.error_message = "Key sequence cannot be empty";
            return result;
        }
        
        // Check if command is empty
        if (binding.command.empty()) {
            result.is_valid = false;
            result.error_message = "Command cannot be empty";
            return result;
        }
        
        // Check for conflicts with existing bindings
        for (const auto& existing : resolver.get_all()) {
            if (existing.key_sequence == binding.key_sequence &&
                existing.modifiers == binding.modifiers &&
                &existing != &binding) {
                result.warnings.push_back(
                    std::string("Conflicts with existing binding for command '") +
                    existing.command + "'"
                );
            }
        }
        
        // Check reserved bindings
        if (!binding.is_reserved) {
            for (const auto& existing : resolver.get_all()) {
                if (existing.is_reserved &&
                    existing.key_sequence == binding.key_sequence &&
                    existing.modifiers == binding.modifiers) {
                    result.is_valid = false;
                    result.error_message =
                        std::string("Cannot override reserved binding '") +
                        existing.key_sequence + "'";
                    return result;
                }
            }
        }
        
        return result;
    }
};

// ============================================================
// Keybinding Template
// ============================================================
class KeybindingTemplate {
public:
    struct TemplateBinding {
        std::string key_sequence;
        std::string command;
        std::string description;
        std::string category;
    };
    
    static std::vector<TemplateBinding> get_default_templates() {
        return {
            {"ctrl+c", "cancel", "Cancel current operation", "Navigation"},
            {"ctrl+d", "exit", "Exit application", "Navigation"},
            {"ctrl+l", "clear_screen", "Clear the screen", "Navigation"},
            {"ctrl+a", "move_to_start", "Move cursor to start", "Editing"},
            {"ctrl+e", "move_to_end", "Move cursor to end", "Editing"},
            {"ctrl+w", "delete_word", "Delete word", "Editing"},
            {"ctrl+u", "delete_line", "Delete line", "Editing"},
            {"ctrl+p", "history_previous", "Previous history entry", "History"},
            {"ctrl+n", "history_next", "Next history entry", "History"},
            {"tab", "complete", "Show completions", "Completion"},
            {"shift+tab", "complete_previous", "Previous completion", "Completion"},
            {"ctrl+r", "search_history", "Search history", "Search"},
        };
    }
    
    static std::vector<Keybinding> apply_template(const std::vector<TemplateBinding>& templates) {
        std::vector<Keybinding> result;
        for (const auto& t : templates) {
            auto parsed = KeybindingParser::parse(t.key_sequence);
            if (parsed) {
                parsed->command = t.command;
                parsed->description = t.description;
                parsed->category = t.category;
                result.push_back(std::move(*parsed));
            }
        }
        return result;
    }
};

// ============================================================
// Keybinding Context Provider
// ============================================================
class KeybindingContext {
public:
    using ActionHandler = std::function<void(const Keybinding&)>;
    
    void set_resolver(std::shared_ptr<KeybindingResolver> resolver) {
        m_resolver = std::move(resolver);
    }
    
    void set_handler(ActionHandler handler) {
        m_handler = std::move(handler);
    }
    
    bool handle_key_event(const KeyEvent& event) {
        if (!m_resolver) {
            return false;
        }
        
        auto binding = m_resolver->resolve(event);
        if (!binding) {
            return false;
        }
        
        if (m_handler) {
            m_handler(*binding);
        }
        
        return true;
    }
    
    KeybindingResolver& resolver() {
        if (!m_resolver) {
            throw std::runtime_error("No resolver set in keybinding context");
        }
        return *m_resolver;
    }
    
    const KeybindingResolver& resolver() const {
        if (!m_resolver) {
            throw std::runtime_error("No resolver set in keybinding context");
        }
        return *m_resolver;
    }
    
private:
    std::shared_ptr<KeybindingResolver> m_resolver;
    ActionHandler m_handler;
};

} // namespace cc::core::keybindings
