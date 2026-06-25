// Interactive REPL input parsing, slash-command routing, paste sanitization
// and at-mention resolution helpers.  These are intentionally dependency-light
// so the input loop can stay snappy on the render thread.
module;

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.bootstrap.interactive;

import cc.hooks.ide_at_mentioned;
import cc.utils.json;

export namespace cc::bootstrap::interactive {

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

enum class CommandKind : uint8_t {
    SlashCommand,
    BashCommand,
    TextPrompt,
    AtMentionOnly,
    Paste,
    Empty,
    Unknown,
};

struct ParsedCommand {
    CommandKind kind = CommandKind::Unknown;
    std::string raw;
    std::string normalized;
    std::string slash_name;
    std::vector<std::string> slash_args;
    std::string bash_command_line;
    std::vector<std::string> at_mentions;
    std::vector<std::pair<size_t, size_t>> at_mention_ranges;
    size_t paste_size_bytes = 0;
    enum class PasteKind {
        Unknown,
        Code,
        Json,
        Markdown,
        Log,
        Diff,
    } paste_kind = PasteKind::Unknown;
};

struct InteractiveContext {
    std::string prompt_state;
    std::string queued_commands_json;
    std::string clipboard_last_text;
    int64_t last_paste_ts_ms = 0;
    std::vector<std::string> at_mention_index;
    std::function<std::vector<std::string>(std::string_view needle)> at_resolver;
    std::chrono::milliseconds paste_threshold_interval{300};
    size_t paste_min_chars = 400;
    bool dollar_is_bash = false;
};

struct DispatchOutcome {
    enum class Action {
        RouteToCommand,
        RouteToBash,
        RouteToPrompt,
        RouteToAtMention,
        AskConfirmation,
        RejectWithReason,
        PromptForInput,
    } action = Action::RejectWithReason;
    std::string target;
    std::string payload_json;
    std::string reason;
    std::string confirmation_prompt;
    std::string input_prompt_label;
};

using SlashHandler = std::function<DispatchOutcome(
    const std::vector<std::string>& args, const InteractiveContext& ctx)>;

// ---------------------------------------------------------------------------
// Local helpers (internal)
// ---------------------------------------------------------------------------

namespace detail {

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

inline std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Quote-aware split used for slash-command args.  Supports "..." and '...'
// with nesting-level toggling per quote character.
inline std::vector<std::string> quote_aware_split(std::string_view s) {
    std::vector<std::string> tokens;
    std::string cur;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else {
                cur.push_back(c);
            }
        } else {
            if (c == '"' || c == '\'') {
                quote = c;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty() || tokens.empty() ? false : true) {
                    // flush token
                }
                if (!cur.empty()) {
                    tokens.push_back(std::move(cur));
                    cur.clear();
                }
            } else {
                cur.push_back(c);
            }
        }
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
    return tokens;
}

inline bool is_hex(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) ||
           (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace detail

// ---------------------------------------------------------------------------
// Paste sanitization + detection
// ---------------------------------------------------------------------------

namespace interactive_detail {
inline size_t g_paste_limit = 50 * 1024;
} // namespace interactive_detail
using namespace interactive_detail;

inline void set_paste_size_limit(size_t max_chars) {
    g_paste_limit = max_chars;
}

inline std::string sanitize_paste(std::string text) {
    // 1) Strip ANSI CSI sequences: \x1b [ <digits and semicolons> [mK]
    {
        static const std::regex ansi(R"(\x1b\[[0-9;]*[mK])");
        text = std::regex_replace(text, ansi, "");
    }

    // 2) Strip control characters (0x00-0x08, 0x0B, 0x0C, 0x0E-0x1F, 0x7F)
    //    but keep \t (0x09), \n (0x0A), \r (0x0D) — \r is normalized later.
    {
        std::string out;
        out.reserve(text.size());
        for (char c : text) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc == 0x09 || uc == 0x0A || uc == 0x0D) {
                out.push_back(c);
                continue;
            }
            if (uc <= 0x08 || uc == 0x0B || uc == 0x0C ||
                (uc >= 0x0E && uc <= 0x1F) || uc == 0x7F) {
                continue;
            }
            out.push_back(c);
        }
        text.swap(out);
    }

    // 3) Normalize newlines: \r\n -> \n, lone \r -> \n
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\r') {
                out.push_back('\n');
                if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            } else {
                out.push_back(text[i]);
            }
        }
        text.swap(out);
    }

    // 4) Apply size cap.
    size_t limit = g_paste_limit;
    if (text.size() > limit) {
        size_t removed = text.size() - limit;
        text.resize(limit);
        text += "\n...[truncated " + std::to_string(removed) + " chars]";
    }
    return text;
}

