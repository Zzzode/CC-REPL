/// @file permission_resolver.cppm
/// @brief Phase 3-G PermissionResolver: low-level permission decision engine.
///
/// This module is intentionally lightweight and UI-agnostic: it takes a
/// PermissionRequest describing what a tool wants to do, consults an
/// in-memory cache of "always" decisions, applies the default-risk
/// heuristic, and returns a single `Decision`.  The interactive user
/// prompt (if any) is the responsibility of the higher-level
/// `PermissionGate` in `cc.hooks.tool_permission_gate`.
///
/// Decision enum numeric values are FROZEN for backward compat with any
/// Phase C-E code that may have stored them on disk:
///   AllowOnce=0  AlwaysAllow=1  Deny=2  AlwaysDeny=3  Abort=4
module;

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.hooks.permission_resolver;

export namespace cc::hooks::permission {

// =========================================================================
// Core enums — keep numeric values in sync with Task #58 contract.
// =========================================================================

/// Final decision returned by Resolver / Gate / user prompt.
/// Numbers must NOT be reordered: persisted cache (CacheToJson) writes
/// integers, so any shift would invalidate on-disk permission grants.
enum class Decision : std::uint8_t {
    AllowOnce    = 0,  /// Allow this single call, do not cache
    AlwaysAllow  = 1,  /// Cache as "always allow" for matching (tool,path) pairs
    Deny         = 2,  /// Deny this call, do not cache
    AlwaysDeny   = 3,  /// Cache as "always deny" for matching pairs
    Abort        = 4,  /// User aborted the whole request, treat as fatal
};

/// Broad action category of a tool request.  Used for risk-level override
/// tables and audit display.
enum class ActionKind : std::uint8_t {
    Read      = 0,
    Write     = 1,
    Execute   = 2,
    Network   = 3,
    MCP       = 4,
    Plugin    = 5,
    Bridge    = 6,
    Unknown   = 255,
};

/// Risk ranking drives the default auto-allow heuristic.
enum class RiskLevel : std::uint8_t {
    Low       = 0,
    Medium    = 1,
    High      = 2,
    Critical  = 3,
};

/// Convert a decision back to a stable display name.  Never returns the
/// raw integer — use `static_cast<int>` for that.
[[nodiscard]] constexpr std::string_view DecisionName(Decision d) noexcept {
    switch (d) {
        case Decision::AllowOnce:   return "AllowOnce";
        case Decision::AlwaysAllow: return "AlwaysAllow";
        case Decision::Deny:        return "Deny";
        case Decision::AlwaysDeny:  return "AlwaysDeny";
        case Decision::Abort:       return "Abort";
    }
    return "Unknown";
}

[[nodiscard]] constexpr std::string_view ActionKindName(ActionKind k) noexcept {
    switch (k) {
        case ActionKind::Read:     return "Read";
        case ActionKind::Write:    return "Write";
        case ActionKind::Execute:  return "Execute";
        case ActionKind::Network:  return "Network";
        case ActionKind::MCP:      return "MCP";
        case ActionKind::Plugin:   return "Plugin";
        case ActionKind::Bridge:   return "Bridge";
        case ActionKind::Unknown:  return "Unknown";
    }
    return "Unknown";
}

// =========================================================================
// PermissionRequest
// =========================================================================

/// Everything the resolver needs to reach a decision.  Callers are
/// responsible for filling in fields that matter for their tool type.
struct PermissionRequest {
    /// Canonical tool identifier, e.g. "Bash", "FileRead", "Glob".
    std::string                tool_name;
    /// High-level action bucket.
    ActionKind                 action = ActionKind::Unknown;
    /// Paths touched by the operation.  May be empty for Network/MCP tools.
    std::vector<std::string>   affected_paths;
    /// Caller-assessed risk.  Medium+ always requires human review unless
    /// the user has previously cached an AlwaysAllow for the (tool,path).
    RiskLevel                  risk = RiskLevel::Low;
    /// If true, even Execute actions get a more lenient default.  Set by
    /// BashTool when it can prove the command will run in a sandbox.
    bool                       sandboxed = false;
    /// Tool-specific free-form payload, e.g. the raw Bash command or the
    /// JSON blob of a FileEditTool argument.  Never used for cache keys.
    std::string                extra;
};

// =========================================================================
// Glob helper — small hand-rolled double-star matcher.
//
// Supports:
//   `?`   — exactly one character except `/`
//   `*`   — zero or more characters except `/`
//   `**`  — zero or more complete path segments (including `/`)
//   any other literal char matched with ==
// =========================================================================

namespace detail {

[[nodiscard]] constexpr bool GlobMatch(std::string_view pattern,
                                       std::string_view text) noexcept {
    // Classic two-pointer + star-stack approach, extended for `**`.
    // We keep things recursive only for the `**` branch, which is rare
    // enough (permission rules are user-written and short) that OOM is
    // not a concern.
    const auto pat_end = pattern.size();
    const auto txt_end = text.size();
    std::size_t pi = 0;
    std::size_t ti = 0;

    while (pi < pat_end) {
        // Handle `**` — match any number of segments.
        if (pi + 1 < pat_end && pattern[pi] == '*' && pattern[pi + 1] == '*') {
            // Consume the `**` token (and an immediately-following `/`).
            pi += 2;
            if (pi < pat_end && pattern[pi] == '/') ++pi;
            // Fast path: `**` at end of pattern matches everything.
            if (pi == pat_end) return true;
            // Otherwise try every suffix of `text`.
            std::string_view rest_pattern = pattern.substr(pi);
            for (std::size_t sj = ti; sj <= txt_end; ++sj) {
                // Skip past a `/` on the first iteration so `a/**/b`
                // matches `a/b` as well as `a/x/b`.
                if (sj != ti && text[sj - 1] != '/') continue;
                if (GlobMatch(rest_pattern, text.substr(sj))) return true;
            }
            return false;
        }

        // Single `*` — non-segment wildcard.
        if (pattern[pi] == '*') {
            // Collect the fixed suffix we must align to.
            std::size_t literal_start = pi + 1;
            while (literal_start < pat_end &&
                   pattern[literal_start] != '*' &&
                   pattern[literal_start] != '?') {
                ++literal_start;
            }
            const std::string_view fixed = pattern.substr(pi + 1, literal_start - (pi + 1));
            // Find last occurrence of `fixed` starting at ti that does
            // NOT cross a `/` (since * is single-segment).
            if (fixed.empty()) {
                // `*` at end or before another wildcard: skip to next `/`
                // or end of text.
                while (ti < txt_end && text[ti] != '/') ++ti;
                ++pi;
                continue;
            }
            std::size_t match_at = std::string_view::npos;
            for (std::size_t scan = ti; scan + fixed.size() <= txt_end; ++scan) {
                if (text[scan] == '/') break;  // single-segment boundary
                if (text.substr(scan, fixed.size()) == fixed) {
                    match_at = scan;
                    break;
                }
            }
            if (match_at == std::string_view::npos) return false;
            ti = match_at + fixed.size();
            pi = literal_start;
            continue;
        }

        if (ti >= txt_end) return false;

        if (pattern[pi] == '?') {
            if (text[ti] == '/') return false;
            ++pi; ++ti;
            continue;
        }

        if (pattern[pi] != text[ti]) return false;
        ++pi; ++ti;
    }

    return ti == txt_end;
}

} // namespace detail

/// Public glob matcher — exported so PermissionGate and tests can reuse it.
[[nodiscard]] inline bool MatchGlob(std::string_view pattern,
                                    std::string_view text) noexcept {
    // Literal "match everything" shortcuts.
    if (pattern.empty()) return text.empty();
    if (pattern == "*" || pattern == "**") return true;
    return detail::GlobMatch(pattern, text);
}

// =========================================================================
// PermissionResolver
// =========================================================================

class PermissionResolver {
public:
    PermissionResolver() = default;

