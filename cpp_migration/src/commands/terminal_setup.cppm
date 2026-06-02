// Terminal Setup command - configures terminal for optimal CC-REPL experience
module;
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
export module cc.commands.terminal_setup;
export namespace cc::commands::terminal_setup {

struct CommandResponse { bool ok{true}; std::string message; };

[[nodiscard]] inline auto name() -> std::string_view { return "terminal-setup"; }

/// Detect terminal emulator type from environment
[[nodiscard]] inline auto detect_terminal() -> std::string {
    if (auto* prog = std::getenv("TERM_PROGRAM"); prog && prog[0] != '\0') {
        return prog;
    }
    if (auto* term = std::getenv("TERM"); term && term[0] != '\0') {
        return term;
    }
    return "unknown";
}

/// Detect shell type
[[nodiscard]] inline auto detect_shell() -> std::string {
    if (auto* s = std::getenv("SHELL"); s && s[0] != '\0') {
        std::string_view sv(s);
        if (sv.find("zsh") != std::string_view::npos) return "zsh";
        if (sv.find("bash") != std::string_view::npos) return "bash";
        if (sv.find("fish") != std::string_view::npos) return "fish";
        return std::string(sv);
    }
    return "unknown";
}

[[nodiscard]] inline auto run(std::string_view shell_override = {}) -> CommandResponse {
    auto terminal = detect_terminal();
    auto shell = shell_override.empty() ? detect_shell() : std::string(shell_override);
    
    std::string msg = "Terminal Setup\n";
    msg += std::string(60, '=') + "\n\n";
    msg += std::format("Terminal: {}\n", terminal);
    msg += std::format("Shell: {}\n\n", shell);
    
    // Terminal-specific setup
    if (terminal == "Apple_Terminal") {
        msg += "Apple Terminal detected.\n\n";
        msg += "Recommendations:\n";
        msg += "  - Consider switching to iTerm2 or Ghostty for better Unicode/color support\n";
        msg += "  - Enable 'Use Option as Meta Key' in Preferences > Profiles > Keyboard\n";
        msg += "  - Set TERM=xterm-256color in your shell profile\n\n";
    } else if (terminal == "iTerm.app" || terminal == "iTerm2") {
        msg += "iTerm2 detected - excellent choice!\n\n";
        msg += "Recommendations:\n";
        msg += "  - Enable 'CSI u' keyboard mode: Profiles > Keys > General\n";
        msg += "  - Set 'Report Terminal Type' to xterm-256color\n\n";
    } else if (terminal == "ghostty") {
        msg += "Ghostty detected - great terminal!\n\n";
        msg += "All features should work out of the box.\n\n";
    } else if (terminal == "vscode") {
        msg += "VS Code integrated terminal detected.\n\n";
        msg += "Recommendations:\n";
        msg += "  - Set \"terminal.integrated.env.osx\": {\"TERM\": \"xterm-256color\"}\n";
        msg += "  - Consider using Trae IDE for native AI coding integration\n\n";
    }
    
    // Shell completion setup
    msg += "Shell Completions\n";
    msg += std::string(40, '-') + "\n\n";
    
    if (shell == "zsh") {
        msg += "Add to ~/.zshrc:\n";
        msg += "  eval \"$(cc-repl completions zsh)\"\n\n";
    } else if (shell == "bash") {
        msg += "Add to ~/.bashrc:\n";
        msg += "  eval \"$(cc-repl completions bash)\"\n\n";
    } else if (shell == "fish") {
        msg += "Run once:\n";
        msg += "  cc-repl completions fish > ~/.config/fish/completions/cc-repl.fish\n\n";
    }
    
    // Keyboard protocol
    msg += "Keyboard Protocol\n";
    msg += std::string(40, '-') + "\n\n";
    msg += "CC-REPL supports the Kitty keyboard protocol (CSI-u) for enhanced key\n";
    msg += "detection. This is automatically enabled in supported terminals:\n";
    msg += "  - Kitty, Ghostty, WezTerm, iTerm2 (3.5+), foot\n\n";
    
    // Environment variables
    msg += "Environment Variables\n";
    msg += std::string(40, '-') + "\n\n";
    msg += "  ANTHROPIC_API_KEY    - Your API key (required)\n";
    msg += "  CC_REPL_THEME       - Color theme (dark/light/auto)\n";
    msg += "  NO_COLOR            - Disable colors (set to any value)\n";
    msg += "  CC_REPL_MAX_TOKENS  - Override default max output tokens\n";
    
    return {.ok = true, .message = msg};
}

}
