module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>

export module cc.utils.side_query;

export namespace cc::utils::side_query {

struct SideQueryRequest {
    std::string query;
    std::optional<std::string> context;
    std::optional<std::string> model;
    bool streaming{false};
};

struct SideQueryResponse {
    std::string answer;
    std::optional<std::string> source;
    bool from_cache{false};
};

struct SideQuestion {
    std::string question;
    std::vector<std::string> options;
    std::optional<std::string> default_option;
};

inline std::expected<SideQueryResponse, std::string> execute_side_query([[maybe_unused]] const SideQueryRequest& request) {
    return SideQueryResponse{"", std::nullopt, false};
}

inline std::expected<std::string, std::string> ask_side_question([[maybe_unused]] const SideQuestion& question) {
    return "";
}

inline bool is_side_query_available() {
    return true;
}

} // namespace cc::utils::side_query
