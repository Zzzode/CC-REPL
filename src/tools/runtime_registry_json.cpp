// Implementation unit for cc.tools.runtime_registry — ad-hoc JSON field
// accessors, notebook/browser action parsing, and small text helpers. Separate
// from the executor/dispatch/register units so a body edit to one helper
// recompiles this smaller object only.
module;

#include <cctype> // std::tolower / std::isspace / std::isalnum

module cc.tools.runtime_registry;

import std;

import cc.utils.json;
import cc.utils.parse_int;
import cc.tools.notebook;
import cc.tools.web_browser;

namespace cc::tools::detail {

namespace fs = std::filesystem;

// Ad-hoc JSON field accessors over a raw JSON string. These now delegate to
// cc.utils.json (parse once, then typed access) instead of hand-written byte
// scanning — the scanner was obfuscation-prone (it matched the first "\"key\""
// substring anywhere, including inside string values) and is eliminated as
// part of the JSON-consolidation work (audit §13 #3). Signatures/semantics are
// preserved so the ~40 call sites are unchanged.

[[nodiscard]] std::optional<std::string> json_string(std::string_view json, std::string_view key) {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return std::nullopt;
    auto val = parsed->root().get(key);
    if (!val.is_str()) return std::nullopt;
    return std::string(val.as_str());
}

[[nodiscard]] std::optional<int> json_int(std::string_view json, std::string_view key) {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return std::nullopt;
    auto val = parsed->root().get(key);
    if (!val.is_num()) return std::nullopt;
    return static_cast<int>(val.as_int());
}

[[nodiscard]] bool json_bool(std::string_view json, std::string_view key, bool fallback) {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed) return fallback;
    auto val = parsed->root().get(key);
    if (!val.is_bool()) return fallback;
    return val.as_bool();
}

[[nodiscard]] std::optional<std::string> runtime_json_string(cc::utils::json::JsonVal obj, std::string_view key) {
    auto val = obj.get(key);
    if (!val.is_str()) return std::nullopt;
    return std::string(val.as_str());
}

[[nodiscard]] std::optional<int> runtime_json_int(cc::utils::json::JsonVal obj, std::string_view key) {
    auto val = obj.get(key);
    if (!val.is_num()) return std::nullopt;
    return static_cast<int>(val.as_int());
}

[[nodiscard]] std::optional<bool> runtime_json_bool(cc::utils::json::JsonVal obj, std::string_view key) {
    auto val = obj.get(key);
    if (!val.is_bool()) return std::nullopt;
    return val.as_bool();
}

[[nodiscard]] std::optional<bool> runtime_json_semantic_bool(
    cc::utils::json::JsonVal obj,
    std::string_view key
) {
    auto val = obj.get(key);
    if (val.is_bool()) return val.as_bool();
    if (!val.is_str()) return std::nullopt;
    std::string normalized(val.as_str());
    std::ranges::transform(normalized, normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (normalized == "true" || normalized == "yes" || normalized == "y" ||
        normalized == "1" || normalized == "approve" || normalized == "approved") {
        return true;
    }
    if (normalized == "false" || normalized == "no" || normalized == "n" ||
        normalized == "0" || normalized == "reject" || normalized == "rejected" ||
        normalized == "deny" || normalized == "denied") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::string> runtime_json_event_array(cc::utils::json::JsonVal obj, std::string_view key) {
    std::vector<std::string> values;
    auto node = obj.get(key);
    if (!node.is_arr()) return values;
    node.iter([&](cc::utils::json::JsonVal item) {
        if (item.is_str()) {
            values.emplace_back(item.as_str());
        } else if (item.valid()) {
            values.push_back(cc::utils::json::to_string(item));
        }
    });
    return values;
}

[[nodiscard]] std::vector<std::string> json_string_array(std::string_view json, std::string_view key) {
    std::vector<std::string> values;
    auto parsed = cc::utils::json::parse(json);
    if (!parsed || !parsed->root().is_obj()) return values;
    auto node = parsed->root().get(key);
    if (node.is_arr()) {
        node.iter([&](cc::utils::json::JsonVal item) {
            if (item.is_str()) values.emplace_back(item.as_str());
        });
        return values;
    }
    if (node.is_str()) {
        std::string text(node.as_str());
        std::string current;
        for (char ch : text) {
            if (ch == '+' || ch == ',') {
                if (!current.empty()) values.push_back(std::exchange(current, {}));
            } else if (!std::isspace(static_cast<unsigned char>(ch))) {
                current.push_back(ch);
            }
        }
        if (!current.empty()) values.push_back(std::move(current));
    }
    return values;
}

[[nodiscard]] std::optional<std::string> json_raw_value(std::string_view json, std::string_view key) {
    auto key_text = std::format("\"{}\"", key);
    auto key_pos = json.find(key_text);
    if (key_pos == std::string_view::npos) return std::nullopt;
    auto colon = json.find(':', key_pos + key_text.size());
    if (colon == std::string_view::npos) return std::nullopt;
    auto pos = colon + 1;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size()) return std::nullopt;

    const auto start = pos;
    if (json[pos] == '{' || json[pos] == '[') {
        const char open = json[pos];
        const char close = open == '{' ? '}' : ']';
        int depth = 0;
        bool in_string = false;
        bool escaping = false;
        while (pos < json.size()) {
            const char c = json[pos];
            if (in_string) {
                if (escaping) escaping = false;
                else if (c == '\\') escaping = true;
                else if (c == '"') in_string = false;
            } else if (c == '"') {
                in_string = true;
            } else if (c == open) {
                ++depth;
            } else if (c == close) {
                if (--depth == 0) {
                    return std::string(json.substr(start, pos - start + 1));
                }
            }
            ++pos;
        }
    return std::nullopt;
}

    if (json[pos] == '"') {
        ++pos;
        bool escaping = false;
        while (pos < json.size()) {
            const char c = json[pos];
            if (escaping) escaping = false;
            else if (c == '\\') escaping = true;
            else if (c == '"') return std::string(json.substr(start, pos - start + 1));
            ++pos;
        }
        return std::nullopt;
    }

    while (pos < json.size() && json[pos] != ',' && json[pos] != '}') ++pos;
    while (pos > start && std::isspace(static_cast<unsigned char>(json[pos - 1]))) --pos;
    return std::string(json.substr(start, pos - start));
}

[[nodiscard]] std::optional<std::size_t> parse_notebook_cell_index(std::string_view text) {
    if (text.starts_with("cell-")) {
        text.remove_prefix(5);
    }
    if (text.empty()) return std::nullopt;
    std::size_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    auto [ptr, ec] = cc::utils::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<std::size_t> resolve_notebook_cell_index(
    const Notebook& notebook,
    std::optional<std::string> cell_id
) {
    if (!cell_id || cell_id->empty()) return std::nullopt;
    for (std::size_t i = 0; i < notebook.cells.size(); ++i) {
        if (notebook.cells[i].id && *notebook.cells[i].id == *cell_id) {
            return i;
        }
    }
    return parse_notebook_cell_index(*cell_id);
}

[[nodiscard]] std::optional<CellOperation> parse_notebook_operation(std::string_view text) {
    if (text == "insert") return CellOperation::Insert;
    if (text == "delete") return CellOperation::Delete;
    if (text == "update" || text == "replace") return CellOperation::Update;
    if (text == "move") return CellOperation::Move;
    return std::nullopt;
}

[[nodiscard]] std::string join_args(const std::vector<std::string>& args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) out += " ";
        out += args[i];
    }
    return out;
}

