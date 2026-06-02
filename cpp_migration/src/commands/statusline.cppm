// Statusline command - configures shell statusline/PS1 integration
module;
#include <cstdlib>
#include <string>
#include <string_view>
export module cc.commands.statusline;
export namespace cc::commands::statusline {

struct CommandResponse { bool ok{true}; std::string message; };

[[nodiscard]] inline auto name() -> std::string_view { return "statusline"; }

[[nodiscard]] inline auto run(std::string_view format = {}) -> CommandResponse {
    // Detect current shell
    std::string shell = "unknown";
    if (auto* s = std::getenv("SHELL"); s && s[0] != '\0') {
        std::string_view sv(s);
        if (sv.find("zsh") != std::string_view::npos) shell = "zsh";
        else if (sv.find("bash") != std::string_view::npos) shell = "bash";
        else if (sv.find("fish") != std::string_view::npos) shell = "fish";
    }
    
    std::string msg = "Statusline Setup\n\n";
    msg += "Detected shell: " + shell + "\n\n";
    
    if (shell == "zsh") {
        msg += "Add to your ~/.zshrc:\n\n"
               "  # CC-REPL statusline integration\n"
               "  precmd() {\n"
               "    local status_info=$(cc-repl --status-json 2>/dev/null)\n"
               "    if [[ -n \"$status_info\" ]]; then\n"
               "      RPROMPT=\"%F{cyan}[cc]%f\"\n"
               "    fi\n"
               "  }\n";
    } else if (shell == "bash") {
        msg += "Add to your ~/.bashrc:\n\n"
               "  # CC-REPL statusline integration\n"
               "  PROMPT_COMMAND='__cc_repl_prompt'\n"
               "  __cc_repl_prompt() {\n"
               "    local status=$(cc-repl --status-json 2>/dev/null)\n"
               "    # Customize PS1 based on status\n"
               "  }\n";
    } else if (shell == "fish") {
        msg += "Add to your ~/.config/fish/conf.d/cc-repl.fish:\n\n"
               "  function fish_right_prompt\n"
               "    set -l status_info (cc-repl --status-json 2>/dev/null)\n"
               "    if test -n \"$status_info\"\n"
               "      set_color cyan; echo '[cc]'; set_color normal\n"
               "    end\n"
               "  end\n";
    } else {
        msg += "Could not detect shell type. Set SHELL environment variable.\n";
    }
    
    if (!format.empty()) {
        msg += "\nCustom format: " + std::string(format) + "\n";
    }
    
    return {.ok = true, .message = msg};
}

}
