module;
#include <expected>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
export module cc.services.lsp.manager;

export namespace cc::services::lsp {

// LSP server manager for multi-language support
class LspManager {
public:
    // Start an LSP server for a specific language
    auto start_server(std::string_view language) -> std::expected<void, std::string> {
        std::lock_guard lock(mutex_);
        if (!is_language_supported(language)) {
            return std::unexpected("Language not supported: " + std::string(language));
        }
        if (running_servers_.contains(std::string(language))) {
            return std::unexpected("Server already running for: " + std::string(language));
        }
        // Track requested server state; process ownership lives outside this module.
        running_servers_[std::string(language)] = true;
        return {};
    }

    // Stop an LSP server for a specific language
    auto stop_server(std::string_view language) -> void {
        std::lock_guard lock(mutex_);
        running_servers_.erase(std::string(language));
    }

    // Get list of currently running server languages
    auto get_running_servers() -> std::vector<std::string> {
        std::lock_guard lock(mutex_);
        std::vector<std::string> result;
        for (const auto& [lang, _] : running_servers_) {
            result.push_back(lang);
        }
        return result;
    }

    // Check if a language has LSP support
    auto is_language_supported(std::string_view language) -> bool {
        static const std::vector<std::string> supported = {
            "typescript", "javascript", "python", "rust",
            "go", "c", "cpp", "java"
        };
        for (const auto& lang : supported) {
            if (lang == language) return true;
        }
        return false;
    }

private:
    std::mutex mutex_;
    std::map<std::string, bool> running_servers_;
};

} // namespace cc::services::lsp
