/// @file insights.cppm
/// @brief InsightsCommand implementing the /insights slash command.
///
/// Faithful C++23 port of the TS /insights facet pipeline (see
/// src/commands/insights.ts). Ports the portable, deterministic core of the
/// TS feature:
///   - Local session stats scan (counts, model distribution, recent sessions)
///   - Facet data model (SessionFacets) with the TS LABEL_MAP for humanising
///     snake_case category/outcome/satisfaction/friction/success labels
///   - goal_categories / outcomes / satisfaction / friction aggregation
///     (aggregate_facets) — pure and deterministic
///   - File-based facet caching (JSON round-trip via cc.utils.json) with the
///     same validity predicate as the TS source
///   - HTML report rendering from aggregated data (pure string building)
///   - LLM facet extraction wired through an injectable std::function seam
///     (llm_extract_fn) so the call site is testable with a stub without
///     hitting the network. The default implementation calls the existing
///     AnthropicClient::create_message completion API.
///
/// Deferred (reported as residual): remote homespace collection (requires
/// ssh/scp/coder subprocesses — environment-specific), parallel narrative
/// insights generation (6 LLM sections), S3 upload, multi-clauding overlap
/// detection (needs ISO timestamp parsing from transcript parsing, which the
/// C++ session store does not yet expose), and the full TS HTML/CSS/JS surface
/// (the port renders a structurally-faithful summary report rather than the
/// 1300-line interactive page).

module;

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module cc.commands.insights;

import cc.types.types;
import cc.commands.command;
import cc.utils.list_sessions;
import cc.utils.json;
import cc.utils.error;