    // Move-only (cache holds stable hash map keys; copies are wasteful).
    PermissionResolver(const PermissionResolver&)            = delete;
    auto operator=(const PermissionResolver&) -> PermissionResolver& = delete;
    PermissionResolver(PermissionResolver&&) noexcept            = default;
    auto operator=(PermissionResolver&&) noexcept -> PermissionResolver& = default;
    ~PermissionResolver() = default;

    /// Resolve a single request, consulting the always-cache first and
    /// falling back to the risk+sandbox default heuristic.  Never
    /// prompts the user; `PermissionGate` owns that interaction.
    [[nodiscard]] Decision Resolve(const PermissionRequest& req) const {
        // Check every stored always-decision against every affected path.
        // First AlwaysDeny wins over AlwaysAllow (defensive bias), so we
        // scan for denies first, then allows, then fall through.
        for (const auto& [key, entry] : cached_always_) {
            (void)key;   // key is hashed pair; we match via the glob strings
            const auto& tool_glob = entry.tool_glob;
            const auto& path_glob = entry.path_glob;
            const Decision d      = entry.decision;

            // Tool match first (cheap string compare before glob)
            if (!MatchToolGlob(tool_glob, req.tool_name)) continue;

            // If this entry has no path constraint or there are no paths,
            // treat the tool-only match as authoritative.
            const bool path_constrained = !path_glob.empty() &&
                                          path_glob != "*" && path_glob != "**";
            if (req.affected_paths.empty() || !path_constrained) {
                if (d == Decision::AlwaysDeny)  return Decision::Deny;
                if (d == Decision::AlwaysAllow) return Decision::AlwaysAllow;
                continue;
            }

            // Otherwise: does ANY affected path match the path glob?
            bool any_match = false;
            for (const auto& path : req.affected_paths) {
                if (MatchGlob(path_glob, path)) { any_match = true; break; }
            }
            if (!any_match) continue;

            if (d == Decision::AlwaysDeny)  return Decision::Deny;
            if (d == Decision::AlwaysAllow) return Decision::AlwaysAllow;
        }

        // ---- Default heuristic -----------------------------------------
        if (req.risk == RiskLevel::Low && req.sandboxed) {
            return Decision::AllowOnce;
        }
        // Everything else requires interactive review.  We return Deny
        // here so the gate can short-circuit the interactive prompt when
        // the caller has not provided one — the gate re-prompts when an
        // interactive hook is available.
        return Decision::Deny;
    }

