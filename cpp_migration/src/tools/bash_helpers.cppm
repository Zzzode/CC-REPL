module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>

export module cc.tools.bash_helpers;

export namespace cc::tools::bash_helpers {

struct CommentLabel {
    std::string tool_name;
    std::string session_id;
    std::optional<std::string> description;
};

inline std::string make_comment_label(const CommentLabel& label) {
    return "# " + label.tool_name;
}

inline std::string get_tool_name() {
    return "bash";
}

inline bool should_use_sandbox(std::string_view command, bool user_requested) {
    return user_requested;
}

inline std::vector<std::string> split_compound_command(std::string_view command) {
    return {std::string(command)};
}

inline std::string normalize_line_endings(std::string_view text) {
    return std::string(text);
}

inline std::optional<std::string> extract_shebang(std::string_view script) {
    return std::nullopt;
}

} // namespace cc::tools::bash_helpers
