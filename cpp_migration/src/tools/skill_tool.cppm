/// @file skill_tool.cppm
/// @brief SkillTool real implementation: YAML-frontmatter skill loading,
///        path-safety checks, template expansion, context-modifier cascading.
module;

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module cc.tools.skill;

import cc.utils.json;

export namespace cc::tools::skill {

namespace fs = std::filesystem;
namespace json = cc::utils::json;

// ==========================================================================
// Constants
// ==========================================================================

/// Properties that are safe to expose through the `frontmatter_safe` output.
inline constexpr std::array<std::string_view, 9> SAFE_SKILL_PROPERTIES = {
    "name", "version", "description", "author", "tags",
    "license", "icon", "homepage", "changelog",
};

// ==========================================================================
// Supporting types
// ==========================================================================

/// Parsed frontmatter of a skill markdown file.
struct SkillFrontmatter {
    std::optional<std::string> name;
    std::optional<std::string> version;
    std::optional<std::string> description;
    std::optional<std::string> author;
    std::vector<std::string> tags;
    std::optional<std::string> license;
    std::optional<std::string> icon;
    std::optional<std::string> homepage;
    std::optional<std::string> changelog;

    // Execution/context-modifier fields
    std::vector<std::string> allowed_tools;
    std::optional<std::string> model;
    std::optional<int> effort;
    std::optional<int> thinking_budget;
    std::optional<double> temperature;
    std::optional<int> max_tokens;
    std::optional<std::string> fork_reason;

    bool fork{false};
    bool safe{false};
    bool should_use_sandbox{false};

    /// Arbitrary raw keys from the frontmatter that were not specifically
    /// typed above (preserved for filtering and caller inspection).
    std::unordered_map<std::string, std::string> raw_keys;
};

// ==========================================================================
// Small helpers
// ==========================================================================

namespace skill_detail {

[[nodiscard]] std::string trim(std::string_view s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(static_cast<unsigned char>(*start))) ++start;
    auto end = s.end();
    if (start == end) return {};
    --end;
    while (end != start && std::isspace(static_cast<unsigned char>(*end))) --end;
    ++end;
    return std::string(start, end);
}

/// Strip surrounding quotes (single or double) from a value string.
[[nodiscard]] std::string strip_quotes(std::string_view s) {
    std::string out = trim(s);
    if (out.size() >= 2) {
        const char first = out.front();
        const char last = out.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return out.substr(1, out.size() - 2);
        }
    }
    return out;
}

/// Parse a bracketed or comma-separated list: `[a, b, "c"]` or `a, b, c`.
[[nodiscard]] std::vector<std::string> parse_list_value(std::string_view raw) {
    std::vector<std::string> out;
    std::string s = trim(raw);

    // Strip surrounding brackets if present.
    if (!s.empty() && s.front() == '[' && s.back() == ']') {
        s = s.substr(1, s.size() - 2);
    }

    std::string current;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char ch = s[i];
        if (ch == ',') {
            auto piece = strip_quotes(current);
            if (!piece.empty()) out.push_back(std::move(piece));
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    auto piece = strip_quotes(current);
    if (!piece.empty()) out.push_back(std::move(piece));
    return out;
}

[[nodiscard]] std::optional<int> parse_int(std::string_view s) {
    std::string t = trim(s);
    if (t.empty()) return std::nullopt;
    int value = 0;
    const auto* first = t.data();
    const auto* last = t.data() + t.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<double> parse_double(std::string_view s) {
    std::string t = trim(s);
    if (t.empty()) return std::nullopt;
    try {
        size_t pos = 0;
        double v = std::stod(t, &pos);
        while (pos < t.size() && std::isspace(static_cast<unsigned char>(t[pos]))) ++pos;
        if (pos != t.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<bool> parse_bool(std::string_view s) {
    std::string t = trim(s);
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (t == "true" || t == "yes" || t == "y" || t == "1") return true;
    if (t == "false" || t == "no" || t == "n" || t == "0") return false;
    return std::nullopt;
}

/// Escape a string for embedding inside a JSON string literal.
[[nodiscard]] std::string json_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
            case '"':  out += R"(\")"; break;
            case '\\': out += R"(\\)"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            case '\b': out += R"(\b)"; break;
            case '\f': out += R"(\f)"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

[[nodiscard]] std::string json_string_val(std::string_view s) {
    return "\"" + json_escape(s) + "\"";
}

[[nodiscard]] std::string serialize_string_array(const std::vector<std::string>& items) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += ", ";
        out += json_string_val(items[i]);
    }
    out += "]";
    return out;
}

[[nodiscard]] std::string serialize_string_map(const std::unordered_map<std::string, std::string>& m) {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, v] : m) {
        if (!first) out += ", ";
        first = false;
        out += json_string_val(k) + ": " + json_string_val(v);
    }
    out += "}";
    return out;
}

} // namespace skill_detail
using namespace skill_detail;

