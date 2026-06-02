module;
#include <string>
#include <string_view>
export module cc.commands.break_cache;
export namespace cc::commands::break_cache {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "break_cache"; }
[[nodiscard]] inline auto run(std::string_view key = {}) -> CommandResponse { return {.ok = true, .message = "Cache break command ready" + (key.empty() ? std::string{} : ": " + std::string(key))}; }
}
