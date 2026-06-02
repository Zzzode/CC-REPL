module;
#include <string>
#include <string_view>
#include <map>
#include <optional>
#include <mutex>

export module cc.skills.bundled.remember;

import cc.skills.load_skills_dir;

export namespace cc::skills::bundled {

namespace detail {
    // Thread-safe memory storage
    inline std::map<std::string, std::string>& get_memory_store() {
        static std::map<std::string, std::string> store;
        return store;
    }

    inline std::mutex& get_memory_mutex() {
        static std::mutex mutex;
        return mutex;
    }
}

// Store a key-value pair in memory
void remember(std::string_view key, std::string_view value) {
    if (key.empty()) return;

    std::lock_guard lock(detail::get_memory_mutex());
    detail::get_memory_store()[std::string(key)] = std::string(value);
}

// Recall a previously stored value by key
std::optional<std::string> recall(std::string_view key) {
    std::lock_guard lock(detail::get_memory_mutex());
    auto& store = detail::get_memory_store();

    auto it = store.find(std::string(key));
    if (it != store.end()) {
        return it->second;
    }
    return std::nullopt;
}

// Forget (delete) a stored key-value pair
bool forget(std::string_view key) {
    std::lock_guard lock(detail::get_memory_mutex());
    auto& store = detail::get_memory_store();

    auto it = store.find(std::string(key));
    if (it != store.end()) {
        store.erase(it);
        return true;
    }
    return false;
}

// List all stored memories
std::map<std::string, std::string> list_memories() {
    std::lock_guard lock(detail::get_memory_mutex());
    return detail::get_memory_store();
}

// Get the skill manifest for the remember skill
cc::skills::SkillManifest get_remember_skill_manifest() {
    return cc::skills::SkillManifest{
        .name = "remember",
        .description = "Store and recall information across conversation turns",
        .version = "1.0.0",
        .triggers = {"remember", "recall", "forget", "memorize", "store this"},
        .directory = {}
    };
}

} // namespace cc::skills::bundled
