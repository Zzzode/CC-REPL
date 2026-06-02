module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <functional>

export module cc.utils.prompt_helpers;

export namespace cc::utils::prompt_helpers {

struct EditorConfig {
    std::string editor_command;
    std::optional<std::string> args;
    bool wait_for_close{true};
};

struct PromptSubmitResult {
    std::string content;
    bool cancelled{false};
    std::optional<std::string> command;
};

inline std::expected<std::string, std::string> open_in_editor(
    std::string_view content, const EditorConfig& config) {
    return std::string(content);
}

inline std::expected<PromptSubmitResult, std::string> handle_prompt_submit(
    std::string_view raw_input) {
    return PromptSubmitResult{std::string(raw_input), false, std::nullopt};
}

inline std::expected<std::string, std::string> execute_shell_prompt(
    std::string_view command) {
    return "";
}

inline std::string capture_early_input() {
    return "";
}

inline bool is_command_input(std::string_view input) {
    return !input.empty() && input[0] == '/';
}

} // namespace cc::utils::prompt_helpers
