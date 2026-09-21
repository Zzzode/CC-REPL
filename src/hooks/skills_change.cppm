module;
#include <functional>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.skills_change;

export namespace cc::hooks {

namespace detail {

    inline std::vector<std::function<void(std::vector<std::string>)>>& skills_listeners() {
        static std::vector<std::function<void(std::vector<std::string>)>> listeners;
        return listeners;
    }

    inline int& next_skills_listener_id() {
        static int id = 0;
        return id;
    }


    inline std::vector<std::string>& active_skills_list() {
        static std::vector<std::string> skills;
        return skills;
    }
} // namespace detail


inline int on_skills_changed(std::function<void(std::vector<std::string>)> callback) {
    detail::skills_listeners().push_back(std::move(callback));
    return ++detail::next_skills_listener_id();
}


inline std::vector<std::string> get_active_skills() {
    return detail::active_skills_list();
}


inline void notify_skill_loaded(std::string_view skill_name) {
    detail::active_skills_list().emplace_back(skill_name);

    for (auto& listener : detail::skills_listeners()) {
        listener(detail::active_skills_list());
    }
}


inline void notify_skill_unloaded(std::string_view skill_name) {
    auto& skills = detail::active_skills_list();
    std::erase_if(skills, [&](const std::string& s) { return s == skill_name; });

    for (auto& listener : detail::skills_listeners()) {
        listener(skills);
    }
}

} // namespace cc::hooks
