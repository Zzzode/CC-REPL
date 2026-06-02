module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

export module cc.commands.thinkback;

export namespace cc::commands {

using time_point = std::chrono::system_clock::time_point;

auto thinking_history_path() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home} / ".cc-repl" / "thinking-history.txt";
    return std::filesystem::path{".cc-repl"} / "thinking-history.txt";
}

// 思考历史条目
struct ThinkingEntry {
    time_point ts;
    std::string content;
    int tokens;
};

// 获取思考历史记录
auto get_thinking_history() -> std::vector<ThinkingEntry> {
    std::vector<ThinkingEntry> history;
    std::ifstream input{thinking_history_path()};
    std::string line;
    while (std::getline(input, line)) {
        std::stringstream ss{line};
        std::string ticks;
        std::string tokens;
        std::string content;
        if (!std::getline(ss, ticks, '|') || !std::getline(ss, tokens, '|') || !std::getline(ss, content)) continue;
        history.push_back(ThinkingEntry{
            .ts = time_point{std::chrono::system_clock::duration{std::stoll(ticks)}},
            .content = content,
            .tokens = std::stoi(tokens),
        });
    }
    return history;
}

// 回放指定索引的思考内容
auto replay_thinking(size_t index) -> std::string {
    auto history = get_thinking_history();
    if (index >= history.size()) {
        return "No thinking entry at index " + std::to_string(index);
    }
    return history[index].content;
}

// 导出所有思考历史为可读文本
auto export_thinking_log() -> std::string {
    auto history = get_thinking_history();
    if (history.empty()) {
        return "No thinking history available.";
    }

    std::string log = "=== Thinking History Export ===\n\n";
    for (size_t i = 0; i < history.size(); ++i) {
        log += "--- Entry " + std::to_string(i + 1) + " ---\n";
        log += "Tokens: " + std::to_string(history[i].tokens) + "\n";
        log += history[i].content + "\n\n";
    }
    return log;
}

// 清除所有思考历史
auto clear_thinking_history() -> void {
    std::filesystem::remove(thinking_history_path());
}

} // namespace cc::commands
