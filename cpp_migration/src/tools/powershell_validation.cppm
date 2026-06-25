module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>

export module cc.tools.powershell_validation;

export namespace cc::tools::powershell_validation {

enum class PSValidationMode {
    Normal,
    ConstrainedLanguage,
    ReadOnly
};

struct PSValidationResult {
    bool valid;
    std::optional<std::string> reason;
};

inline PSValidationResult validate_ps_mode(
    [[maybe_unused]] std::string_view command,
    [[maybe_unused]] PSValidationMode mode) {
    return {true, std::nullopt};
}

inline PSValidationResult validate_ps_paths(
    [[maybe_unused]] std::string_view command,
    [[maybe_unused]] std::string_view working_dir) {
    return {true, std::nullopt};
}

inline PSValidationResult validate_ps_read_only([[maybe_unused]] std::string_view command) {
    return {true, std::nullopt};
}

inline bool is_destructive_ps_command([[maybe_unused]] std::string_view command) {
    return false;
}

inline bool check_git_safety([[maybe_unused]] std::string_view command) {
    return true;
}

inline std::optional<std::string> get_ps_destructive_warning(
    [[maybe_unused]] std::string_view command) {
    return std::nullopt;
}

} // namespace cc::tools::powershell_validation