inline std::pair<bool, ParsedCommand::PasteKind> detect_paste_kind(std::string_view text) {
    if (text.empty()) return {false, ParsedCommand::PasteKind::Unknown};

    // JSON: first non-whitespace char is { or [
    size_t p = 0;
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    if (p < text.size() && (text[p] == '{' || text[p] == '[')) {
        std::string buf{text};
        if (cc::utils::json::parse(buf)) {
            return {true, ParsedCommand::PasteKind::Json};
        }
    }

    // Diff: 2+ lines of diff headers
    {
        int diff_lines = 0;
        size_t pos = 0;
        while (pos < text.size() && diff_lines < 2) {
            size_t end = text.find('\n', pos);
            size_t lstart = pos;
            while (lstart < text.size() && lstart < end &&
                   std::isspace(static_cast<unsigned char>(text[lstart]))) ++lstart;
            std::string_view line = text.substr(lstart, (end == std::string_view::npos ? text.size() : end) - lstart);
            if (line.substr(0, 9) == "diff --gi" ||  // diff --git
                line.substr(0, 4) == "--- " ||
                line.substr(0, 4) == "+++ ") {
                ++diff_lines;
            }
            if (end == std::string_view::npos) break;
            pos = end + 1;
        }
        if (diff_lines >= 2) return {true, ParsedCommand::PasteKind::Diff};
    }

    // Code: shebangs or multiple keyword occurrences
    {
        static const std::regex kw(
            R"(#!\s*/usr/bin/env|(^|\s)(function\s+[A-Za-z_]|class\s+[A-Za-z_]|def\s+[A-Za-z_]|fn\s+[A-Za-z_]|pub\s+fn\s+[A-Za-z_]|int\s+main\s*\())"
        );
        auto begin = std::cregex_iterator(text.data(), text.data() + text.size(), kw);
        auto end = std::cregex_iterator();
        int matches = 0;
        for (auto it = begin; it != end && matches < 3; ++it, ++matches) {}
        if (matches >= 2) return {true, ParsedCommand::PasteKind::Code};
    }

    // Markdown: headers or code fences
    {
        int md_lines = 0;
        int fences = 0;
        size_t pos = 0;
        while (pos < text.size() && (md_lines < 2 || fences < 1)) {
            size_t end = text.find('\n', pos);
            size_t lstart = pos;
            while (lstart < text.size() && lstart < end &&
                   std::isspace(static_cast<unsigned char>(text[lstart]))) ++lstart;
            std::string_view line = text.substr(lstart, (end == std::string_view::npos ? text.size() : end) - lstart);
            if (!line.empty() && line[0] == '#' && (line.size() == 1 || line[1] == ' ' || line[1] == '#')) ++md_lines;
            if (line.substr(0, 3) == "```") ++fences;
            if (end == std::string_view::npos) break;
            pos = end + 1;
        }
        if (md_lines >= 2 || fences >= 1) return {true, ParsedCommand::PasteKind::Markdown};
    }

    // Log: timestamp + level patterns (e.g. "2024-01-01 12:34:56 INFO ...")
    {
        static const std::regex log_re(
            R"(\d{4}[-/]\d{2}[-/]\d{2}[ T]\d{2}:\d{2}(:\d{2})?\s+(INFO|WARN|ERROR|DEBUG|TRACE|FATAL)\b)"
        );
        auto begin = std::cregex_iterator(text.data(), text.data() + text.size(), log_re);
        if (begin != std::cregex_iterator()) return {true, ParsedCommand::PasteKind::Log};
    }

    return {true, ParsedCommand::PasteKind::Unknown};
}

