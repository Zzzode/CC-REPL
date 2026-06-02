module;
#include <string>
#include <string_view>
export module cc.commands.version;
export namespace cc::commands::version {

struct CommandResponse { bool ok{true}; std::string message; };

[[nodiscard]] inline auto name() -> std::string_view { return "version"; }

[[nodiscard]] inline auto run(std::string_view detail = {}) -> CommandResponse {
    // CC_REPL_VERSION is defined via CMakeLists.txt; fallback to dev
    #ifndef CC_REPL_VERSION
    #define CC_REPL_VERSION "0.1.0-dev"
    #endif

    std::string msg = "cc-repl " CC_REPL_VERSION " (C++23, built " __DATE__ ")";
    if (!detail.empty()) {
        msg += " [";
        msg += detail;
        msg += "]";
    }
    return {.ok = true, .message = std::move(msg)};
}

} // namespace cc::commands::version
