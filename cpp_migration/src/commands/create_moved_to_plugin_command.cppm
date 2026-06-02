module;
#include <string>
#include <string_view>
export module cc.commands.create_moved_to_plugin_command;
export namespace cc::commands::create_moved_to_plugin_command {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "create_moved_to_plugin_command"; }
[[nodiscard]] inline auto run(std::string_view command = {}) -> CommandResponse { return {.ok = true, .message = "Moved-to-plugin command adapter ready" + (command.empty() ? std::string{} : ": " + std::string(command))}; }
}