export namespace cc::commands {

using namespace cc::core;
namespace fs = std::filesystem;

// ============================================================================
// Label map — mirrors the TS LABEL_MAP verbatim. Humanises snake_case keys
// returned by the LLM facet extraction (and stored in cache files).
// ============================================================================
inline const std::map<std::string, std::string>& label_map() {
    static const std::map<std::string, std::string> m = {
        // Goal categories
        {"debug_investigate", "Debug/Investigate"},
        {"implement_feature", "Implement Feature"},
        {"fix_bug", "Fix Bug"},
        {"write_script_tool", "Write Script/Tool"},
        {"refactor_code", "Refactor Code"},
        {"configure_system", "Configure System"},
        {"create_pr_commit", "Create PR/Commit"},
        {"analyze_data", "Analyze Data"},
        {"understand_codebase", "Understand Codebase"},
        {"write_tests", "Write Tests"},
        {"write_docs", "Write Docs"},
        {"deploy_infra", "Deploy/Infra"},
        {"warmup_minimal", "Cache Warmup"},
        // Success factors
        {"fast_accurate_search", "Fast/Accurate Search"},
        {"correct_code_edits", "Correct Code Edits"},
        {"good_explanations", "Good Explanations"},
        {"proactive_help", "Proactive Help"},
        {"multi_file_changes", "Multi-file Changes"},
        {"handled_complexity", "Multi-file Changes"},
        {"good_debugging", "Good Debugging"},
        // Friction types
        {"misunderstood_request", "Misunderstood Request"},
        {"wrong_approach", "Wrong Approach"},
        {"buggy_code", "Buggy Code"},
        {"user_rejected_action", "User Rejected Action"},
        {"claude_got_blocked", "Claude Got Blocked"},
        {"user_stopped_early", "User Stopped Early"},
        {"wrong_file_or_location", "Wrong File/Location"},
        {"excessive_changes", "Excessive Changes"},
        {"slow_or_verbose", "Slow/Verbose"},
        {"tool_failed", "Tool Failed"},
        {"user_unclear", "User Unclear"},
        {"external_issue", "External Issue"},
        // Satisfaction labels
        {"frustrated", "Frustrated"},
        {"dissatisfied", "Dissatisfied"},
        {"likely_satisfied", "Likely Satisfied"},
        {"satisfied", "Satisfied"},
        {"happy", "Happy"},
        {"unsure", "Unsure"},
        {"neutral", "Neutral"},
        {"delighted", "Delighted"},
        // Session types
        {"single_task", "Single Task"},
        {"multi_task", "Multi Task"},
        {"iterative_refinement", "Iterative Refinement"},
        {"exploration", "Exploration"},
        {"quick_question", "Quick Question"},
        // Outcomes
        {"fully_achieved", "Fully Achieved"},
        {"mostly_achieved", "Mostly Achieved"},
        {"partially_achieved", "Partially Achieved"},
        {"not_achieved", "Not Achieved"},
        {"unclear_from_transcript", "Unclear"},
        // Helpfulness
        {"unhelpful", "Unhelpful"},
        {"slightly_helpful", "Slightly Helpful"},
        {"moderately_helpful", "Moderately Helpful"},
        {"very_helpful", "Very Helpful"},
        {"essential", "Essential"},
    };
    return m;
}

/// Humanise a snake_case key, falling back to the raw key when unknown.
/// Matches the TS behaviour (unknown keys are rendered verbatim).
[[nodiscard]] inline std::string humanise(std::string_view key) {
    const auto& m = label_map();
    auto it = m.find(std::string(key));
    if (it != m.end()) return it->second;
    return std::string(key);
}

// ============================================================================
// Facet data model — mirrors the TS SessionFacets struct.
// ============================================================================
struct SessionFacets {
    std::string session_id;
    std::string underlying_goal;
    std::map<std::string, std::size_t> goal_categories;
    std::string outcome;                                   // fully_achieved | ...
    std::map<std::string, std::size_t> user_satisfaction_counts;
    std::string claude_helpfulness;                        // unhelpful | ...
    std::string session_type;                              // single_task | ...
    std::map<std::string, std::size_t> friction_counts;
    std::string friction_detail;
    std::string primary_success;                           // none | fast_accurate_search | ...
    std::string brief_summary;
};

/// Validity predicate mirroring TS isValidSessionFacets: the four required
/// string fields must be strings and the three map fields must be objects.
/// (We accept any object here; type is already enforced by our JSON reader.)
[[nodiscard]] inline bool is_valid_facets(const cc::utils::json::JsonVal& o) {
    if (!o.is_obj()) return false;
    if (!o.get("underlying_goal").is_str()) return false;
    if (!o.get("outcome").is_str()) return false;
    if (!o.get("brief_summary").is_str()) return false;
    if (!o.get("goal_categories").is_obj()) return false;
    if (!o.get("user_satisfaction_counts").is_obj()) return false;
    if (!o.get("friction_counts").is_obj()) return false;
    return true;
}

// ============================================================================
// Aggregated facet data — the deterministic aggregate the report renders from.
// ============================================================================
struct AggregatedFacets {
    std::size_t sessions_with_facets = 0;
    std::map<std::string, std::size_t> goal_categories;
    std::map<std::string, std::size_t> outcomes;
    std::map<std::string, std::size_t> satisfaction;
    std::map<std::string, std::size_t> helpfulness;
    std::map<std::string, std::size_t> session_types;
    std::map<std::string, std::size_t> friction;
    std::map<std::string, std::size_t> success;
};

/// Read a {key:count} object into a std::map<std::string,std::size_t>,
/// skipping non-positive counts (matches the TS `count > 0` filter for the
/// *_counts maps).
[[nodiscard]] inline std::map<std::string, std::size_t>
read_count_map(const cc::utils::json::JsonVal& obj) {
    std::map<std::string, std::size_t> out;
    if (!obj.is_obj()) return out;
    obj.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal val) {
        if (key.is_str() && val.is_num() && val.as_int() > 0) {
            out[std::string(key.as_str())] =
                static_cast<std::size_t>(val.as_int());
        }
    });
    return out;
}

