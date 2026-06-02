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
#include <iterator>

export module cc.commands.keybindings_cmd;

export namespace cc::commands {

auto keybindings_path() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home} / ".cc-repl" / "keybindings.txt";
    return std::filesystem::path{".cc-repl"} / "keybindings.txt";
}

auto default_keybindings() -> std::map<std::string, std::string> {
    return {{"cancel", "Ctrl+C"}, {"exit", "Ctrl+D"}, {"clear", "Ctrl+L"}, {"search_history", "Ctrl+R"},
            {"accept_suggestion", "Tab"}, {"cycle_suggestions", "Shift+Tab"}, {"history_up", "Up"},
            {"history_down", "Down"}, {"dismiss", "Escape"}};
}

auto load_keybindings() -> std::map<std::string, std::string> {
    auto bindings = default_keybindings();
    std::ifstream input{keybindings_path()};
    std::string line;
    while (std::getline(input, line)) {
        auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        bindings[line.substr(0, separator)] = line.substr(separator + 1);
    }
    return bindings;
}

// 显示当前所有快捷键绑定
auto show_keybindings() -> std::string {
    std::string output = "Current Keybindings:\n";
    output += "  Ctrl+C     - Cancel current operation\n";
    output += "  Ctrl+D     - Exit\n";
    output += "  Ctrl+L     - Clear screen\n";
    output += "  Ctrl+R     - Search history\n";
    output += "  Tab        - Accept suggestion\n";
    output += "  Shift+Tab  - Cycle suggestions\n";
    output += "  Up/Down    - Navigate history\n";
    output += "  Escape     - Dismiss\n";
    return output;
}

// 设置自定义快捷键绑定
auto set_keybinding(std::string_view action, std::string_view keys)
    -> std::expected<void, std::string> {
    if (action.empty()) {
        return std::unexpected("Action cannot be empty");
    }
    if (keys.empty()) {
        return std::unexpected("Key binding cannot be empty");
    }
    auto bindings = load_keybindings();
    bindings[std::string(action)] = std::string(keys);
    auto path = keybindings_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::trunc};
    if (!output) return std::unexpected("Cannot write keybindings config");
    for (const auto& [name, binding] : bindings) output << name << '=' << binding << '\n';
    return {};
}

// 重置所有快捷键为默认值
auto reset_keybindings() -> void {
    std::filesystem::remove(keybindings_path());
}

// 导出快捷键配置为 JSON 字符串
auto export_keybindings() -> std::string {
    auto bindings = load_keybindings();
    std::string json = "{\n  \"bindings\": {\n";
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        json += "    \"" + it->first + "\": \"" + it->second + "\"";
        json += std::next(it) == bindings.end() ? "\n" : ",\n";
    }
    json += "  }\n}";
    return json;
}

} // namespace cc::commands
