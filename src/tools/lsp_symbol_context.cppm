module;
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <optional>
#include <fstream>
#include <cctype>

export module cc.tools.lsp_symbol_context;

export namespace cc::tools {


struct SymbolContext {
    std::string name;
    std::string kind;
    std::filesystem::path file;
    int line = 0;
    std::optional<std::string> container;
};



inline auto get_symbol_at_position(
    const std::filesystem::path& file,
    int line,
    int column
) -> std::optional<SymbolContext> {
    namespace fs = std::filesystem;

    if (!fs::exists(file)) {
        return std::nullopt;
    }

    if (line < 0 || column < 0) {
        return std::nullopt;
    }

    std::ifstream in(file);
    if (!in) return std::nullopt;
    std::string text_line;
    for (int current = 0; current <= line && std::getline(in, text_line); ++current) {
        if (current != line) continue;
        if (column >= static_cast<int>(text_line.size())) return std::nullopt;
        auto is_ident = [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; };
        int begin = column;
        while (begin > 0 && is_ident(static_cast<unsigned char>(text_line[begin - 1]))) --begin;
        int end = column;
        while (end < static_cast<int>(text_line.size()) && is_ident(static_cast<unsigned char>(text_line[end]))) ++end;
        if (begin == end) return std::nullopt;
        auto name = text_line.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin));
        return SymbolContext{
            .name = name,
            .kind = "Identifier",
            .file = file,
            .line = line,
            .container = std::nullopt
        };
    }
    return std::nullopt;
}



inline auto find_references(const SymbolContext& symbol) -> std::vector<SymbolContext> {
    std::vector<SymbolContext> references;
    if (symbol.name.empty() || symbol.file.empty()) return references;
    std::ifstream in(symbol.file);
    if (!in) return references;
    std::string line_text;
    int line_no = 0;
    while (std::getline(in, line_text)) {
        if (line_text.find(symbol.name) != std::string::npos) {
            references.push_back(SymbolContext{.name = symbol.name, .kind = symbol.kind, .file = symbol.file, .line = line_no, .container = symbol.container});
        }
        ++line_no;
    }

    return references;
}



inline auto get_definition(const SymbolContext& symbol) -> std::optional<SymbolContext> {

    if (!symbol.name.empty() && !symbol.file.empty()) {

        return symbol;
    }

    return std::nullopt;
}


inline auto normalize_symbol_kind(std::string_view kind) -> std::string_view {

    if (kind == "1")  return "File";
    if (kind == "2")  return "Module";
    if (kind == "3")  return "Namespace";
    if (kind == "4")  return "Package";
    if (kind == "5")  return "Class";
    if (kind == "6")  return "Method";
    if (kind == "7")  return "Property";
    if (kind == "8")  return "Field";
    if (kind == "9")  return "Constructor";
    if (kind == "10") return "Enum";
    if (kind == "11") return "Interface";
    if (kind == "12") return "Function";
    if (kind == "13") return "Variable";
    if (kind == "14") return "Constant";
    return kind;
}

} // namespace cc::tools
