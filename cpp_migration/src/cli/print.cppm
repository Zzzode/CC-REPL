module;
#include <string>
#include <string_view>
#include <vector>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>

export module cc.cli.print;

export namespace cc::cli {

// ANSI color codes for terminal output
namespace colors {
    inline constexpr const char* RESET = "\033[0m";
    inline constexpr const char* RED = "\033[31m";
    inline constexpr const char* GREEN = "\033[32m";
    inline constexpr const char* YELLOW = "\033[33m";
    inline constexpr const char* BLUE = "\033[34m";
    inline constexpr const char* BOLD = "\033[1m";
    inline constexpr const char* DIM = "\033[2m";
}

// Print a standard message to stdout
void print_message(std::string_view msg) {
    std::cout << msg << '\n';
}

// Print an error message in red to stderr
void print_error(std::string_view msg) {
    std::cerr << colors::RED << colors::BOLD << "Error: " << colors::RESET
              << colors::RED << msg << colors::RESET << '\n';
}

// Print a warning message in yellow to stderr
void print_warning(std::string_view msg) {
    std::cerr << colors::YELLOW << "Warning: " << msg << colors::RESET << '\n';
}

// Print a success message in green to stdout
void print_success(std::string_view msg) {
    std::cout << colors::GREEN << "✓ " << msg << colors::RESET << '\n';
}

// Print formatted JSON with optional pretty-printing
void print_json(std::string_view json, bool pretty) {
    if (!pretty) {
        std::cout << json << '\n';
        return;
    }

    // Simple JSON pretty-printer with indentation
    int indent = 0;
    bool in_string = false;
    bool escaped = false;

    for (char c : json) {
        if (escaped) {
            std::cout << c;
            escaped = false;
            continue;
        }

        if (c == '\\' && in_string) {
            std::cout << c;
            escaped = true;
            continue;
        }

        if (c == '"') {
            in_string = !in_string;
            std::cout << c;
            continue;
        }

        if (in_string) {
            std::cout << c;
            continue;
        }

        switch (c) {
            case '{':
            case '[':
                std::cout << c << '\n';
                ++indent;
                std::cout << std::string(indent * 2, ' ');
                break;
            case '}':
            case ']':
                std::cout << '\n';
                --indent;
                std::cout << std::string(indent * 2, ' ') << c;
                break;
            case ',':
                std::cout << c << '\n' << std::string(indent * 2, ' ');
                break;
            case ':':
                std::cout << ": ";
                break;
            default:
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                    std::cout << c;
                }
                break;
        }
    }
    std::cout << '\n';
}

// Print a formatted table with headers and rows
void print_table(std::vector<std::vector<std::string>> rows, std::vector<std::string> headers) {
    if (headers.empty()) return;

    size_t num_cols = headers.size();

    // Calculate column widths (max of header and all row values)
    std::vector<size_t> widths(num_cols, 0);
    for (size_t i = 0; i < num_cols; ++i) {
        widths[i] = headers[i].size();
    }
    for (const auto& row : rows) {
        for (size_t i = 0; i < std::min(num_cols, row.size()); ++i) {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }

    // Print header row
    std::cout << colors::BOLD;
    for (size_t i = 0; i < num_cols; ++i) {
        std::cout << std::left << std::setw(static_cast<int>(widths[i] + 2)) << headers[i];
    }
    std::cout << colors::RESET << '\n';

    // Print separator
    for (size_t i = 0; i < num_cols; ++i) {
        std::cout << std::string(widths[i], '-') << "  ";
    }
    std::cout << '\n';

    // Print data rows
    for (const auto& row : rows) {
        for (size_t i = 0; i < num_cols; ++i) {
            std::string value = (i < row.size()) ? row[i] : "";
            std::cout << std::left << std::setw(static_cast<int>(widths[i] + 2)) << value;
        }
        std::cout << '\n';
    }
}

} // namespace cc::cli