/// Parse a SessionFacets from an already-validated JSON value.
/// `session_id` is supplied by the caller (cache filename), matching the TS
/// `{ ...parsed, session_id }` merge.
[[nodiscard]] inline SessionFacets
facets_from_json(const cc::utils::json::JsonVal& o, std::string session_id) {
    SessionFacets f;
    f.session_id = std::move(session_id);
    f.underlying_goal = o.get_string("underlying_goal");
    f.goal_categories = read_count_map(o.get("goal_categories"));
    f.outcome = o.get_string("outcome");
    f.user_satisfaction_counts =
        read_count_map(o.get("user_satisfaction_counts"));
    f.claude_helpfulness = o.get_string("claude_helpfulness");
    f.session_type = o.get_string("session_type");
    f.friction_counts = read_count_map(o.get("friction_counts"));
    f.friction_detail = o.get_string("friction_detail");
    f.primary_success = o.get_string("primary_success");
    f.brief_summary = o.get_string("brief_summary");
    return f;
}

/// Aggregate a collection of SessionFacets into the rollup the report needs.
/// Pure and deterministic — directly mirrors the TS aggregateData loop for the
/// facet-derived fields (goal_categories/outcomes/satisfaction/helpfulness/
/// session_types/friction/success).
[[nodiscard]] inline AggregatedFacets
aggregate_facets(const std::vector<SessionFacets>& facets) {
    AggregatedFacets r;
    r.sessions_with_facets = facets.size();

    auto bump = [](std::map<std::string, std::size_t>& m,
                   const std::string& key, std::size_t n) {
        if (!key.empty()) m[key] += n;
    };
    auto bump_count_map = [&](std::map<std::string, std::size_t>& dst,
                              const std::map<std::string, std::size_t>& src) {
        for (const auto& [k, v] : src) {
            if (v > 0) dst[k] += v;
        }
    };

    for (const auto& f : facets) {
        bump_count_map(r.goal_categories, f.goal_categories);
        bump(r.outcomes, f.outcome, 1);
        bump_count_map(r.satisfaction, f.user_satisfaction_counts);
        bump(r.helpfulness, f.claude_helpfulness, 1);
        bump(r.session_types, f.session_type, 1);
        bump_count_map(r.friction, f.friction_counts);
        if (f.primary_success != "none") {
            bump(r.success, f.primary_success, 1);
        }
    }
    return r;
}

// ============================================================================
// Facet caching — file-based JSON round-trip. Mirrors the TS load/save pair,
// writing one file per session under <data_dir>/facets/<id>.json.
// ============================================================================
namespace detail {

inline fs::path usage_data_dir() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::temp_directory_path();
    return base / ".claude" / "usage-data";
}

inline fs::path facets_dir() { return usage_data_dir() / "facets"; }

inline fs::path facet_path(std::string_view session_id) {
    return facets_dir() / std::string(session_id).append(".json");
}

} // namespace detail

/// Render a SessionFacets to a pretty JSON string. Round-trips through
/// facets_from_json so the written form is the canonical aggregate input.
[[nodiscard]] inline std::string facets_to_json(const SessionFacets& f) {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    doc.set_root(root);

    auto put_map = [&](std::string_view key,
                       const std::map<std::string, std::size_t>& m) {
        auto obj = doc.object();
        for (const auto& [k, v] : m) {
            obj.add(std::string_view(k), doc.number(static_cast<int64_t>(v)));
        }
        root.add(key, obj);
    };

    root.add("session_id", doc.string(f.session_id));
    root.add("underlying_goal", doc.string(f.underlying_goal));
    put_map("goal_categories", f.goal_categories);
    root.add("outcome", doc.string(f.outcome));
    put_map("user_satisfaction_counts", f.user_satisfaction_counts);
    root.add("claude_helpfulness", doc.string(f.claude_helpfulness));
    root.add("session_type", doc.string(f.session_type));
    put_map("friction_counts", f.friction_counts);
    root.add("friction_detail", doc.string(f.friction_detail));
    root.add("primary_success", doc.string(f.primary_success));
    root.add("brief_summary", doc.string(f.brief_summary));

    return doc.to_pretty_string();
}