// ==========================================================================
// Skill root directories + containment check
// ==========================================================================

/// Default list of allowed skill root directories.
[[nodiscard]] std::vector<fs::path> skill_root_dirs() {
    std::vector<fs::path> roots;
    if (const char* home = std::getenv("HOME")) {
        roots.emplace_back(fs::path(home) / ".claude" / "skills");
        roots.emplace_back(fs::path(home) / ".cc-repl" / "skills");
        roots.emplace_back(fs::path(home) / ".codex" / "skills");
        roots.emplace_back(fs::path(home) / ".agents" / "skills");
    }
    roots.emplace_back("/usr/local/share/cc-repl/skills");
    roots.emplace_back("/opt/cc-repl/skills");
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (!ec) roots.emplace_back(cwd / "skills");
    return roots;
}

/// Return true if `candidate` is contained inside `root` (walks upward).
[[nodiscard]] bool is_contained_by(const fs::path& candidate, const fs::path& root) {
    auto p = candidate;
    while (true) {
        std::error_code eq_ec;
        if (fs::equivalent(p, root, eq_ec)) return true;
        if (eq_ec) return false;
        if (p == p.root_path() || p.empty()) return false;
        auto parent = p.parent_path();
        if (parent == p) return false;
        p = parent;
    }
}

[[nodiscard]] bool is_inside_allowed_root(const fs::path& candidate,
                                          const std::vector<fs::path>& roots) {
    for (const auto& root : roots) {
        std::error_code ec;
        auto canon_root = fs::weakly_canonical(root, ec);
        if (ec) continue;
        if (canon_root.empty()) continue;
        if (!fs::exists(canon_root, ec)) { ec.clear(); continue; }
        if (is_contained_by(candidate, canon_root)) return true;
    }
    return false;
}

/// Resolve a user-supplied skill_path to a canonical, contained path.
[[nodiscard]] std::expected<fs::path, std::string> resolve_safe_skill_path(
    std::string_view raw_path,
    const std::vector<fs::path>& roots)
{
    if (raw_path.empty()) return std::unexpected("skill_path is empty");

    // Reject obvious traversal patterns early (fail-fast heuristic).
    const std::string raw(raw_path);
    if (raw.find("/../") != std::string::npos || raw.find("..\\") != std::string::npos) {
        // Allow if after canonicalization it resolves inside roots, but flag.
    }

    fs::path p(raw);
    std::error_code ec;
    if (!p.is_absolute()) {
        bool found = false;
        // Try each root: <root>/<raw>, <root>/<raw>.md, <root>/<raw>/SKILL.md
        for (const auto& root : roots) {
            for (const auto& suffix : {"", ".md", "/SKILL.md"}) {
                auto candidate = root / (std::string(raw_path) + suffix);
                if (fs::exists(candidate, ec)) {
                    ec.clear();
                    p = candidate;
                    found = true;
                    break;
                }
                ec.clear();
            }
            if (found) break;
        }
        if (!found) {
            // As a last resort, try raw relative path.
            p = fs::path(raw);
        }
    }

    if (!fs::exists(p, ec)) return std::unexpected("skill file does not exist");
    if (!fs::is_regular_file(p, ec)) return std::unexpected("skill path is not a regular file");

    auto canon = fs::weakly_canonical(p, ec);
    if (ec) return std::unexpected("cannot canonicalize skill path: " + ec.message());

    if (!is_inside_allowed_root(canon, roots)) {
        return std::unexpected("unsafe path traversal detected: skill path escapes allowed skill roots");
    }
    return canon;
}

// ==========================================================================
// Frontmatter parser (simple YAML-like subset)
// ==========================================================================

