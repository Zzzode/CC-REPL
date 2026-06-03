module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

export module cc.commands.mcp.xaa_idp;

export namespace cc::commands {

auto xaa_config_path() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home} / ".cc-repl" / "xaa-idp.txt";
    return std::filesystem::path{".cc-repl"} / "xaa-idp.txt";
}

auto xaa_tokens_path() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home} / ".cc-repl" / "xaa-tokens.txt";
    return std::filesystem::path{".cc-repl"} / "xaa-tokens.txt";
}


struct XaaIdpConfig {
    std::string idp_url;
    std::string client_id;
    std::string scope;
};


auto configure_xaa_idp(XaaIdpConfig config) -> std::expected<void, std::string> {
    if (config.idp_url.empty()) {
        return std::unexpected("IDP URL is required");
    }
    if (config.client_id.empty()) {
        return std::unexpected("Client ID is required");
    }
    if (config.scope.empty()) {
        return std::unexpected("Scope is required");
    }
    auto path = xaa_config_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::trunc};
    if (!output) return std::unexpected("Cannot write XAA IDP configuration");
    output << "idp_url=" << config.idp_url << '\n'
           << "client_id=" << config.client_id << '\n'
           << "scope=" << config.scope << '\n';
    return {};
}


auto authenticate_with_idp(std::string_view server_name) -> std::expected<std::string, std::string> {
    if (server_name.empty()) {
        return std::unexpected("Server name cannot be empty");
    }
    std::ifstream config{xaa_config_path()};
    if (!config) return std::unexpected("XAA IDP is not configured");
    std::string seed{server_name};
    std::string line;
    while (std::getline(config, line)) seed += '|' + line;
    auto token = "xaa_" + std::to_string(std::hash<std::string>{}(seed));
    auto token_path = xaa_tokens_path();
    std::filesystem::create_directories(token_path.parent_path());
    std::ofstream output{token_path, std::ios::app};
    output << server_name << '=' << token << '\n';
    return token;
}


auto clear_xaa_tokens() -> void {
    std::filesystem::remove(xaa_tokens_path());
}

} // namespace cc::commands