// ---------------------------------------------------------------------------
// At-mention helpers
// ---------------------------------------------------------------------------

inline std::vector<std::string> resolve_at_mentions(std::string_view text,
                                                    const InteractiveContext& ctx) {
    static const std::regex mention(R"(@([A-Za-z0-9_\-\.]+))");
    auto begin = std::cregex_iterator(text.data(), text.data() + text.size(), mention);
    auto end = std::cregex_iterator();
    std::vector<std::string> out;
    std::unordered_map<std::string, bool> seen;

    for (auto it = begin; it != end; ++it) {
        std::string needle = (*it)[1].str();
        std::vector<std::string> resolved;
        if (ctx.at_resolver) {
            resolved = ctx.at_resolver(needle);
        } else {
            // Fallback: substring-match against at_mention_index.
            auto low = detail::to_lower(needle);
            for (const auto& candidate : ctx.at_mention_index) {
                if (detail::to_lower(candidate).find(low) != std::string::npos) {
                    resolved.push_back(candidate);
                    if (resolved.size() >= 5) break;
                }
            }
        }
        for (const auto& r : resolved) {
            if (seen.emplace(r, true).second) out.push_back(r);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Slash name validation
// ---------------------------------------------------------------------------

inline bool is_valid_slash_name(std::string_view name) {
    if (name.empty() || name.size() > 32) return false;
    if (!std::isalpha(static_cast<unsigned char>(name[0]))) return false;
    for (size_t i = 1; i < name.size(); ++i) {
        char c = name[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Main parse function
// ---------------------------------------------------------------------------

inline std::expected<ParsedCommand, std::string>
parse_interactive_input(std::string_view raw, const InteractiveContext& ctx) {
    ParsedCommand cmd;
    cmd.raw = std::string(raw);
    auto trimmed = detail::trim(raw);
    cmd.normalized = std::string(trimmed);

    // Empty
    if (trimmed.empty()) {
        cmd.kind = CommandKind::Empty;
        return cmd;
    }

    // Paste detection based on length threshold
    if (trimmed.size() >= ctx.paste_min_chars) {
        cmd.kind = CommandKind::Paste;
        cmd.paste_size_bytes = trimmed.size();
        auto [ok, kind] = detect_paste_kind(trimmed);
        (void)ok;
        cmd.paste_kind = kind;
        return cmd;
    }

    // Slash command
    if (trimmed.front() == '/') {
        cmd.kind = CommandKind::SlashCommand;
        std::string_view rest = trimmed.substr(1);
        size_t space = rest.find_first_of(" \t");
        if (space == std::string_view::npos) {
            cmd.slash_name = std::string(rest);
        } else {
            cmd.slash_name = std::string(rest.substr(0, space));
            auto args_sv = rest.substr(space + 1);
            while (!args_sv.empty() && std::isspace(static_cast<unsigned char>(args_sv.front()))) {
                args_sv.remove_prefix(1);
            }
            cmd.slash_args = detail::quote_aware_split(args_sv);
        }
        return cmd;
    }

    // Bash bang
    if (trimmed.front() == '!') {
        cmd.kind = CommandKind::BashCommand;
        auto body = trimmed.substr(1);
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front()))) {
            body.remove_prefix(1);
        }
        cmd.bash_command_line = std::string(body);
        return cmd;
    }

    // Dollar bash (contextual)
    if (ctx.dollar_is_bash && trimmed.size() >= 2 &&
        trimmed[0] == '$' && std::isspace(static_cast<unsigned char>(trimmed[1]))) {
        cmd.kind = CommandKind::BashCommand;
        auto body = trimmed.substr(2);
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front()))) {
            body.remove_prefix(1);
        }
        cmd.bash_command_line = std::string(body);
        return cmd;
    }

    // At-mention scan
    {
        static const std::regex mention(R"(@([A-Za-z0-9_\-\.]+))");
        auto begin = std::cregex_iterator(
            trimmed.data(), trimmed.data() + trimmed.size(), mention);
        auto end = std::cregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            cmd.at_mentions.push_back(m[1].str());
            cmd.at_mention_ranges.emplace_back(
                static_cast<size_t>(m.position()),
                static_cast<size_t>(m.length()));
        }
    }

    // Whole input is a single @mention
    if (!cmd.at_mentions.empty() && cmd.at_mentions.size() == 1 &&
        cmd.at_mention_ranges.front().first == 0 &&
        cmd.at_mention_ranges.front().second == trimmed.size()) {
        cmd.kind = CommandKind::AtMentionOnly;
        return cmd;
    }

    // Default: text prompt
    cmd.kind = CommandKind::TextPrompt;
    return cmd;
}

