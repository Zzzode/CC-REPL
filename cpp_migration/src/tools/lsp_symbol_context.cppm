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

// 符号上下文信息
struct SymbolContext {
    std::string name;                    // 符号名称
    std::string kind;                    // 符号类型（function, class, variable, etc.）
    std::filesystem::path file;          // 所在文件路径
    int line = 0;                        // 所在行号
    std::optional<std::string> container; // 所属容器（类名、模块名等）
};

// 获取指定位置的符号上下文
// 注：实际实现需要与 LSP 服务器通信
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
        return SymbolContext{.name = name, .kind = "Identifier", .file = file, .line = line};
    }
    return std::nullopt;
}

// 查找符号的所有引用
// 注：实际实现需要与 LSP 服务器通信
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

// 获取符号的定义位置
// 注：实际实现需要与 LSP 服务器通信
inline auto get_definition(const SymbolContext& symbol) -> std::optional<SymbolContext> {
    // 如果符号本身就是定义，直接返回
    if (!symbol.name.empty() && !symbol.file.empty()) {
        // 简单情况：符号本身可能就是定义
        return symbol;
    }

    return std::nullopt;
}

// 辅助函数：将符号类型字符串规范化
inline auto normalize_symbol_kind(std::string_view kind) -> std::string_view {
    // LSP SymbolKind 数值到字符串的映射
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
