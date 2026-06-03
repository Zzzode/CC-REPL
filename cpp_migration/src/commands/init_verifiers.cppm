module;
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
export module cc.commands.init_verifiers;
export namespace cc::commands::init_verifiers {
namespace fs = std::filesystem;

struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "init_verifiers"; }

[[nodiscard]] inline auto run(std::string_view profile = {}) -> CommandResponse {
    const auto selected = profile.empty() ? std::string{"default"} : std::string(profile);
    const auto dir = fs::current_path() / ".cc-repl" / "verifiers";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return {.ok = false, .message = "Failed to create verifier directory: " + ec.message()};

    const auto path = dir / (selected + ".json");
    std::ofstream out(path, std::ios::trunc);
    if (!out) return {.ok = false, .message = "Failed to write verifier profile: " + path.string()};

    out << "{\n";
    out << "  \"profile\": \"" << selected << "\",\n";
    out << "  \"checks\": [\"build\", \"test\", \"e2e\", \"diff-check\"],\n";
    out << "  \"generated_by\": \"cc-repl-cpp\"\n";
    out << "}\n";
    out.close();

    return {.ok = true, .message = "Verifier profile initialized: " + path.string()};
}
}
