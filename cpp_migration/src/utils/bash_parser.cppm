// C++23 Bash Parser Module
// Provides utilities for parsing and manipulating bash commands
module;

#include <string>
#include <vector>
#include <optional>
#include <cctype>
#include <algorithm>

export module cc.utils.bash_parser;

export namespace cc::utils::bash_parser {


enum class ParseState {
    Normal,
    SingleQuote,
    DoubleQuote,
    Escape,
    Comment
};


struct Token {
    std::string value;
    bool is_quoted;
    char quote_type;
};


struct ParsedCommand {
    std::string command;
    std::vector<std::string> arguments;
    std::vector<Token> tokens;
    std::string redirect_input;
    std::string redirect_output;
    bool append_output;
    bool background;
};


[[nodiscard]] inline std::vector<Token> tokenize(std::string_view command) {
    std::vector<Token> tokens;
    std::string current_token;
    ParseState state = ParseState::Normal;
    char quote_char = 0;
    
    for (std::size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        
        switch (state) {
            case ParseState::Normal:
                if (std::isspace(static_cast<unsigned char>(c))) {
                    if (!current_token.empty()) {
                        tokens.push_back({std::move(current_token), false, 0});
                        current_token.clear();
                    }
                } else if (c == '\'') {
                    if (!current_token.empty()) {
                        tokens.push_back({std::move(current_token), false, 0});
                        current_token.clear();
                    }
                    state = ParseState::SingleQuote;
                    quote_char = '\'';
                } else if (c == '"') {
                    if (!current_token.empty()) {
                        tokens.push_back({std::move(current_token), false, 0});
                        current_token.clear();
                    }
                    state = ParseState::DoubleQuote;
                    quote_char = '"';
                } else if (c == '\\') {
                    state = ParseState::Escape;
                } else if (c == '#') {
                    if (!current_token.empty()) {
                        tokens.push_back({std::move(current_token), false, 0});
                        current_token.clear();
                    }
                    state = ParseState::Comment;
                } else {
                    current_token += c;
                }
                break;
                
            case ParseState::SingleQuote:
                if (c == '\'') {
                    tokens.push_back({std::move(current_token), true, '\''});
                    current_token.clear();
                    state = ParseState::Normal;
                    quote_char = 0;
                } else {
                    current_token += c;
                }
                break;
                
            case ParseState::DoubleQuote:
                if (c == '"') {
                    tokens.push_back({std::move(current_token), true, '"'});
                    current_token.clear();
                    state = ParseState::Normal;
                    quote_char = 0;
                } else if (c == '\\') {
                    state = ParseState::Escape;
                } else {
                    current_token += c;
                }
                break;
                
            case ParseState::Escape:
                current_token += c;
                if (quote_char == 0) {
                    state = ParseState::Normal;
                } else if (quote_char == '"') {
                    state = ParseState::DoubleQuote;
                }
                break;
                
            case ParseState::Comment:

                break;
        }
    }
    
    if (!current_token.empty()) {
        tokens.push_back({std::move(current_token), quote_char != 0, quote_char});
    }
    
    return tokens;
}


[[nodiscard]] inline ParsedCommand parse_command(std::string_view command) {
    ParsedCommand result;
    auto tokens = tokenize(command);
    
    result.tokens = tokens;
    
    bool first = true;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        

        if (token.value == ">") {
            if (i + 1 < tokens.size()) {
                result.redirect_output = tokens[i + 1].value;
                i++;
            }
            continue;
        }
        if (token.value == ">>") {
            if (i + 1 < tokens.size()) {
                result.redirect_output = tokens[i + 1].value;
                result.append_output = true;
                i++;
            }
            continue;
        }
        if (token.value == "<") {
            if (i + 1 < tokens.size()) {
                result.redirect_input = tokens[i + 1].value;
                i++;
            }
            continue;
        }
        

        if (token.value == "&" && i == tokens.size() - 1) {
            result.background = true;
            continue;
        }
        
        if (first) {
            result.command = token.value;
            first = false;
        } else {
            result.arguments.push_back(token.value);
        }
    }
    
    return result;
}


[[nodiscard]] inline std::string shell_escape(std::string_view str) {
    std::string result;
    
    if (str.empty()) {
        return "''";
    }
    
    bool needs_quote = false;
    for (char c : str) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && 
            c != '-' && c != '_' && c != '.' && c != '/' && c != ':') {
            needs_quote = true;
            break;
        }
    }
    
    if (!needs_quote) {
        return std::string(str);
    }
    
    result += '\'';
    for (char c : str) {
        if (c == '\'') {
            result += "'\\''";
        } else {
            result += c;
        }
    }
    result += '\'';
    
    return result;
}


[[nodiscard]] inline std::string build_command(const std::vector<std::string>& args) {
    std::string result;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            result += ' ';
        }
        result += shell_escape(args[i]);
    }
    return result;
}


[[nodiscard]] inline bool is_dangerous_command(std::string_view command) {
    static const std::vector<std::string> dangerous_patterns = {
        "rm -rf",
        "rm -fr",
        "mkfs",
        "dd if",
        ":(){ :|:& };:",
        "chmod -R 777",
        "mv /*",
        "cp -rf /dev/null"
    };
    
    std::string lower_command;
    for (char c : command) {
        lower_command += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    
    for (const auto& pattern : dangerous_patterns) {
        if (lower_command.find(pattern) != std::string::npos) {
            return true;
        }
    }
    
    return false;
}


[[nodiscard]] inline std::string extract_command_name(std::string_view command) {
    auto tokens = tokenize(command);
    if (!tokens.empty()) {
        return tokens[0].value;
    }
    return "";
}

} // namespace cc::utils::bash_parser
