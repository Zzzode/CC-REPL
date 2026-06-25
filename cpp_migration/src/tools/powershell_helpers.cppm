module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>

export module cc.tools.powershell_helpers;

export namespace cc::tools::powershell_helpers {

enum class CLMMode {
    FullLanguage,
    ConstrainedLanguage,
    NoLanguage
};

struct CommonParameter {
    std::string name;
    std::string type;
    bool is_switch{false};
};

inline CLMMode get_clm_mode() {
    return CLMMode::FullLanguage;
}

inline std::vector<CommonParameter> get_common_parameters() {
    return {};
}

inline std::string get_ps_tool_name() {
    return "powershell";
}

inline bool is_common_parameter([[maybe_unused]] std::string_view param_name) {
    return false;
}

inline std::string normalize_ps_command(std::string_view command) {
    return std::string(command);
}

} // namespace cc::tools::powershell_helpers