[[nodiscard]] std::pair<SkillFrontmatter, std::string> parse_skill_content(std::string_view content) {
    SkillFrontmatter fm;

    if (content.empty()) return {fm, {}};

    // Split into lines first.
    std::vector<std::string_view> lines;
    {
        std::string_view sv = content;
        while (!sv.empty()) {
            auto nl = sv.find('\n');
            if (nl == std::string_view::npos) {
                lines.push_back(sv);
                break;
            }
            lines.push_back(sv.substr(0, nl));
            sv = sv.substr(nl + 1);
        }
    }

    // Find the opening `---` (first non-empty line).
    std::size_t start = 0;
    while (start < lines.size() && trim(lines[start]).empty()) ++start;
    if (start >= lines.size() || trim(lines[start]) != "---") {
        // No frontmatter.
        return {fm, std::string(content)};
    }
    std::size_t opener = start;
    // Find the closing `---`.
    std::size_t closer = std::string_view::npos;
    for (std::size_t i = opener + 1; i < lines.size(); ++i) {
        if (trim(lines[i]) == "---") { closer = i; break; }
    }
    if (closer == std::string_view::npos) {
        // No closing fence: treat as no frontmatter.
        return {fm, std::string(content)};
    }

    // Compute body = bytes after the closer line + newline.
    std::size_t body_start = 0;
    {
        std::string_view sv = content;
        std::size_t line_idx = 0;
        while (!sv.empty() && line_idx <= closer) {
            auto nl = sv.find('\n');
            if (nl == std::string_view::npos) {
                body_start = content.size();
                break;
            }
            body_start += (nl + 1);
            sv = sv.substr(nl + 1);
            ++line_idx;
        }
    }
    std::string body = body_start < content.size()
                           ? std::string(content.substr(body_start))
                           : std::string{};

    // Assign scalar helper.
    std::string current_list_key;
    std::vector<std::string> current_list;
    auto flush_list = [&]() {
        if (current_list_key.empty()) return;
        if (current_list_key == "tags") fm.tags = current_list;
        else if (current_list_key == "allowed_tools") fm.allowed_tools = current_list;
        else fm.raw_keys[current_list_key] = serialize_string_array(current_list);
        current_list_key.clear();
        current_list.clear();
    };
    auto assign_kv = [&](std::string_view key, std::string_view value_raw) {
        const std::string k(key);
        const std::string val = strip_quotes(value_raw);

        if (k == "name")        { fm.name = val; return; }
        if (k == "version")     { fm.version = val; return; }
        if (k == "description") { fm.description = val; return; }
        if (k == "author")      { fm.author = val; return; }
        if (k == "license")     { fm.license = val; return; }
        if (k == "icon")        { fm.icon = val; return; }
        if (k == "homepage")    { fm.homepage = val; return; }
        if (k == "changelog")   { fm.changelog = val; return; }
        if (k == "fork_reason") { fm.fork_reason = val; return; }
        if (k == "model")       { fm.model = val; return; }
        if (k == "tags")        { fm.tags = parse_list_value(value_raw); return; }
        if (k == "allowed_tools") { fm.allowed_tools = parse_list_value(value_raw); return; }
        if (k == "effort")          { if (auto v = parse_int(val)) fm.effort = *v; return; }
        if (k == "thinking_budget") { if (auto v = parse_int(val)) fm.thinking_budget = *v; return; }
        if (k == "max_tokens")      { if (auto v = parse_int(val)) fm.max_tokens = *v; return; }
        if (k == "temperature")     { if (auto v = parse_double(val)) fm.temperature = *v; return; }
        if (k == "fork")            { if (auto v = parse_bool(val)) fm.fork = *v; return; }
        if (k == "safe")            { if (auto v = parse_bool(val)) fm.safe = *v; return; }
        if (k == "should_use_sandbox") { if (auto v = parse_bool(val)) fm.should_use_sandbox = *v; return; }
        if (k == "context_modifiers") {
            fm.raw_keys[k] = val.empty() ? std::string("{object}") : val;
            return;
        }
        fm.raw_keys[k] = val;
    };

    for (std::size_t i = opener + 1; i < closer; ++i) {
        std::string_view l = lines[i];
        auto tl = trim(l);
        if (tl.empty()) { flush_list(); continue; }

        // List item: starts with "  - " (or more indent + dash + space).
        // Detect a leading dash as list continuation.
        {
            std::size_t nsp = 0;
            while (nsp < l.size() && l[nsp] == ' ') ++nsp;
            if (nsp > 0 && nsp < l.size() && l[nsp] == '-') {
                auto after = l.substr(nsp + 1);
                // Require a space after dash to be a list item.
                if (!after.empty() && after.front() == ' ') {
                    auto item = trim(after.substr(1));
                    if (!current_list_key.empty()) {
                        current_list.push_back(strip_quotes(item));
                    }
                    continue;
                }
            }
        }

        // Strip indentation and find key: value.
        // NOTE: trim() returns std::string; we materialize it into std::string
        // to avoid a dangling string_view from the temporary.
        std::size_t nsp = 0;
        while (nsp < l.size() && l[nsp] == ' ') ++nsp;
        std::string_view stripped = l.substr(nsp);
        auto colon = stripped.find(':');
        if (colon == std::string_view::npos) continue;
        std::string key = trim(stripped.substr(0, colon));
        if (key.empty()) continue;
        std::string_view value = colon + 1 < stripped.size()
                                     ? stripped.substr(colon + 1)
                                     : std::string_view{};

        flush_list();

        auto tv = trim(value);
        if (tv.empty()) {
            // Possibly a list-block opener.
            current_list_key = key;
            current_list.clear();
            continue;
        }

        // Bracketed inline list.
        if (tv.front() == '[') {
            auto list = parse_list_value(tv);
            if (key == "tags") fm.tags = std::move(list);
            else if (key == "allowed_tools") fm.allowed_tools = std::move(list);
            else fm.raw_keys[key] = serialize_string_array(list);
            continue;
        }

        assign_kv(key, value);
    }
    flush_list();

    return {fm, body};
}