    /// Record a user's "always" decision for a (tool_glob, path_glob) pair.
    /// Only AlwaysAllow / AlwaysDeny produce cache entries; other Decisions
    /// are no-ops (defense-in-depth — the caller should not pass other
    /// values, but silently ignoring keeps the API robust).
    void CacheAlways(std::string tool_glob,
                     std::string path_glob,
                     Decision d) {
        if (d != Decision::AlwaysAllow && d != Decision::AlwaysDeny) return;
        const size_t h = HashPair(tool_glob, path_glob);
        cached_always_[h] = CacheEntry{std::move(tool_glob), std::move(path_glob), d};
    }

    /// Number of always-* cache entries (for introspection / tests).
    [[nodiscard]] auto cache_size() const noexcept -> std::size_t {
        return cached_always_.size();
    }

    /// Serialize the cache to a compact JSON string.
    ///
    /// Schema:
    ///   {"version":1,"entries":[{"t":"<tool_glob>","p":"<path_glob>","d":<int>},...]}
    /// Version is bumped if we ever change the storage format; readers
    /// should reject unknown versions to stay safe.
    [[nodiscard]] std::string CacheToJson() const {
        // Hand-rolled writer keeps this module independent of the JSON
        // utility layer; permission payloads are small enough that code
        // simplicity wins over performance here.
        std::string out;
        out.reserve(64 + cached_always_.size() * 80);
        out.append("{\"version\":1,\"entries\":[");
        bool first = true;
        for (const auto& [key, entry] : cached_always_) {
            (void)key;
            if (!first) out.push_back(',');
            first = false;
            out.append("{\"t\":");
            JsonAppendString(out, entry.tool_glob);
            out.append(",\"p\":");
            JsonAppendString(out, entry.path_glob);
            out.append(",\"d\":");
            out.append(std::to_string(static_cast<int>(entry.decision)));
            out.push_back('}');
        }
        out.append("]}");
        return out;
    }

