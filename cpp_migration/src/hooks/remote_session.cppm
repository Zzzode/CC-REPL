module;
#include <cstdlib>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

export module cc.hooks.remote_session;

export namespace cc::hooks {


struct RemoteSessionInfo {
    std::string host;
    std::string session_id;
};


inline bool is_remote_session() {

    const char* remote_host = std::getenv("CC_REMOTE_HOST");
    return remote_host != nullptr && remote_host[0] != '\0';
}


inline std::optional<RemoteSessionInfo> get_remote_session_info() {
    if (!is_remote_session()) {
        return std::nullopt;
    }
    const char* host = std::getenv("CC_REMOTE_HOST");
    const char* sid = std::getenv("CC_REMOTE_SESSION_ID");
    return RemoteSessionInfo{
        .host = host ? host : "",
        .session_id = sid ? sid : ""
    };
}


inline std::expected<void, std::string> sync_remote_state() {
    if (!is_remote_session()) {
        return std::unexpected("Not a remote session");
    }

    return {};
}


inline void handle_remote_disconnect() {


}

} // namespace cc::hooks