/// Save facets to <facets_dir>/<session_id>.json, creating the directory.
/// Returns an error if the write fails (mirrors TS writeFile). The directory
/// is created with default permissions; the file is written 0600 on POSIX
/// (std::ofstream has no portable mode knob, so we chmod after the fact).
[[nodiscard]] inline cc::utils::Result<void>
save_facets(const SessionFacets& f) {
    std::error_code ec;
    fs::create_directories(detail::facets_dir(), ec);
    // Ignore directory-exists; surface other errors.
    auto path = detail::facet_path(f.session_id);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::internal_error,
            std::format("Failed to open facet cache for write: {}",
                        path.string())));
    }
    out << facets_to_json(f);
    out.close();
    if (!out) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::internal_error,
            std::format("Failed to write facet cache: {}", path.string())));
    }
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    return {};
}

/// Load cached facets for a session, or std::nullopt if missing/invalid.
/// Mirrors the TS loadCachedFacets: a corrupt file is treated as a cache miss
/// (the TS source deletes it; here we only return nullopt to keep the helper
/// pure and side-effect-free for testability — the caller can recreate it).
[[nodiscard]] inline std::optional<SessionFacets>
load_facets(std::string_view session_id) {
    auto path = detail::facet_path(session_id);
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return std::nullopt;

    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    auto parsed = cc::utils::json::parse(content);
    if (!parsed) return std::nullopt;
    auto root = parsed->root();
    if (!is_valid_facets(root)) return std::nullopt;
    return facets_from_json(root, std::string(session_id));
}

// ============================================================================
// HTML report rendering — pure string building from aggregated facet data.
// Produces a structurally faithful summary of the TS HTML report: a DOCTYPE
// document with a header, aggregate stat blocks, and labelled bar-style lists
// for each facet dimension. The full interactive TS page (CSS theme, JS charts,
// timezone selector, copy buttons) is intentionally NOT reproduced here — see
// the module docstring (residual).
// ============================================================================

namespace detail {

/// Escape a string for safe inclusion as HTML text content. Matches the TS
/// escapeHtml usage for user-derived strings (goal/summary text).
[[nodiscard]] inline std::string escape_html(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + sv.size() / 8);
    for (char c : sv) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

/// Render a ranked list of (label,count) rows as an HTML <ol>. Keys are
/// humanised; the list is sorted by descending count then by key for a stable
/// deterministic order (the TS sorts by descending count only; the tie-break
/// makes the output testable).
[[nodiscard]] inline std::string render_ranked_list(
    const std::map<std::string, std::size_t>& counts,
    std::string_view list_class) {
    if (counts.empty()) return "<p class=\"empty\">No data</p>\n";

    std::vector<std::pair<std::string, std::size_t>> rows(counts.begin(),
                                                          counts.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });

    std::string out;
    out += std::format("<ol class=\"{}\">\n", list_class);
    for (const auto& [key, count] : rows) {
        out += std::format("  <li><span class=\"label\">{}</span>"
                           "<span class=\"count\">{}</span></li>\n",
                           escape_html(humanise(key)), count);
    }
    out += "</ol>\n";
    return out;
}

/// Render a section with a heading and a ranked list body.
[[nodiscard]] inline std::string render_section(
    std::string_view id, std::string_view title,
    const std::map<std::string, std::size_t>& counts) {
    std::string out;
    out += std::format("<section id=\"{}\">\n  <h2>{}</h2>\n",
                       escape_html(id), escape_html(title));
    out += render_ranked_list(counts, "facet-list");
    out += "</section>\n";
    return out;
}

} // namespace detail