// ==========================================================================
// Template expansion
// ==========================================================================

using LookupFn = std::function<std::optional<std::string>(std::string_view)>;

[[nodiscard]] std::string expand_template(std::string_view source, const LookupFn& lookup) {
    static const std::regex pattern(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)\})");
    std::string input(source);
    std::string output;
    output.reserve(input.size());

    std::sregex_iterator it(input.begin(), input.end(), pattern);
    std::sregex_iterator end;
    std::size_t last_pos = 0;

    for (; it != end; ++it) {
        const auto& m = *it;
        output.append(input, last_pos, static_cast<std::size_t>(m.position()) - last_pos);
        std::string var_name = m[1].str();
        if (auto replacement = lookup(var_name)) {
            output += *replacement;
        } else {
            output += m.str();
        }
        last_pos = static_cast<std::size_t>(m.position()) + m.length();
    }
    output.append(input, last_pos, std::string::npos);
    return output;
}

// ==========================================================================
// Input parsing
// ==========================================================================

struct SkillToolInput {
    enum class Action { Execute, Install, Update, List, Search };

    Action action{Action::Execute};
    std::string skill_path;
    std::unordered_map<std::string, std::string> arguments;
    std::unordered_map<std::string, std::string> context_modifiers;
    bool fork_model{false};
    std::optional<std::string> session_id;
    std::optional<int> budget_token_limit;
    std::optional<bool> should_use_sandbox;
    std::vector<std::string> allowed_tools;
    std::optional<std::string> model;
    std::optional<int> effort;
};

