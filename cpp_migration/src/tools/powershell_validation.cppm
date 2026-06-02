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

inline PSValidationResult validate_ps_mode(std::string_view command, PSValidationMode mode) {
    return {true, std::nullopt};
}

inline PSValidationResult validate_ps_paths(std::string_view command, std::string_view working_dir) {
    return {true, std::nullopt};
}

inline PSValidationResult validate_ps_read_only(std::string_view command) {
    return {true, std::nullopt};
}

inline bool is_destructive_ps_command(std::string_view command) {
    return false;
}

inline bool check_git_safety(std::string_view command) {
    return true;
}

inline std::optional<std::string> get_ps_destructive_warning(std::string_view command) {
    return std::nullopt;
}

} // namespace cc::tools::powershell_validation