// ---------------------------------------------------------------------------
// Slash command registry (singleton with pimpl)
// ---------------------------------------------------------------------------

class SlashRegistry {
public:
    struct Impl {
        std::unordered_map<std::string, SlashHandler> handlers;
        std::unordered_map<std::string, std::string> descriptions;
    };

    void register_handler(std::string name, SlashHandler h, std::string description = "") {
        std::lock_guard lk(mu_);
        impl_->handlers.emplace(std::move(name), std::move(h));
        if (!description.empty()) {
            impl_->descriptions.emplace(name, std::move(description));
        }
    }

    std::vector<std::string> list_names() const {
        std::lock_guard lk(mu_);
        std::vector<std::string> out;
        out.reserve(impl_->handlers.size());
        for (const auto& kv : impl_->handlers) out.push_back(kv.first);
        std::sort(out.begin(), out.end());
        return out;
    }

    bool has(std::string_view name) const {
        std::lock_guard lk(mu_);
        return impl_->handlers.contains(std::string(name));
    }

    std::optional<SlashHandler> find(std::string_view name) const {
        std::lock_guard lk(mu_);
        auto it = impl_->handlers.find(std::string(name));
        if (it == impl_->handlers.end()) return std::nullopt;
        return it->second;
    }

    std::string description(std::string_view name) const {
        std::lock_guard lk(mu_);
        auto it = impl_->descriptions.find(std::string(name));
        return it == impl_->descriptions.end() ? std::string{} : it->second;
    }

    static SlashRegistry& instance() {
        static SlashRegistry r;
        return r;
    }

private:
    SlashRegistry() : impl_(std::make_unique<Impl>()) {
        // Default registered handlers.  Each returns RouteToCommand with
        // target == name and payload == JSON-encoded args array.
        auto make_route = [](std::string name) {
            return [n = std::move(name)](const std::vector<std::string>& args,
                                         const InteractiveContext& /*ctx*/) {
                cc::utils::json::JsonArray arr;
                for (const auto& a : args) arr.push(a);
                DispatchOutcome o;
                o.action = DispatchOutcome::Action::RouteToCommand;
                o.target = n;
                o.payload_json = arr.serialize();
                return o;
            };
        };
        const char* builtins[] = {
            "help", "exit", "clear", "compact", "commit", "review", "config",
            "doctor", "mcp", "tasks", "agents", "repl", "skill", "version",
            "goal", "loop", "remember",
        };
        for (auto* b : builtins) impl_->handlers.emplace(b, make_route(b));
    }

