// C++23 Module: Code indexing for semantic search
// 代码索引模块：基于关键词和正则的符号提取与搜索
module;
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.utils.code_indexing;


export namespace cc::utils::code_indexing {

// 符号类型枚举
enum class SymbolKind : uint8_t {
    Function,
    Class,
    Method,
    Variable,
    Type,
    Module
};

// 代码符号结构
struct CodeSymbol {
    std::string name;
    SymbolKind kind;
    std::filesystem::path file;
    size_t line{0};
    std::string signature;  // 完整签名 (如函数声明)

    // 格式化显示
    [[nodiscard]] std::string display() const {
        return std::format("{}:{} [{}] {}", file.string(), line, kind_name(), signature);
    }

private:
    [[nodiscard]] std::string_view kind_name() const {
        switch (kind) {
            case SymbolKind::Function: return "func";
            case SymbolKind::Class:    return "class";
            case SymbolKind::Method:   return "method";
            case SymbolKind::Variable: return "var";
            case SymbolKind::Type:     return "type";
            case SymbolKind::Module:   return "module";
        }
        return "unknown";
    }
};

// 语言识别
enum class Language : uint8_t {
    Cpp, Python, JavaScript, TypeScript, Go, Rust, Java, Unknown
};

// 根据文件扩展名判断语言
[[nodiscard]] inline Language detect_language(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    if (ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".cc" || ext == ".cxx")
        return Language::Cpp;
    if (ext == ".py") return Language::Python;
    if (ext == ".js" || ext == ".mjs") return Language::JavaScript;
    if (ext == ".ts" || ext == ".tsx") return Language::TypeScript;
    if (ext == ".go") return Language::Go;
    if (ext == ".rs") return Language::Rust;
    if (ext == ".java") return Language::Java;
    return Language::Unknown;
}

// 简单正则模式匹配器 (每种语言定义不同的模式)
struct LanguagePattern {
    std::string pattern;
    SymbolKind kind;
};

// 获取语言对应的符号提取模式
[[nodiscard]] inline std::vector<LanguagePattern> get_patterns(Language lang) {
    switch (lang) {
        case Language::Cpp:
            return {
                {R"(^\s*(?:class|struct)\s+(\w+))", SymbolKind::Class},
                {R"(^\s*(?:[\w:]+\s+)+(\w+)\s*\([^)]*\)\s*(?:const)?\s*\{)", SymbolKind::Function},
                {R"(^\s*(?:export\s+)?module\s+([\w.]+))", SymbolKind::Module},
                {R"(^\s*(?:using|typedef)\s+.*\s+(\w+)\s*[;=])", SymbolKind::Type},
                {R"(^\s*(?:enum\s+(?:class\s+)?)(\w+))", SymbolKind::Type},
            };
        case Language::Python:
            return {
                {R"(^\s*class\s+(\w+))", SymbolKind::Class},
                {R"(^\s*def\s+(\w+))", SymbolKind::Function},
                {R"(^\s*(\w+)\s*=)", SymbolKind::Variable},
            };
        case Language::JavaScript:
        case Language::TypeScript:
            return {
                {R"(^\s*(?:export\s+)?class\s+(\w+))", SymbolKind::Class},
                {R"(^\s*(?:export\s+)?(?:async\s+)?function\s+(\w+))", SymbolKind::Function},
                {R"(^\s*(?:export\s+)?(?:const|let|var)\s+(\w+))", SymbolKind::Variable},
                {R"(^\s*(?:export\s+)?(?:type|interface)\s+(\w+))", SymbolKind::Type},
            };
        case Language::Go:
            return {
                {R"(^func\s+(?:\(\w+\s+\*?\w+\)\s+)?(\w+))", SymbolKind::Function},
                {R"(^type\s+(\w+)\s+struct)", SymbolKind::Class},
                {R"(^type\s+(\w+)\s+interface)", SymbolKind::Type},
            };
        case Language::Rust:
            return {
                {R"(^\s*(?:pub\s+)?fn\s+(\w+))", SymbolKind::Function},
                {R"(^\s*(?:pub\s+)?struct\s+(\w+))", SymbolKind::Class},
                {R"(^\s*(?:pub\s+)?enum\s+(\w+))", SymbolKind::Type},
                {R"(^\s*(?:pub\s+)?trait\s+(\w+))", SymbolKind::Type},
                {R"(^\s*mod\s+(\w+))", SymbolKind::Module},
            };
        case Language::Java:
            return {
                {R"(^\s*(?:public|private|protected)?\s*class\s+(\w+))", SymbolKind::Class},
                {R"(^\s*(?:public|private|protected)?\s*(?:static\s+)?[\w<>]+\s+(\w+)\s*\()", SymbolKind::Function},
                {R"(^\s*(?:public|private|protected)?\s*interface\s+(\w+))", SymbolKind::Type},
            };
        default:
            return {};
    }
}

// 代码索引类
class CodeIndex {
public:
    CodeIndex() = default;

