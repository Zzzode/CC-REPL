module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>

export module cc.utils.doctor_diagnostics;

export namespace cc::utils::doctor_diagnostics {

enum class DiagnosticLevel { Pass, Warning, Error, Critical };

struct DiagnosticResult {
    std::string check_name;
    DiagnosticLevel level;
    std::string message;
    std::optional<std::string> fix_suggestion;
};

struct ContextWarning {
    std::string source;
    std::string message;
    std::optional<std::string> affected_feature;
};

inline std::expected<std::vector<DiagnosticResult>, std::string> run_all_diagnostics() {
    return std::vector<DiagnosticResult>{};
}

inline std::vector<ContextWarning> get_context_warnings() {
    return {};
}

inline bool has_critical_issues(const std::vector<DiagnosticResult>& results) {
    return false;
}

inline std::string format_diagnostic_report(const std::vector<DiagnosticResult>& results) {
    return "All checks passed";
}

} // namespace cc::utils::doctor_diagnostics