/// Build the full HTML insights report from the session stats + aggregated
/// facets. Pure function — given the same inputs it always produces the same
/// document, which is what the tests assert.
[[nodiscard]] inline std::string render_html_report(
    std::size_t total_sessions, std::size_t total_messages,
    std::string_view date_range_start, std::string_view date_range_end,
    const AggregatedFacets& agg) {
    std::string out;
    out += "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n";
    out += "  <meta charset=\"utf-8\">\n";
    out += "  <title>Claude Code Insights</title>\n";
    out += "  <style>\n"
           "    body { font-family: Inter, system-ui, sans-serif; "
           "margin: 2rem auto; max-width: 48rem; color: #1f2937; }\n"
           "    h1 { font-size: 1.6rem; }\n"
           "    .subtitle { color: #6b7280; margin-bottom: 1.5rem; }\n"
           "    .stats { display: flex; gap: 1rem; margin-bottom: 2rem; }\n"
           "    .stat { background: #f3f4f6; padding: 0.75rem 1rem; "
           "border-radius: 0.5rem; }\n"
           "    .stat .value { font-size: 1.25rem; font-weight: 600; }\n"
           "    .stat .label { color: #6b7280; font-size: 0.8rem; }\n"
           "    ol.facet-list { list-style: none; padding: 0; }\n"
           "    ol.facet-list li { display: flex; justify-content: "
           "space-between; padding: 0.25rem 0; border-bottom: 1px solid "
           "#e5e7eb; }\n"
           "    .count { font-variant-numeric: tabular-nums; color: #2563eb; "
           "font-weight: 600; }\n"
           "    .empty { color: #9ca3af; font-style: italic; }\n"
           "  </style>\n";
    out += "</head>\n<body>\n";
    out += "  <h1>Claude Code Insights</h1>\n";
    out += std::format("  <p class=\"subtitle\">{} messages across {} "
                       "sessions | {} to {}</p>\n",
                       total_messages, total_sessions,
                       detail::escape_html(date_range_start),
                       detail::escape_html(date_range_end));

    out += "  <div class=\"stats\">\n";
    out += std::format("    <div class=\"stat\"><div class=\"value\">{}</div>"
                       "<div class=\"label\">Sessions</div></div>\n",
                       total_sessions);
    out += std::format("    <div class=\"stat\"><div class=\"value\">{}</div>"
                       "<div class=\"label\">Messages</div></div>\n",
                       total_messages);
    out += std::format("    <div class=\"stat\"><div class=\"value\">{}</div>"
                       "<div class=\"label\">Analyzed</div></div>\n",
                       agg.sessions_with_facets);
    out += "  </div>\n";

    out += detail::render_section("goals", "What You Worked On",
                                  agg.goal_categories);
    out += detail::render_section("outcomes", "Outcomes", agg.outcomes);
    out += detail::render_section("satisfaction", "Satisfaction",
                                  agg.satisfaction);
    out += detail::render_section("helpfulness", "Claude Helpfulness",
                                  agg.helpfulness);
    out += detail::render_section("types", "Session Types",
                                  agg.session_types);
    out += detail::render_section("friction", "Where Things Went Wrong",
                                  agg.friction);
    out += detail::render_section("success", "What Worked Well",
                                  agg.success);

    out += "</body>\n</html>\n";
    return out;
}

// ============================================================================
// LLM facet extraction seam.
//
// The TS source calls queryWithModel() to ask the model to return a JSON
// object matching the facet schema for a given transcript. That call is not
// deterministically testable, so we expose it through a std::function the
// command accepts. The function takes a transcript string and returns either
// a SessionFacets (with session_id filled in by the caller) or std::nullopt
// on failure / non-JSON / invalid schema.
//
// extract_facets_with_seam() runs the seam, parses the JSON blob out of the
// response (matching the TS /\{[\s\S]*\}/ regex), validates it, and returns
// the facets tagged with the supplied session id. It is the unit of behaviour
// the seam encapsulates so the parsing/validation can be tested with a stub.
// ============================================================================
using LlmExtractFn =
    std::function<std::optional<std::string>(const std::string& transcript)>;