[[nodiscard]] std::expected<SkillToolInput, std::string> parse_input(std::string_view input_json) {
    SkillToolInput out;
    if (input_json.empty()) return std::unexpected("empty input JSON");

    auto parsed = json::parse(input_json);
    if (!parsed) return std::unexpected("invalid JSON input");
    auto root = parsed->root();
    if (!root.is_obj()) return std::unexpected("input JSON must be an object");

    if (auto a = root.get("action"); a.is_str()) {
        std::string sv(a.as_str());
        std::transform(sv.begin(), sv.end(), sv.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (sv == "install")      out.action = SkillToolInput::Action::Install;
        else if (sv == "update")  out.action = SkillToolInput::Action::Update;
        else if (sv == "list")    out.action = SkillToolInput::Action::List;
        else if (sv == "search")  out.action = SkillToolInput::Action::Search;
        else                      out.action = SkillToolInput::Action::Execute;
    }

    if (auto sp = root.get("skill_path"); sp.is_str()) {
        out.skill_path = std::string(sp.as_str());
    } else if (auto sn = root.get("name"); sn.is_str()) {
        out.skill_path = std::string(sn.as_str());
    } else if (auto sk = root.get("skill"); sk.is_str()) {
        out.skill_path = std::string(sk.as_str());
    }

    if (auto args = root.get("arguments"); args.is_obj()) {
        args.iter_obj([&](json::JsonVal k, json::JsonVal v) {
            if (!k.is_str()) return;
            std::string key(k.as_str());
            if (v.is_str())         out.arguments[key] = std::string(v.as_str());
            else if (v.is_num())    out.arguments[key] = std::to_string(v.as_int());
            else if (v.is_bool())   out.arguments[key] = v.as_bool() ? "true" : "false";
            else                    out.arguments[key] = json::to_string(v);
        });
    }

    if (auto cm = root.get("context_modifiers"); cm.is_obj()) {
        cm.iter_obj([&](json::JsonVal k, json::JsonVal v) {
            if (!k.is_str()) return;
            std::string key(k.as_str());
            if (v.is_str())       out.context_modifiers[key] = std::string(v.as_str());
            else if (v.is_num())  out.context_modifiers[key] = std::to_string(v.as_int());
            else if (v.is_bool()) out.context_modifiers[key] = v.as_bool() ? "true" : "false";
        });
    }

    if (auto fm = root.get("fork_model"); fm.is_bool())
        out.fork_model = fm.as_bool();
    else if (auto fk = root.get("fork"); fk.is_bool())
        out.fork_model = fk.as_bool();

    if (auto sid = root.get("session_id"); sid.is_str())
        out.session_id = std::string(sid.as_str());

    if (auto btl = root.get("budget_token_limit"); btl.is_num())
        out.budget_token_limit = static_cast<int>(btl.as_int());

    if (auto sus = root.get("should_use_sandbox"); sus.is_bool())
        out.should_use_sandbox = sus.as_bool();

    if (auto at = root.get("allowed_tools"); at.is_arr()) {
        at.iter([&](json::JsonVal item) {
            if (item.is_str()) out.allowed_tools.emplace_back(item.as_str());
        });
    }

    if (auto md = root.get("model"); md.is_str())
        out.model = std::string(md.as_str());

    if (auto ef = root.get("effort"); ef.is_num())
        out.effort = static_cast<int>(ef.as_int());

    return out;
}

// ==========================================================================
// Skill catalog: list/search installed skills
// ==========================================================================

/// Scan installed skill roots and serialize the catalog as JSON.
/// When `query` is non-empty, only skills whose name/description/path contain
/// the query (case-insensitive) are included. Mirrors the TS `list`/`search`
/// actions over locally installed skills; the TS remote marketplace search is
/// not bundled in this build.
[[nodiscard]] std::string serialize_skill_catalog(const std::string& query) {
    struct CatalogEntry {
        std::string name;
        std::string description;
        std::string path;
    };

    std::vector<CatalogEntry> entries;
    std::error_code ec;
    auto roots = skill_root_dirs();
    for (const auto& root : roots) {
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;
        for (auto it = fs::directory_iterator(root, ec);
             !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            if (!it->is_directory()) continue;
            auto skill_md = it->path() / "SKILL.md";
            if (!fs::exists(skill_md, ec) || !fs::is_regular_file(skill_md, ec)) continue;
            std::ifstream f(skill_md);
            if (!f) continue;
            std::stringstream ss;
            ss << f.rdbuf();
            auto [fm, body] = parse_skill_content(ss.str());
            (void)body;
            CatalogEntry e;
            e.name = fm.name.value_or(it->path().filename().string());
            e.description = fm.description.value_or("");
            auto canon = fs::weakly_canonical(skill_md, ec);
            e.path = ec ? skill_md.string() : canon.string();
            ec.clear();
            entries.push_back(std::move(e));
        }
        ec.clear();
    }

    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto ci_contains = [&](const std::string& hay) {
        if (q.empty()) return true;
        std::string h = hay;
        std::transform(h.begin(), h.end(), h.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return h.find(q) != std::string::npos;
    };

    std::string out = "{\"ok\": true, \"skills\": [";
    bool first = true;
    std::size_t matched = 0;
    for (const auto& e : entries) {
        if (!(ci_contains(e.name) || ci_contains(e.description) || ci_contains(e.path))) continue;
        if (!first) out += ", ";
        first = false;
        ++matched;
        out += "{\"name\": " + json_string_val(e.name) +
               ", \"description\": " + json_string_val(e.description) +
               ", \"path\": " + json_string_val(e.path) + "}";
    }
    out += "], \"count\": " + std::to_string(matched) + "}";
    return out;
}

// ==========================================================================
// Main entry point: execute_skill_tool_simple
// ==========================================================================

/// Simple (non-looping) skill execution. Performs validation + setup +
/// returns the skill body plus execution plan for the caller to run.
[[nodiscard]] std::expected<std::string, std::string>
execute_skill_tool_simple(std::string_view input_json)
{
    auto input = parse_input(input_json);
    if (!input) return std::unexpected(input.error());

    // List / Search operate over locally installed skills and do not require a
    // skill_path. (The TS CLI's remote marketplace search is not bundled here.)
    if (input->action == SkillToolInput::Action::List) {
        return serialize_skill_catalog(std::string{});
    }
    if (input->action == SkillToolInput::Action::Search) {
        return serialize_skill_catalog(input->skill_path);
    }

    // Install / Update require an external skill source (local directory, git
    // URL, or marketplace) which this build does not bundle.
    if (input->action == SkillToolInput::Action::Install ||
        input->action == SkillToolInput::Action::Update) {
        std::string action_name =
            input->action == SkillToolInput::Action::Install ? "install" : "update";
        std::string msg =
            "The '" + action_name + "' action requires a skill source (local directory, "
            "git URL, or marketplace), which is not available in this build. To add a "
            "skill, place its SKILL.md under a skill root (e.g. "
            "~/.claude/skills/<name>/SKILL.md) and use the 'list' or 'execute' action.";
        return std::string("{\"ok\": false, \"error\": ") + json_string_val(msg) + "}";
    }

    // Execute path: a skill_path is required.
    if (input->skill_path.empty()) {
        return std::unexpected("skill_path is required");
    }

    auto roots = skill_root_dirs();
    auto resolved = resolve_safe_skill_path(input->skill_path, roots);
    if (!resolved) return std::unexpected(resolved.error());

    auto canon_dir = resolved->parent_path();
    std::string canon_dir_str = canon_dir.string();

    std::ifstream file(*resolved);
    if (!file) return std::unexpected("cannot open skill file for reading");
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string raw_content = buffer.str();
    if (raw_content.empty()) return std::unexpected("skill file is empty");

    auto [frontmatter, body] = parse_skill_content(raw_content);

    std::string arguments_json = serialize_string_map(input->arguments);

    LookupFn lookup = [&](std::string_view name) -> std::optional<std::string> {
        if (name == "ARGUMENTS") return arguments_json;
        if (name == "CLAUDE_SKILL_DIR") return canon_dir_str;
        if (name == "CLAUDE_SESSION_ID") {
            if (input->session_id) return *input->session_id;
            return std::nullopt;
        }
        auto it = input->arguments.find(std::string(name));
        if (it != input->arguments.end()) return it->second;
        return std::nullopt;
    };

    std::string expanded_body = expand_template(body, lookup);

    // --- Context modifier cascading ---------------------------------------

    std::string resolved_model = input->model ? *input->model
                                 : frontmatter.model ? *frontmatter.model
                                 : std::string{};

    int resolved_effort = 0;
    {
        auto it = input->context_modifiers.find("effort");
        if (it != input->context_modifiers.end()) {
            if (auto v = parse_int(it->second)) resolved_effort = *v;
        } else if (input->effort) {
            resolved_effort = *input->effort;
        } else if (frontmatter.effort) {
            resolved_effort = *frontmatter.effort;
        } else {
            // Check frontmatter context_modifiers via raw_keys fallback.
            // (The simple parser doesn't nest; nested fields under
            // context_modifiers are captured individually by key.)
            auto it2 = input->context_modifiers.find("effort");
            (void)it2; // handled above
        }
    }
    if (resolved_effort > 0) resolved_effort = std::clamp(resolved_effort, 1, 5);

    std::vector<std::string> resolved_allowed_tools = input->allowed_tools.empty()
                                                          ? frontmatter.allowed_tools
                                                          : input->allowed_tools;

    bool resolved_sandbox = input->should_use_sandbox.value_or(frontmatter.should_use_sandbox);
    const bool fork_required = frontmatter.fork || input->fork_model;

    // --- Build execution_plan JSON (typed via JsonMutDoc) -----------------
    std::string plan_json = [&]() {
        json::JsonMutDoc doc;
        json::JsonMutVal root = doc.object();
        doc.set_root(root);

        json::JsonMutVal steps = doc.array();
        steps.append(doc.string("Parse: " + resolved->string()));
        steps.append(doc.string("Apply context modifiers"));
        steps.append(doc.string("Run skill workflow"));
        root.add("steps", steps);

        if (!resolved_model.empty())
            root.add("model_override", doc.string(resolved_model));
        if (resolved_effort > 0)
            root.add("effort", doc.number(static_cast<int64_t>(resolved_effort)));

        {
            json::JsonMutVal tools = doc.array();
            for (const auto& t : resolved_allowed_tools)
                tools.append(doc.string(t));
            root.add("allowed_tools", tools);
        }

        if (fork_required) {
            root.add("fork_context_required", doc.boolean(true));
            if (frontmatter.fork_reason)
                root.add("fork_reason", doc.string(*frontmatter.fork_reason));
        }
        return doc.to_string();
    }();

    // --- Build outer response JSON ----------------------------------------
    json::JsonMutDoc doc;
    json::JsonMutVal root = doc.object();
    doc.set_root(root);
    root.add("ok", doc.boolean(true));

    // skill sub-object
    {
        json::JsonMutVal sd = doc.object();
        std::string nm = frontmatter.name.value_or(resolved->stem().string());
        sd.add("name", doc.string(nm));
        if (frontmatter.version)     sd.add("version", doc.string(*frontmatter.version));
        if (frontmatter.description) sd.add("description", doc.string(*frontmatter.description));
        root.add("skill", sd);
    }

    // execution_plan (parse the pre-built plan text and embed as typed object)
    {
        auto pp = json::parse(plan_json);
        if (pp && pp->root().is_obj())
            root.add("execution_plan", doc.copy_val(pp->root()));
    }

    // expanded_args
    {
        json::JsonMutVal ea = doc.object();
        for (const auto& [k, v] : input->arguments) ea.add(k, doc.string(v));
        root.add("expanded_args", ea);
    }

    // frontmatter_safe — filtered to SAFE_SKILL_PROPERTIES only
    {
        json::JsonMutVal sf = doc.object();
        static const std::unordered_set<std::string_view> safe_set(
            SAFE_SKILL_PROPERTIES.begin(), SAFE_SKILL_PROPERTIES.end());
        if (safe_set.count("name") && frontmatter.name)
            sf.add("name", doc.string(*frontmatter.name));
        if (safe_set.count("version") && frontmatter.version)
            sf.add("version", doc.string(*frontmatter.version));
        if (safe_set.count("description") && frontmatter.description)
            sf.add("description", doc.string(*frontmatter.description));
        if (safe_set.count("author") && frontmatter.author)
            sf.add("author", doc.string(*frontmatter.author));
        if (safe_set.count("license") && frontmatter.license)
            sf.add("license", doc.string(*frontmatter.license));
        if (safe_set.count("icon") && frontmatter.icon)
            sf.add("icon", doc.string(*frontmatter.icon));
        if (safe_set.count("homepage") && frontmatter.homepage)
            sf.add("homepage", doc.string(*frontmatter.homepage));
        if (safe_set.count("changelog") && frontmatter.changelog)
            sf.add("changelog", doc.string(*frontmatter.changelog));
        if (!frontmatter.tags.empty()) {
            json::JsonMutVal tags = doc.array();
            for (const auto& t : frontmatter.tags) tags.append(doc.string(t));
            sf.add("tags", tags);
        }
        root.add("frontmatter_safe", sf);
    }

    root.add("inline_skill_body", doc.string(expanded_body));

    if (resolved_sandbox) root.add("should_use_sandbox", doc.boolean(true));
    if (input->budget_token_limit)
        root.add("budget_token_limit", doc.number(static_cast<int64_t>(*input->budget_token_limit)));

    return doc.to_string();
}

} // namespace cc::tools::skill
