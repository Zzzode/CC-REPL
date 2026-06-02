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

export module cc.commands.privacy_settings;

export namespace cc::commands {

auto privacy_settings_path() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home} / ".cc-repl" / "privacy.txt";
    return std::filesystem::path{".cc-repl"} / "privacy.txt";
}

auto parse_bool(std::string_view value, bool fallback) -> bool {
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    return fallback;
}

// 隐私设置配置
struct PrivacySettings {
    bool telemetry;        // 是否允许遥测数据收集
    bool crash_reports;    // 是否发送崩溃报告
    bool usage_stats;      // 是否收集使用统计
    std::string data_retention; // 数据保留策略（"30d", "90d", "1y", "forever"）
};

// 获取当前隐私设置
auto get_privacy_settings() -> PrivacySettings {
    PrivacySettings settings{
        .telemetry = false,
        .crash_reports = true,
        .usage_stats = false,
        .data_retention = "90d"
    };
    std::ifstream input{privacy_settings_path()};
    std::string line;
    while (std::getline(input, line)) {
        auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        auto key = line.substr(0, separator);
        auto value = line.substr(separator + 1);
        if (key == "telemetry") settings.telemetry = parse_bool(value, settings.telemetry);
        else if (key == "crash_reports") settings.crash_reports = parse_bool(value, settings.crash_reports);
        else if (key == "usage_stats") settings.usage_stats = parse_bool(value, settings.usage_stats);
        else if (key == "data_retention") settings.data_retention = value;
    }
    return settings;
}

// 更新隐私设置
auto set_privacy_settings(PrivacySettings settings) -> void {
    auto path = privacy_settings_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::trunc};
    output << "telemetry=" << (settings.telemetry ? "true" : "false") << '\n'
           << "crash_reports=" << (settings.crash_reports ? "true" : "false") << '\n'
           << "usage_stats=" << (settings.usage_stats ? "true" : "false") << '\n'
           << "data_retention=" << settings.data_retention << '\n';
}

// 显示隐私相关信息（数据收集说明等）
auto show_privacy_info() -> std::string {
    auto settings = get_privacy_settings();

    std::string info = "Privacy Settings:\n";
    info += "  Telemetry:       " + std::string(settings.telemetry ? "Enabled" : "Disabled") + "\n";
    info += "  Crash Reports:   " + std::string(settings.crash_reports ? "Enabled" : "Disabled") + "\n";
    info += "  Usage Statistics: " + std::string(settings.usage_stats ? "Enabled" : "Disabled") + "\n";
    info += "  Data Retention:  " + settings.data_retention + "\n";
    info += "\nData Collection:\n";
    info += "  - We collect anonymized usage patterns to improve the product.\n";
    info += "  - Crash reports help us identify and fix bugs faster.\n";
    info += "  - No conversation content is ever stored on our servers.\n";
    info += "  - You can opt out of all data collection at any time.\n";
    return info;
}

} // namespace cc::commands