/// The facet extraction prompt prefix — verbatim from the TS source. Exported
/// so callers (and tests) can assert the contract matches the TS feature.
inline const std::string& facet_extraction_prompt() {
    static const std::string p =
        "Analyze this Claude Code session and extract structured facets.\n\n"
        "CRITICAL GUIDELINES:\n\n"
        "1. **goal_categories**: Count ONLY what the USER explicitly asked "
        "for.\n"
        "   - DO NOT count Claude's autonomous codebase exploration\n"
        "   - DO NOT count work Claude decided to do on its own\n"
        "   - ONLY count when user says \"can you...\", \"please...\", "
        "\"I need...\", \"let's...\"\n\n"
        "2. **user_satisfaction_counts**: Base ONLY on explicit user signals.\n"
        "   - \"Yay!\", \"great!\", \"perfect!\" -> happy\n"
        "   - \"thanks\", \"looks good\", \"that works\" -> satisfied\n"
        "   - \"ok, now let's...\" (continuing without complaint) -> "
        "likely_satisfied\n"
        "   - \"that's not right\", \"try again\" -> dissatisfied\n"
        "   - \"this is broken\", \"I give up\" -> frustrated\n\n"
        "3. **friction_counts**: Be specific about what went wrong.\n"
        "   - misunderstood_request: Claude interpreted incorrectly\n"
        "   - wrong_approach: Right goal, wrong solution method\n"
        "   - buggy_code: Code didn't work correctly\n"
        "   - user_rejected_action: User said no/stop to a tool call\n"
        "   - excessive_changes: Over-engineered or changed too much\n\n"
        "4. If very short or just warmup, use warmup_minimal for "
        "goal_category\n\n"
        "SESSION:\n";
    return p;
}

/// Run a facet-extraction seam against a transcript, parse the JSON blob from
/// the response, validate it, and tag the result with the session id.
/// Returns std::nullopt on any failure (no seam, empty response, no JSON,
/// invalid schema) — mirroring the TS extractFacetsFromAPI null-on-failure
/// contract.
[[nodiscard]] inline std::optional<SessionFacets>
extract_facets_with_seam(const LlmExtractFn& seam,
                         const std::string& transcript,
                         std::string_view session_id) {
    if (!seam) return std::nullopt;
    auto text = seam(transcript);
    if (!text || text->empty()) return std::nullopt;

    // Match the first {...} blob, like the TS /\{[\s\S]*\}/ regex.
    auto first = text->find('{');
    auto last = text->rfind('}');
    if (first == std::string::npos || last == std::string::npos ||
        last <= first) {
        return std::nullopt;
    }
    auto blob = text->substr(first, last - first + 1);

    auto parsed = cc::utils::json::parse(blob);
    if (!parsed) return std::nullopt;
    auto root = parsed->root();
    if (!is_valid_facets(root)) return std::nullopt;
    return facets_from_json(root, std::string(session_id));
}