    mutable std::mutex mu_;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

namespace detail {

inline std::string string_vec_json(const std::vector<std::string>& v) {
    cc::utils::json::JsonArray arr;
    for (const auto& a : v) arr.push(a);
    return arr.serialize();
}

inline bool is_dangerous_bash(std::string_view cmd) {
    static const std::regex danger(
        R"((sudo|rm\s+-rf|mkfs|dd\s+of=/dev/sd|wget[^\n]*\|[^\n]*\b(sh|bash)\b|curl[^\n]*\|[^\n]*\b(sh|bash)\b))",
        std::regex_constants::icase);
    return std::regex_search(cmd.begin(), cmd.end(), danger);
}

} // namespace detail

inline std::expected<DispatchOutcome, std::string>
dispatch_parsed(const ParsedCommand& cmd, const InteractiveContext& ctx) {
    DispatchOutcome out;

    switch (cmd.kind) {
        case CommandKind::Empty:
            out.action = DispatchOutcome::Action::RejectWithReason;
            out.reason = "empty input";
            return out;

        case CommandKind::SlashCommand: {
            auto handler = SlashRegistry::instance().find(cmd.slash_name);
            if (handler) {
                return (*handler)(cmd.slash_args, ctx);
            }
            // Unknown slash — fall through to RouteToCommand
            out.action = DispatchOutcome::Action::RouteToCommand;
            out.target = cmd.slash_name;
            out.payload_json = detail::string_vec_json(cmd.slash_args);
            return out;
        }

        case CommandKind::BashCommand: {
            if (detail::is_dangerous_bash(cmd.bash_command_line)) {
                out.action = DispatchOutcome::Action::AskConfirmation;
                out.confirmation_prompt =
                    "This command may be destructive. Confirm you want to run: " +
                    cmd.bash_command_line;
                out.target = cmd.bash_command_line;
                return out;
            }
            out.action = DispatchOutcome::Action::RouteToBash;
            out.target = cmd.bash_command_line;
            return out;
        }

        case CommandKind::AtMentionOnly: {
            auto resolved = resolve_at_mentions(cmd.normalized, ctx);
            out.action = DispatchOutcome::Action::RouteToAtMention;
            out.target = resolved.empty() ? (cmd.at_mentions.empty() ? std::string{}
                                                                      : cmd.at_mentions.front())
                                          : resolved.front();
            out.payload_json = detail::string_vec_json(resolved.empty() ? cmd.at_mentions : resolved);
            return out;
        }

        case CommandKind::Paste: {
            auto sanitized = sanitize_paste(cmd.normalized);
            const char* kind_str = "unknown";
            switch (cmd.paste_kind) {
                case ParsedCommand::PasteKind::Code: kind_str = "code"; break;
                case ParsedCommand::PasteKind::Json: kind_str = "json"; break;
                case ParsedCommand::PasteKind::Markdown: kind_str = "markdown"; break;
                case ParsedCommand::PasteKind::Log: kind_str = "log"; break;
                case ParsedCommand::PasteKind::Diff: kind_str = "diff"; break;
                default: kind_str = "unknown"; break;
            }
            cc::utils::json::JsonBuilder b;
            b.str("content", sanitized)
             .boolean("paste", true)
             .str("paste_kind", kind_str);
            out.action = DispatchOutcome::Action::RouteToPrompt;
            out.payload_json = b.serialize();
            return out;
        }

        case CommandKind::TextPrompt: {
            cc::utils::json::JsonBuilder b;
            b.str("content", cmd.normalized);
            auto& doc = b.doc();
            auto root = b.root();
            auto arr = doc.array();
            for (const auto& m : cmd.at_mentions) arr.append(doc.string(m));
            root.add("mentions", arr);
            out.action = DispatchOutcome::Action::RouteToPrompt;
            out.payload_json = b.serialize();
            return out;
        }

        case CommandKind::Unknown:
        default:
            out.action = DispatchOutcome::Action::RejectWithReason;
            out.reason = "unknown input kind";
            return out;
    }
}

inline std::expected<DispatchOutcome, std::string>
process_complete(std::string_view raw, const InteractiveContext& ctx) {
    auto parsed = parse_interactive_input(raw, ctx);
    if (!parsed) return std::unexpected(parsed.error());
    return dispatch_parsed(*parsed, ctx);
}

} // namespace cc::bootstrap::interactive