[[nodiscard]] std::vector<std::string> read_lines(const fs::path& file) {
    std::ifstream input(file);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    return lines;
}

[[nodiscard]] std::string word_at_position(const std::vector<std::string>& lines, int line_no, int character) {
    if (line_no < 0 || static_cast<std::size_t>(line_no) >= lines.size()) return {};
    const auto& line = lines[static_cast<std::size_t>(line_no)];
    auto pos = std::clamp(character, 0, static_cast<int>(line.size()));
    auto is_word = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    int begin = pos;
    while (begin > 0 && is_word(line[static_cast<std::size_t>(begin - 1)])) --begin;
    int end = pos;
    while (end < static_cast<int>(line.size()) && is_word(line[static_cast<std::size_t>(end)])) ++end;
    if (end <= begin) return {};
    return line.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin));
}

[[nodiscard]] bool is_source_file(const fs::path& path) {
    auto ext = path.extension().string();
    static const std::vector<std::string> exts = {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh",
        ".m", ".mm", ".ts", ".tsx", ".js", ".jsx", ".py",
        ".rs", ".go", ".java", ".kt", ".swift", ".cppm"
    };
    return std::ranges::find(exts, ext) != exts.end();
}

[[nodiscard]] std::optional<cc::tools::BrowserAction> parse_browser_action(std::string_view action) {
    using cc::tools::BrowserAction;
    if (action == "navigate") return BrowserAction::Navigate;
    if (action == "click") return BrowserAction::Click;
    if (action == "extract") return BrowserAction::Extract;
    if (action == "screenshot") return BrowserAction::Screenshot;
    if (action == "fill_form") return BrowserAction::FillForm;
    if (action == "get_title") return BrowserAction::GetTitle;
    return std::nullopt;
}

[[nodiscard]] std::vector<cc::tools::FormField> json_form_fields(std::string_view json) {
    std::vector<cc::tools::FormField> fields;
    auto parsed = cc::utils::json::parse(json);
    if (!parsed || !parsed->root().is_obj()) return fields;

    auto node = parsed->root().get("form_fields");
    if (!node.valid() || !node.is_arr()) {
        node = parsed->root().get("fields");
    }
    if (!node.valid() || !node.is_arr()) return fields;

    node.iter([&](cc::utils::json::JsonVal item) {
        if (!item.is_obj()) return;
        auto selector = runtime_json_string(item, "selector");
        auto value = runtime_json_string(item, "value");
        if (!selector || selector->empty() || !value) return;
        fields.push_back(cc::tools::FormField{
            .selector = std::move(*selector),
            .value = std::move(*value),
        });
    });
    return fields;
}

} // namespace cc::tools::detail