// ============================================================================
// InsightsCommand — the /insights slash command.
//
// Renders local session stats (as before) plus, when facets are available,
// the aggregate facet breakdown and a pointer to the rendered HTML report.
// The LLM extraction is invoked through an injectable seam so the command
// itself never performs a network call during normal command execution
// unless the caller wires a real client-backed seam.
// ============================================================================
class InsightsCommand {
public:
    /// Default analysis model id (Opus). Mirrors the TS getAnalysisModel().
    [[nodiscard]] static constexpr std::string_view default_analysis_model() {
        return "claude-opus-4-20250514";
    }

    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "insights",
            .description =
                "Report local usage statistics and session facet analysis",
            .args = {},
            .category = "tools",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] static VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] static Result<CommandResult> execute(const CommandContext&) {
        auto sessions = cc::utils::list_sessions(std::nullopt);

        if (sessions.empty()) {
            return CommandResult::success(
                "No sessions found in local storage (~/.claude/sessions/).\n"
                "Usage insights are generated from recorded session "
                "transcripts.");
        }

        std::size_t total_messages = 0;
        std::size_t with_model = 0;
        std::map<std::string, std::size_t> by_model;
        for (const auto& s : sessions) {
            total_messages += s.message_count;
            if (!s.model.empty()) {
                by_model[s.model] += 1;
                ++with_model;
            }
        }

        std::string out = "Local usage insights:\n";
        out += std::format("  Total sessions: {}\n", sessions.size());
        out += std::format("  Total messages: {}\n", total_messages);
        if (total_messages > 0) {
            out += std::format(
                "  Avg messages/session: {:.1}\n",
                static_cast<double>(total_messages) /
                    static_cast<double>(sessions.size()));
        }
        out += std::format("  Sessions with a recorded model: {}\n",
                           with_model);

        if (!by_model.empty()) {
            out += "\n  Sessions by model:\n";
            std::vector<std::pair<std::string, std::size_t>> sorted(
                by_model.begin(), by_model.end());
            std::ranges::sort(sorted, [](const auto& a, const auto& b) {
                return a.second > b.second;
            });
            for (const auto& [model, count] : sorted) {
                out += std::format("    {:<30} {}\n", model, count);
            }
        }

        out += "\n  Most recent sessions:\n";
        std::size_t shown = 0;
        for (const auto& s : sessions) {
            if (shown++ >= 5) break;
            out += std::format("    {} ({} messages", s.id, s.message_count);
            if (!s.model.empty()) out += std::format(", {}", s.model);
            out += ")\n";
        }

        // Load any cached facets for the known sessions and render the
        // aggregate facet breakdown + HTML report when facets exist. This is
        // the deterministic half of the TS pipeline; live extraction is the
        // caller's responsibility (see LlmExtractFn).
        std::vector<SessionFacets> facets;
        facets.reserve(sessions.size());
        for (const auto& s : sessions) {
            if (auto f = load_facets(s.id)) facets.push_back(std::move(*f));
        }

        if (!facets.empty()) {
            auto agg = aggregate_facets(facets);
            out += std::format("\n  Facet analysis ({} sessions):\n",
                               agg.sessions_with_facets);
            out += render_facet_summary(agg);

            // Best-effort HTML report write. Date range is derived from the
            // sorted session list's last_active timestamps (the session store
            // exposes filesystem write times, not transcript timestamps).
            std::string date_start;
            std::string date_end;
            if (!sessions.empty()) {
                date_end = format_date(sessions.front().last_active);
                date_start = format_date(sessions.back().last_active);
            }
            auto html = render_html_report(sessions.size(), total_messages,
                                           date_start, date_end, agg);
            auto report_path = detail::usage_data_dir() / "report.html";
            std::error_code ec;
            fs::create_directories(detail::usage_data_dir(), ec);
            if (std::ofstream f(report_path, std::ios::binary | std::ios::trunc);
                f) {
                f << html;
                out += std::format("\n  HTML report written to: {}\n",
                                   report_path.string());
            }
        } else {
            out += "\nNote: no cached session facets found. The TS CLI "
                   "additionally runs LLM facet extraction "
                   "(goal/outcome/satisfaction) per session via the Claude "
                   "API and renders an HTML report. In this build extraction "
                   "is exposed through an injectable LlmExtractFn seam "
                   "(cc::commands::extract_facets_with_seam); wire a "
                   "client-backed seam to populate the facet cache, then "
                   "re-run /insights to render the report.\n";
        }
        return CommandResult::success(std::move(out));
    }

    [[nodiscard]] static std::vector<std::string>
    complete(std::string_view) {
        return {};
    }

private:
    /// Format a time_point as YYYY-MM-DD for the report date range.
    [[nodiscard]] static std::string
    format_date(std::chrono::system_clock::time_point tp) {
        const auto t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[16] = {};
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        return std::string(buf);
    }

    /// Render the aggregate facet breakdown as a compact text summary for the
    /// command's stdout output. Top-3 per dimension, humanised labels.
    [[nodiscard]] static std::string
    render_facet_summary(const AggregatedFacets& agg) {
        auto render_top = [](const std::map<std::string, std::size_t>& m,
                             std::size_t n) {
            if (m.empty()) return std::string("    (none)\n");
            std::vector<std::pair<std::string, std::size_t>> rows(m.begin(),
                                                                  m.end());
            std::sort(rows.begin(), rows.end(),
                      [](const auto& a, const auto& b) {
                          if (a.second != b.second) return a.second > b.second;
                          return a.first < b.first;
                      });
            std::string s;
            std::size_t i = 0;
            for (const auto& [k, v] : rows) {
                if (i++ >= n) break;
                s += std::format("      {} ({})\n", humanise(k), v);
            }
            return s;
        };
        std::string s;
        s += "    Top goals:\n";
        s += render_top(agg.goal_categories, 3);
        s += "    Outcomes:\n";
        s += render_top(agg.outcomes, 3);
        s += "    Top friction:\n";
        s += render_top(agg.friction, 3);
        return s;
    }
};

} // namespace cc::commands
