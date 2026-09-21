module;
#include <cstdio>
#include <string>
#include <string_view>

export module cc.hooks.notifs.lsp_initialization;

export namespace cc::hooks::notifs {


inline void show_lsp_initializing_notification(std::string_view language) {
    std::fprintf(stderr, "[LSP] Initializing language server for %.*s...\n",
                 static_cast<int>(language.size()), language.data());
}


inline void show_lsp_ready_notification(std::string_view language) {
    std::fprintf(stderr, "[LSP] Language server for %.*s is ready.\n",
                 static_cast<int>(language.size()), language.data());
}


inline void show_lsp_error_notification(std::string_view language, std::string_view error) {
    std::fprintf(stderr, "[LSP] Error starting language server for %.*s: %.*s\n",
                 static_cast<int>(language.size()), language.data(),
                 static_cast<int>(error.size()), error.data());
}

} // namespace cc::hooks::notifs
