module;
#include <map>
#include <string>
#include <string_view>

export module cc.utils.model.aliases;

export namespace cc::utils {

// Canonical alias-to-model mapping
inline const std::map<std::string, std::string>& alias_map() {
    static const std::map<std::string, std::string> m = {
        {"sonnet",       "claude-sonnet-4-20250514"},
        {"opus",         "claude-opus-4-20250514"},
        {"haiku",        "claude-haiku-4-20250514"},
        {"sonnet-4",     "claude-sonnet-4-20250514"},
        {"opus-4",       "claude-opus-4-20250514"},
        {"haiku-4",      "claude-haiku-4-20250514"},
    };
    return m;
}

std::string resolve_model_alias(std::string_view alias) {
    auto& m = alias_map();
    auto it = m.find(std::string(alias));
    if (it != m.end()) {
        return it->second;
    }
    // If not an alias, return input as-is
    return std::string(alias);
}

std::map<std::string, std::string> get_all_aliases() {
    return alias_map();
}

bool is_alias(std::string_view name) {
    auto& m = alias_map();
    return m.contains(std::string(name));
}

} // namespace cc::utils
