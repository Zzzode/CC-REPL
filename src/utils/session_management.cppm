module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>

export module cc.utils.session_management;

export namespace cc::utils::session_management {

struct SessionTitle {
    std::string title;
    bool auto_generated{true};
};

struct IngressAuth {
    std::string token;
    std::string method;
    std::optional<std::string> user_id;
};

inline std::expected<SessionTitle, std::string> generate_session_title(std::string_view first_message) {
    return SessionTitle{std::string(first_message).substr(0, 50), true};
}

inline std::expected<void, std::string> update_session_title([[maybe_unused]] std::string_view session_id, [[maybe_unused]] std::string_view new_title) {
    return {};
}

inline std::expected<IngressAuth, std::string> validate_ingress_auth(std::string_view token) {
    return IngressAuth{std::string(token), "bearer", std::nullopt};
}

inline void register_file_access_hook([[maybe_unused]] std::string_view session_id) {}

inline std::vector<std::string> get_accessed_files([[maybe_unused]] std::string_view session_id) {
    return {};
}

} // namespace cc::utils::session_management
