module;
#include <map>
#include <string>
#include <utility>

export module cc.utils.control_message_compat;

export namespace cc::utils::control_message_compat {

struct ControlMessageLike {
    std::map<std::string, std::string> fields;
    bool has_response = false;
    std::map<std::string, std::string> response;
};

namespace detail {

inline void normalize_request_id_key(std::map<std::string, std::string>& object) {
    const auto camel = object.find("requestId");
    if (camel == object.end() || object.contains("request_id")) return;

    object.emplace("request_id", camel->second);
    object.erase(camel);
}

} // namespace detail

inline ControlMessageLike& normalize_control_message_keys(ControlMessageLike& message) {
    detail::normalize_request_id_key(message.fields);
    if (message.has_response) {
        detail::normalize_request_id_key(message.response);
    }
    return message;
}

} // namespace cc::utils::control_message_compat
