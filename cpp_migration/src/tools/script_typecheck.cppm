module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>

export module cc.tools.script_typecheck;

export namespace cc::tools::script_typecheck {

enum class TypeCheckSeverity { Error, Warning, Info };

struct TypeCheckDiagnostic {
    std::string file;
    int line;
    int column;
    std::string message;
    TypeCheckSeverity severity;
};

struct TypeCheckResult {
    bool success;
    std::vector<TypeCheckDiagnostic> diagnostics;
};

inline std::expected<TypeCheckResult, std::string> check_script_types(std::string_view script_content) {
    return TypeCheckResult{true, {}};
}

inline std::expected<TypeCheckResult, std::string> check_file_types(std::string_view file_path) {
    return TypeCheckResult{true, {}};
}

inline bool has_type_errors(const TypeCheckResult& result) {
    return false;
}

inline std::vector<TypeCheckDiagnostic> filter_errors(const TypeCheckResult& result) {
    return {};
}

} // namespace cc::tools::script_typecheck
