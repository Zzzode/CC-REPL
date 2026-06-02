module;
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>
export module cc.services.lsp.diagnostic_registry;

export namespace cc::services::lsp {

namespace fs = std::filesystem;

// LSP diagnostic severity levels
enum class DiagnosticSeverity { Error, Warning, Information, Hint };

// A single diagnostic entry
struct Diagnostic {
    int line{0};
    int column{0};
    std::string message;
    DiagnosticSeverity severity{DiagnosticSeverity::Error};
    std::string source;
};

// Registry for managing diagnostics across files
class DiagnosticRegistry {
public:
    // Register a diagnostic for a file
    auto register_diagnostic(const fs::path& file, Diagnostic diag) -> void {
        std::lock_guard lock(mutex_);
        diagnostics_[file.string()].push_back(std::move(diag));
    }

    // Get all diagnostics for a file
    auto get_diagnostics(const fs::path& file) -> std::vector<Diagnostic> {
        std::lock_guard lock(mutex_);
        auto it = diagnostics_.find(file.string());
        if (it != diagnostics_.end()) {
            return it->second;
        }
        return {};
    }

    // Clear diagnostics for a file
    auto clear_diagnostics(const fs::path& file) -> void {
        std::lock_guard lock(mutex_);
        diagnostics_.erase(file.string());
    }

    // Get total error count across all files
    auto get_error_count() -> size_t {
        std::lock_guard lock(mutex_);
        size_t count = 0;
        for (const auto& [_, diags] : diagnostics_) {
            for (const auto& d : diags) {
                if (d.severity == DiagnosticSeverity::Error) {
                    ++count;
                }
            }
        }
        return count;
    }

private:
    std::mutex mutex_;
    std::map<std::string, std::vector<Diagnostic>> diagnostics_;
};

} // namespace cc::services::lsp
