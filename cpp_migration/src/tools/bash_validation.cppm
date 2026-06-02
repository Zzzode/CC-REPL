module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>

export module cc.tools.bash_validation;

export namespace cc::tools::bash_validation {

enum class ValidationMode {
    Normal,
    ReadOnly,
    Restricted,
    Sandbox
};

struct ValidationResult {
    bool valid;
    std::optional<std::string> reason;
};

struct PathValidationContext {
    std::string working_dir;
    std::vector<std::string> allowed_paths;
    bool allow_absolute{true};
};

inline ValidationResult validate_mode(std::string_view command, ValidationMode mode) {
    return {true, std::nullopt};
}

inline ValidationResult validate_paths(std::string_view command, const PathValidationContext& ctx) {
    return {true, std::nullopt};
}

inline ValidationResult validate_read_only(std::string_view command) {
    return {true, std::nullopt};
}

inline ValidationResult validate_sed_command(std::string_view sed_expr) {
    return {true, std::nullopt};
}

inline bool is_destructive_command(std::string_view command) {
    return false;
}

inline std::optional<std::string> get_destructive_warning(std::string_view command) {
    return std::nullopt;
}

} // namespace cc::tools::bash_validation