    // 索引单个文件，返回提取到的符号
    [[nodiscard]] std::vector<CodeSymbol> index_file(const std::filesystem::path& path) {
        std::vector<CodeSymbol> symbols;
        auto lang = detect_language(path);
        if (lang == Language::Unknown) return symbols;

        std::ifstream file(path);
        if (!file) return symbols;

        auto patterns = get_patterns(lang);
        std::string line;
        size_t line_num = 0;

        while (std::getline(file, line)) {
            ++line_num;
            for (const auto& [pattern_str, kind] : patterns) {
                std::regex pattern(pattern_str);
                std::smatch match;
                if (std::regex_search(line, match, pattern) && match.size() > 1) {
                    CodeSymbol sym;
                    sym.name = match[1].str();
                    sym.kind = kind;
                    sym.file = path;
                    sym.line = line_num;
                    // 清理签名行
                    sym.signature = trim(line);
                    symbols.push_back(std::move(sym));
                    break;  // 每行只取第一个匹配
                }
            }
        }

        // 更新索引数据库
        auto path_str = path.string();
        index_[path_str] = symbols;
        file_mtimes_[path_str] = std::filesystem::last_write_time(path);

        return symbols;
    }

    // 搜索符号 (模糊匹配)
    [[nodiscard]] std::vector<CodeSymbol> search(std::string_view query) const {
        std::vector<CodeSymbol> results;
        auto lower_query = to_lower(query);

        for (const auto& [file, symbols] : index_) {
            for (const auto& sym : symbols) {
                auto lower_name = to_lower(sym.name);
                // 前缀匹配或子串匹配
                if (lower_name.find(lower_query) != std::string::npos) {
                    results.push_back(sym);
                }
            }
        }

        // 按匹配质量排序 (精确匹配优先)
        std::ranges::sort(results, [&](const auto& a, const auto& b) {
            auto la = to_lower(a.name);
            auto lb = to_lower(b.name);
            bool a_exact = (la == lower_query);
            bool b_exact = (lb == lower_query);
            if (a_exact != b_exact) return a_exact;
            return la.size() < lb.size();
        });

        return results;
    }

    // 获取符号定义位置
    [[nodiscard]] std::vector<CodeSymbol> get_definitions(std::string_view name) const {
        std::vector<CodeSymbol> results;
        for (const auto& [file, symbols] : index_) {
            for (const auto& sym : symbols) {
                if (sym.name == name) {
                    results.push_back(sym);
                }
            }
        }
        return results;
    }

    // 刷新索引：重新索引已修改的文件
    size_t refresh() {
        size_t refreshed = 0;
        std::vector<std::string> stale_files;

        for (const auto& [path_str, mtime] : file_mtimes_) {
            std::filesystem::path path(path_str);
            std::error_code ec;
            auto current_mtime = std::filesystem::last_write_time(path, ec);
            if (ec) {
                // 文件已被删除
                stale_files.push_back(path_str);
                continue;
            }
            if (current_mtime != mtime) {
                index_file(path);
                ++refreshed;
            }
        }

        // 清理已删除文件
        for (const auto& stale : stale_files) {
            index_.erase(stale);
            file_mtimes_.erase(stale);
        }

        return refreshed;
    }

    // 获取索引统计
    [[nodiscard]] size_t file_count() const { return index_.size(); }
    [[nodiscard]] size_t symbol_count() const {
        size_t count = 0;
        for (const auto& [_, symbols] : index_) { count += symbols.size(); }
        return count;
    }

private:
    // 文件路径 -> 符号列表
    std::unordered_map<std::string, std::vector<CodeSymbol>> index_;
    // 文件路径 -> 修改时间
    std::unordered_map<std::string, std::filesystem::file_time_type> file_mtimes_;

    [[nodiscard]] static std::string to_lower(std::string_view sv) {
        std::string result(sv);
        std::ranges::transform(result, result.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        return result;
    }

    [[nodiscard]] static std::string trim(std::string_view sv) {
        auto start = sv.find_first_not_of(" \t");
        if (start == std::string_view::npos) return "";
        auto end = sv.find_last_not_of(" \t\n\r");
        return std::string(sv.substr(start, end - start + 1));
    }
};

} // namespace cc::utils::code_indexing
