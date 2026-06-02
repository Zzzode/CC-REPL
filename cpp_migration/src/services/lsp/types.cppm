// LSP Types Module
module;
#include <string>
#include <unordered_map>
#include <vector>

export module cc.services.lsp.types;

import cc.utils.error;

export namespace cc::services::lsp {

using cc::utils::Result;

// Scoped LSP server config
struct ScopedLspServerConfig {
    std::string command;
    std::vector<std::string> args;
    std::unordered_map<std::string, std::string> env;
    std::unordered_map<std::string, std::string> extension_to_language;
    // More config fields
};

// LSP Client config
struct LspClientConfig {
    std::string server_name;
    ScopedLspServerConfig config;
};

} // namespace cc::services::lsp
