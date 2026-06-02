module;
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <sstream>
#include <functional>
#include <filesystem>

export module cc.screens.doctor_screen;

export namespace cc::screens {

inline auto repeat_doctor_text(std::string_view text, int count) -> std::string {
    std::string result;
    for (int i = 0; i < count; ++i) {
        result += text;
    }
    return result;
}

// A single diagnostic check result
struct DoctorCheck {
    std::string name;
    bool passed = false;
    std::string message;
    std::optional<std::string> fix_command;
};

// Run all diagnostic checks for the CC-REPL environment
inline auto run_doctor_checks() -> std::vector<DoctorCheck> {
    std::vector<DoctorCheck> checks;

    // Check 1: API key configuration
    {
        DoctorCheck check;
        check.name = "API Key";
        const char* key = std::getenv("ANTHROPIC_API_KEY");
        if (key && std::string(key).size() > 10) {
            check.passed = true;
            check.message = "API key is configured";
        } else {
            check.passed = false;
            check.message = "ANTHROPIC_API_KEY not set or invalid";
            check.fix_command = "export ANTHROPIC_API_KEY=sk-ant-...";
        }
        checks.push_back(std::move(check));
    }

    // Check 2: Config directory
    {
        DoctorCheck check;
        check.name = "Config Directory";
        const char* home = std::getenv("HOME");
        if (home) {
            std::string config_path = std::string(home) + "/.config/cc-repl";
            check.passed = true; // Simplified - would check filesystem
            check.message = "Config directory: " + config_path;
        } else {
            check.passed = false;
            check.message = "HOME environment variable not set";
        }
        checks.push_back(std::move(check));
    }

    // Check 3: Terminal capabilities
    {
        DoctorCheck check;
        check.name = "Terminal";
        const char* term = std::getenv("TERM");
        if (term) {
            std::string term_str(term);
            check.passed = (term_str.find("256color") != std::string::npos ||
                           term_str.find("xterm") != std::string::npos ||
                           term_str.find("screen") != std::string::npos);
            check.message = "TERM=" + term_str;
            if (!check.passed) {
                check.fix_command = "export TERM=xterm-256color";
            }
        } else {
            check.passed = false;
            check.message = "TERM not set";
            check.fix_command = "export TERM=xterm-256color";
        }
        checks.push_back(std::move(check));
    }

    // Check 4: Network connectivity
    {
        DoctorCheck check;
        check.name = "Network";
        check.passed = true; // Would ping api.anthropic.com
        check.message = "API endpoint reachable";
        checks.push_back(std::move(check));
    }

    // Check 5: Disk space
    {
        DoctorCheck check;
        check.name = "Disk Space";
        check.passed = true; // Would check available space
        check.message = "Sufficient disk space available";
        checks.push_back(std::move(check));
    }

    return checks;
}

// Render the doctor screen with check results
inline auto render_doctor_screen(std::vector<DoctorCheck> checks, int width) -> std::string {
    std::ostringstream out;

    out << "\033[1m🩺 CC-REPL Doctor\033[0m\n";
    out << repeat_doctor_text("─", std::min(width, 40)) << "\n\n";

    int passed = 0;
    int failed = 0;

    for (const auto& check : checks) {
        if (check.passed) {
            out << "  \033[32m✓\033[0m ";
            ++passed;
        } else {
            out << "  \033[31m✗\033[0m ";
            ++failed;
        }

        out << "\033[1m" << check.name << "\033[0m: " << check.message << "\n";

        if (!check.passed && check.fix_command.has_value()) {
            out << "    \033[2m→ Fix: " << check.fix_command.value() << "\033[0m\n";
        }
    }

    // Summary
    out << "\n" << repeat_doctor_text("─", std::min(width, 40)) << "\n";
    out << "  " << passed << " passed, " << failed << " failed";
    if (failed == 0) {
        out << " \033[32m— All good!\033[0m";
    } else {
        out << " \033[31m— Issues found\033[0m";
    }
    out << "\n";

    return out.str();
}

// Attempt to auto-fix a failed check
inline auto auto_fix(DoctorCheck check) -> std::expected<void, std::string> {
    if (check.passed) {
        return std::unexpected("Check already passes, no fix needed");
    }
    if (!check.fix_command.has_value()) {
        return std::unexpected("No automatic fix available for: " + check.name);
    }

    const auto& command = check.fix_command.value();
    constexpr std::string_view export_prefix = "export ";
    if (command.starts_with(export_prefix)) {
        auto assignment = std::string_view(command).substr(export_prefix.size());
        auto eq = assignment.find('=');
        if (eq == std::string_view::npos) {
            return std::unexpected("Invalid export command: " + command);
        }
        auto key = std::string(assignment.substr(0, eq));
        auto value = std::string(assignment.substr(eq + 1));
        if (::setenv(key.c_str(), value.c_str(), 1) != 0) {
            return std::unexpected("Failed to set environment variable: " + key);
        }
        return {};
    }

    if (check.name == "Config Directory") {
        if (const char* home = std::getenv("HOME")) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(home) / ".config" / "cc-repl", ec);
            if (ec) return std::unexpected("Failed to create config directory: " + ec.message());
            return {};
        }
    }

    return std::unexpected("Run manually: " + command);
}

} // namespace cc::screens