    /// Restore cache entries from a JSON string produced by CacheToJson.
    /// Returns true if parsing completed successfully; malformed payloads
    /// leave the cache in a partial state (entries loaded before the
    /// error are kept) but still return `false` so the caller can log.
    [[nodiscard]] bool CacheFromJson(std::string_view json) {
        // Minimal recursive-descent parser.  We intentionally avoid
        // pulling in the full JSON module so this resolver can be used
        // by the earliest startup paths (before the heavy utils layer
        // is initialised).
        const auto end = json.size();
        std::size_t p = 0;
        auto skip_ws = [&] {
            while (p < end) {
                const char c = json[p];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p;
                else break;
            }
        };
        auto expect = [&](char c) -> bool {
            skip_ws();
            if (p >= end || json[p] != c) return false;
            ++p;
            return true;
        };
        auto read_int = [&](int& out_val) -> bool {
            skip_ws();
            std::size_t start = p;
            if (p < end && (json[p] == '-' || json[p] == '+')) ++p;
            if (p >= end || json[p] < '0' || json[p] > '9') return false;
            while (p < end && json[p] >= '0' && json[p] <= '9') ++p;
            std::string_view slice = json.substr(start, p - start);
            long long v = 0;
            for (char ch : slice) {
                if (ch == '+' || ch == '-') continue;
                v = v * 10 + (ch - '0');
            }
            if (slice.front() == '-') v = -v;
            out_val = static_cast<int>(v);
            return true;
        };
        auto read_string = [&](std::string& out_val) -> bool {
            skip_ws();
            if (!expect('"')) return false;
            out_val.clear();
            while (p < end && json[p] != '"') {
                if (json[p] == '\\') {
                    ++p;
                    if (p >= end) return false;
                    const char esc = json[p++];
                    switch (esc) {
                        case '"':  out_val.push_back('"'); break;
                        case '\\': out_val.push_back('\\'); break;
                        case '/':  out_val.push_back('/'); break;
                        case 'n':  out_val.push_back('\n'); break;
                        case 't':  out_val.push_back('\t'); break;
                        case 'r':  out_val.push_back('\r'); break;
                        case 'b':  out_val.push_back('\b'); break;
                        case 'f':  out_val.push_back('\f'); break;
                        case 'u': {
                            // Accept \uXXXX but only for ASCII (permission
                            // rules never need full unicode).  Anything
                            // non-ASCII is written back verbatim as four
                            // hex digits so roundtrip stays lossless.
                            if (p + 4 > end) return false;
                            out_val.append("\\u");
                            out_val.append(json.substr(p, 4).data(), 4);
                            p += 4;
                            break;
                        }
                        default:
                            return false;
                    }
                } else {
                    out_val.push_back(json[p++]);
                }
            }
            if (!expect('"')) return false;
            return true;
        };

        if (!expect('{')) return false;
        // parse top-level key/value pairs until we hit `}`
        int version = 0;
        bool have_entries = false;
        bool ok = true;
        bool need_comma = false;
        while (true) {
            skip_ws();
            if (p >= end) return false;
            if (json[p] == '}') { ++p; break; }
            if (need_comma) { if (!expect(',')) return false; }
            need_comma = true;

            std::string key;
            if (!read_string(key)) return false;
            if (!expect(':')) return false;

            if (key == "version") {
                if (!read_int(version)) return false;
                if (version != 1) return false;  // forward-incompat
            } else if (key == "entries") {
                have_entries = true;
                if (!expect('[')) return false;
                bool entry_comma = false;
                while (true) {
                    skip_ws();
                    if (p >= end) return false;
                    if (json[p] == ']') { ++p; break; }
                    if (entry_comma) { if (!expect(',')) return false; }
                    entry_comma = true;
                    if (!expect('{')) return false;
                    std::string tg, pg;
                    int         dint = -1;
                    bool field_comma = false;
                    while (true) {
                        skip_ws();
                        if (p >= end) return false;
                        if (json[p] == '}') { ++p; break; }
                        if (field_comma) { if (!expect(',')) return false; }
                        field_comma = true;
                        std::string fk;
                        if (!read_string(fk)) return false;
                        if (!expect(':')) return false;
                        if (fk == "t")      { if (!read_string(tg)) return false; }
                        else if (fk == "p") { if (!read_string(pg)) return false; }
                        else if (fk == "d") { if (!read_int(dint)) return false; }
                        else { return false; }  // unknown field
                    }
                    if (dint != static_cast<int>(Decision::AlwaysAllow) &&
                        dint != static_cast<int>(Decision::AlwaysDeny)) {
                        ok = false;
                        continue;
                    }
                    CacheAlways(std::move(tg), std::move(pg),
                                static_cast<Decision>(dint));
                }
            } else {
                return false;  // unknown top-level key — bail
            }
        }
        return ok && have_entries;
    }

private:
    /// A single cached "always" decision.  Storing the original globs
    /// (instead of just the hash) lets us re-run the matcher per path
    /// without collisions — the hash is only the lookup key.
    struct CacheEntry {
        std::string tool_glob;
        std::string path_glob;
        Decision    decision;
    };

    /// Keyed by hash of (tool_glob + "|" + path_glob).  Collisions are
    /// harmless because the match logic re-checks the glob strings.
    std::unordered_map<std::size_t, CacheEntry> cached_always_;

    [[nodiscard]] static std::size_t HashPair(std::string_view a,
                                              std::string_view b) noexcept {
        std::string s;
        s.reserve(a.size() + 1 + b.size());
        s.append(a.data(), a.size());
        s.push_back('|');
        s.append(b.data(), b.size());
        return std::hash<std::string>{}(s);
    }

    /// Match `tool_name` against `tool_glob`.  Most callers use a simple
    /// string (e.g. "Bash") — for those we skip the glob engine.  The
    /// only user-visible glob allowed here is `*` as "any tool".
    [[nodiscard]] static bool MatchToolGlob(std::string_view pattern,
                                            std::string_view name) noexcept {
        if (pattern.empty()) return name.empty();
        if (pattern == "*" || pattern == "**") return true;
        if (pattern == name) return true;
        // Support the same glob grammar for tool names so users *can* do
        // `File*` if they really want to.
        return MatchGlob(pattern, name);
    }

    /// Append a properly-escaped JSON string `s` into `out`.
    static void JsonAppendString(std::string& out, std::string_view s) {
        out.push_back('"');
        for (char c : s) {
            switch (c) {
                case '"':  out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\n': out.append("\\n");  break;
                case '\r': out.append("\\r");  break;
                case '\t': out.append("\\t");  break;
                case '\b': out.append("\\b");  break;
                case '\f': out.append("\\f");  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out.append(buf);
                    } else {
                        out.push_back(c);
                    }
            }
        }
        out.push_back('"');
    }
};

} // namespace cc::hooks::permission
