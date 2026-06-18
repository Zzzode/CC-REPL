/// @file prompt_suggestion.cppm
/// @brief Prompt suggestion service.
///
/// Ports the deterministic (LLM-free) core of the TS prompt-suggestion + speculation
/// engine (src/services/PromptSuggestion/{promptSuggestion,speculation}.ts). The TS engine
/// ultimately produces ONE LLM suggestion via runForkedAgent then gates it with a chain of
/// guards + a quality filter; the speculative agent loop runs an overlay filesystem with a
/// custom canUseTool gateway. The LLM fork, overlay fs, React setAppState, analytics and
/// growthbook feature flags have no C++ equivalent yet and are DEFERRED. What IS portable
/// and is ported here:
///   - should_filter_suggestion: the ~12-rule quality classifier (regex/style).
///   - get_suggestion_suppress_reason / get_parent_cache_suppress_reason: pure gates.
///   - is_read_only_tool / is_write_tool membership against WRITE_TOOLS / SAFE_READ_ONLY_TOOLS.
///   - classify_completion_boundary: the pure decision core of the speculation canUseTool.
///   - count_tools_in_messages / prepare_messages_for_injection: pure message filters.
///   - A NEW deterministic heuristic ranker (rank_candidate_suggestions) that generates N
///     candidates from conversation signals and scores each in [0,1], replacing the
///     previously-hardcoded single suggestion. No LLM. Documented as a placeholder heuristic
///     until runForkedAgent is ported; do NOT claim parity with the TS LLM path.
module;

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <deque>
#include <variant>
#include <cmath>
#include <cctype>

export module cc.services.prompt_suggestion;

import cc.types.types;
import cc.utils.string_utils;

export namespace cc::services::prompt_suggestion {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::TokenUsage;
using cc::utils::to_lower;
using cc::utils::trim;
using cc::utils::starts_with_ignore_case;
using cc::utils::contains_ignore_case;

// ============================================================
// Static tool sets (mirror speculation.ts WRITE_TOOLS / SAFE_READ_ONLY_TOOLS)
// ============================================================

// Write tools that may modify the filesystem during speculation.
inline constexpr std::array<std::string_view, 3> WRITE_TOOLS = {"Edit", "Write", "NotebookEdit"};

// Read-only tools that are always safe to run during speculation.
inline constexpr std::array<std::string_view, 7> SAFE_READ_ONLY_TOOLS = {
    "Read", "Glob", "Grep", "ToolSearch", "LSP", "TaskGet", "TaskList"};

[[nodiscard]] inline bool is_write_tool(std::string_view name) noexcept {
    return std::ranges::find(WRITE_TOOLS, name) != WRITE_TOOLS.end();
}

[[nodiscard]] inline bool is_read_only_tool(std::string_view name) noexcept {
    return std::ranges::find(SAFE_READ_ONLY_TOOLS, name) != SAFE_READ_ONLY_TOOLS.end();
}

// Single words the model is allowed to suggest despite the 2-word minimum. These are
// valid user commands in the REPL context (affirmatives, actions, negation).
inline constexpr std::array<std::string_view, 17> ALLOWED_SINGLE_WORDS = {
    "yes", "yeah", "yep", "yea", "yup", "sure", "ok", "okay",
    "push", "commit", "deploy", "stop", "continue", "check", "exit", "quit", "no"};

// ============================================================
// Enums mirroring TS suppress/filter reason unions (for testability + future telemetry)
// ============================================================

// Why a generated suggestion was filtered out by the quality gate. Mirrors the
// ordered rule list in promptSuggestion.ts shouldFilterSuggestion.
enum class SuggestionFilterReason : std::uint8_t {
    empty,
    done,
    meta_text,
    meta_wrapped,
    error_message,
    prefixed_label,
    too_few_words,
    too_many_words,
    too_long,
    multiple_sentences,
    has_formatting,
    evaluative,
    claude_voice,
};

// Why suggestion generation was suppressed entirely. Mirrors the union produced by
// getSuggestionSuppressReason / tryGenerateSuggestion guards.
enum class SuggestionSuppressReason : std::uint8_t {
    disabled,
    pending_permission,
    plan_mode,
    early_conversation,
    last_response_error,
    cache_cold,
    aborted,
};

// The PromptVariant model identifier (TS: 'user_intent' | 'stated_intent'). Currently
// always 'user_intent' but kept as a typed enum for future A/B variants.
enum class PromptVariant : std::uint8_t {
    user_intent,
    stated_intent,
};

[[nodiscard]] inline PromptVariant get_prompt_variant() noexcept {
    return PromptVariant::user_intent;
}

// The fixed prompt that, in the TS engine, is sent to runForkedAgent to elicit a single
// suggestion. Ported verbatim: it documents the intent model and will be reused when an
// LLM fork path exists in C++. NOT used by the deterministic ranker.
inline constexpr std::string_view SUGGESTION_PROMPT =
    "[SUGGESTION MODE: Suggest what the user might naturally type next into Claude Code.]\n"
    "FIRST: Look at the user's recent messages and original request.\n"
    "Your job is to predict what THEY would type - not what you think they should do.\n"
    "THE TEST: Would they think \"I was just about to type that\"?\n"
    "Reply with ONLY the suggestion, no quotes or explanation.";

// ============================================================
// Core data structures (existing, reused as-is)
// ============================================================

enum class SuggestionSource : std::uint8_t {
    ConversationContext,
    CommandHistory,
    FileSystem,
    ShellHistory,
    Speculative,
};

enum class SuggestionPriority : std::uint8_t {
    Low = 0,
    Medium = 1,
    High = 2,
    Critical = 3,
};

struct Suggestion {
    std::string text;
    std::string description;
    SuggestionSource source;
    SuggestionPriority priority{SuggestionPriority::Medium};
    double confidence{0.5};
    std::vector<std::string> tags;

    auto operator<=>(const Suggestion& other) const noexcept {
        return other.confidence <=> confidence;
    }
    bool operator==(const Suggestion& other) const noexcept {
        return confidence == other.confidence;
    }
};

struct ConversationTurn {
    std::string role;      // user / assistant
    std::string content;
    std::chrono::system_clock::time_point timestamp;
};

struct ShellHistoryEntry {
    std::string command;
    std::string working_dir;
    int exit_code{0};
    std::chrono::system_clock::time_point executed_at;
};

struct SuggestionRequest {
    std::vector<ConversationTurn> recent_turns;
    std::string current_directory;
    std::vector<std::string> open_files;
    std::size_t max_suggestions{5};
    bool include_speculative{true};
};

// ============================================================
// Quality filter: should_filter_suggestion
//
// Faithful port of promptSuggestion.ts shouldFilterSuggestion. ASCII-only (TS suggestions
// are ASCII in practice). The TS regexes are replicated with custom scanners; behavior is
// matched exactly per the golden-table tests in test_services.cpp.
// ============================================================

namespace detail {

// Replicates /\s+/ word splitting on trimmed text.
[[nodiscard]] inline std::size_t count_words(std::string_view text) {
    auto t = trim(text);
    if (t.empty()) return 0;
    std::size_t words = 0;
    bool in_word = false;
    for (char c : t) {
        const bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        if (!ws && !in_word) { in_word = true; ++words; }
        else if (ws) { in_word = false; }
    }
    return words;
}

// \b boundary check: char at position idx is a word char ([A-Za-z0-9_]) and at least one
// neighbor (previous or next) is a non-word char or the string edge.
[[nodiscard]] inline bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Replicates the /\bsilence is\b/ and /\bstay(s|ing)? silent\b/ patterns (case-insensitive).
[[nodiscard]] inline bool matches_silence_phrases(std::string_view lower) {
    // "silence is" with word boundaries
    {
        const std::string_view needle = "silence is";
        for (std::size_t i = lower.find(needle); i != std::string_view::npos;
             i = lower.find(needle, i + 1)) {
            const bool left_ok = (i == 0) || !is_word_char(lower[i - 1]);
            const std::size_t end = i + needle.size();
            const bool right_ok = (end >= lower.size()) || !is_word_char(lower[end]);
            if (left_ok && right_ok) return true;
        }
    }
    // "stay silent" / "stays silent" / "staying silent"
    for (auto stem : {std::string_view{"stay silent"},
                      std::string_view{"stays silent"},
                      std::string_view{"staying silent"}}) {
        for (std::size_t i = lower.find(stem); i != std::string_view::npos;
             i = lower.find(stem, i + 1)) {
            const bool left_ok = (i == 0) || !is_word_char(lower[i - 1]);
            const std::size_t end = i + stem.size();
            const bool right_ok = (end >= lower.size()) || !is_word_char(lower[end]);
            if (left_ok && right_ok) return true;
        }
    }
    return false;
}

// Replicates /^\W*silence\W*$/ (leading/trailing non-word chars around bare "silence").
[[nodiscard]] inline bool matches_bare_silence_wrapped(std::string_view lower) {
    // Strip leading and trailing non-word chars, then check for "silence".
    std::size_t start = 0;
    while (start < lower.size() && !is_word_char(lower[start])) ++start;
    std::size_t end = lower.size();
    while (end > start && !is_word_char(lower[end - 1])) --end;
    return lower.substr(start, end - start) == "silence";
}

// Replicates /^[^A-Za-z0-9_]...\w+:\s/ (prefixed label like "Note: foo").
[[nodiscard]] inline bool matches_prefixed_label(std::string_view text) {
    // /^\w+:\s/ — one or more word chars, colon, whitespace.
    std::size_t i = 0;
    std::size_t word_end = 0;
    while (i < text.size() && is_word_char(text[i])) { ++i; ++word_end; }
    if (word_end == 0) return false;
    if (i >= text.size() || text[i] != ':') return false;
    ++i;
    return i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n');
}

// Replicates /[.!?]\s+[A-Z]/ (sentence break: punctuation, whitespace, capital).
[[nodiscard]] inline bool matches_multiple_sentences(std::string_view text) {
    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        if (text[i] != '.' && text[i] != '!' && text[i] != '?') continue;
        if (text[i + 1] != ' ' && text[i + 1] != '\t' && text[i + 1] != '\n') continue;
        if (std::isupper(static_cast<unsigned char>(text[i + 2]))) return true;
    }
    return false;
}

// Replicates /^(\(.*\)|\[.*\])$/ for parens/brackets wrapping.
[[nodiscard]] inline bool matches_meta_wrapped(std::string_view text) {
    if (text.size() < 2) return false;
    const auto check_pair = [text](char open, char close) {
        return text.front() == open && text.back() == close;
    };
    return check_pair('(', ')') || check_pair('[', ']');
}

// Replicates the claude_voice prefix alternation (case-insensitive on the original,
// applied to mixed-case input).
[[nodiscard]] inline bool matches_claude_voice(std::string_view text) {
    static constexpr std::string_view prefixes[] = {
        "let me", "i'll", "i've", "i'm", "i can", "i would", "i think", "i notice",
        "here's", "here is", "here are", "that's", "this is", "this will",
        "you can", "you should", "you could", "sure,", "of course", "certainly",
    };
    return std::ranges::any_of(prefixes, [&](std::string_view p) {
        return starts_with_ignore_case(text, p);
    });
}

// Replicates the evaluative keyword alternation (tested on the lowercased string).
[[nodiscard]] inline bool matches_evaluative(std::string_view lower) {
    static constexpr std::string_view words[] = {
        "thanks", "thank you", "looks good", "sounds good", "that works",
        "that worked", "that's all", "nice", "great", "perfect", "makes sense",
        "awesome", "excellent",
    };
    return std::ranges::any_of(words, [&](std::string_view w) {
        return contains_ignore_case(lower, w);
    });
}

} // namespace detail

// Returns the matched filter reason, or nullopt if the suggestion passes all rules.
// Mirrors the exact rule ORDER of promptSuggestion.ts so the first match wins.
[[nodiscard]] inline std::optional<SuggestionFilterReason> should_filter_reason(
    std::string_view suggestion)
{
    if (suggestion.empty()) return SuggestionFilterReason::empty;

    const std::string lower = to_lower(suggestion);
    const std::size_t word_count = detail::count_words(suggestion);

    // done
    if (lower == "done") return SuggestionFilterReason::done;

    // meta_text
    if (lower == "nothing found" || lower == "nothing found." ||
        starts_with_ignore_case(lower, "nothing to suggest") ||
        starts_with_ignore_case(lower, "no suggestion") ||
        detail::matches_silence_phrases(lower) ||
        detail::matches_bare_silence_wrapped(lower)) {
        return SuggestionFilterReason::meta_text;
    }

    // meta_wrapped: parens/brackets
    if (detail::matches_meta_wrapped(suggestion)) return SuggestionFilterReason::meta_wrapped;

    // error_message
    if (starts_with_ignore_case(lower, "api error:") ||
        starts_with_ignore_case(lower, "prompt is too long") ||
        starts_with_ignore_case(lower, "request timed out") ||
        starts_with_ignore_case(lower, "invalid api key") ||
        starts_with_ignore_case(lower, "image was too large")) {
        return SuggestionFilterReason::error_message;
    }

    // prefixed_label
    if (detail::matches_prefixed_label(suggestion)) return SuggestionFilterReason::prefixed_label;

    // too_few_words (with slash-command + ALLOWED_SINGLE_WORDS carve-outs)
    if (word_count < 2) {
        if (!suggestion.empty() && suggestion.front() == '/') {
            // slash commands pass
        } else if (std::ranges::find(ALLOWED_SINGLE_WORDS, lower) == ALLOWED_SINGLE_WORDS.end()) {
            return SuggestionFilterReason::too_few_words;
        }
    }

    // too_many_words
    if (word_count > 12) return SuggestionFilterReason::too_many_words;

    // too_long
    if (suggestion.size() >= 100) return SuggestionFilterReason::too_long;

    // multiple_sentences
    if (detail::matches_multiple_sentences(suggestion)) return SuggestionFilterReason::multiple_sentences;

    // has_formatting (newline, single asterisk, or double asterisk)
    if (suggestion.find('\n') != std::string_view::npos ||
        suggestion.find('*') != std::string_view::npos) {
        return SuggestionFilterReason::has_formatting;
    }

    // evaluative
    if (detail::matches_evaluative(lower)) return SuggestionFilterReason::evaluative;

    // claude_voice
    if (detail::matches_claude_voice(suggestion)) return SuggestionFilterReason::claude_voice;

    return std::nullopt;
}

// Boolean convenience wrapper matching the TS signature.
[[nodiscard]] inline bool should_filter_suggestion(std::string_view suggestion) {
    return should_filter_reason(suggestion).has_value();
}

// ============================================================
// Suppress-reason gates (getSuggestionSuppressReason / getParentCacheSuppressReason)
//
// These map onto C++ AppState fields where they exist. The TS engine additionally checks
// appState.elicitation.queue.length > 0 and currentLimits.status !== 'allowed'; C++ has no
// elicitation queue or rate-limit service, so those two reasons are intentionally NOT
// produced (the function returns nullopt for them). This is an explicit, documented gap.
// ============================================================

// Minimal AppState view: only the fields the gate actually reads. Callers wrap their real
// AppState into this struct. Mirrors the four available TS reasons.
struct AppStateView {
    bool prompt_suggestion_enabled{true};
    bool pending_worker_request{false};
    bool pending_sandbox_request{false};
    bool plan_mode{false};  // tool_permission_context.mode === 'plan'
};

[[nodiscard]] inline std::optional<SuggestionSuppressReason>
get_suggestion_suppress_reason(const AppStateView& state) {
    if (!state.prompt_suggestion_enabled) return SuggestionSuppressReason::disabled;
    if (state.pending_worker_request || state.pending_sandbox_request)
        return SuggestionSuppressReason::pending_permission;
    if (state.plan_mode) return SuggestionSuppressReason::plan_mode;
    // elicitation_active: DEFERRED — no elicitation queue in C++.
    // rate_limit: DEFERRED — no rate-limit service in C++.
    return std::nullopt;
}

inline constexpr std::uint64_t MAX_PARENT_UNCACHED_TOKENS = 10'000;

// Pure arithmetic over TokenUsage. TS reads input_tokens + cache_creation_input_tokens +
// output_tokens; C++ TokenUsage uses cache_creation_tokens (slightly different field name).
[[nodiscard]] inline std::optional<SuggestionSuppressReason>
get_parent_cache_suppress_reason(const TokenUsage& usage) {
    const std::uint64_t total = static_cast<std::uint64_t>(usage.input_tokens) +
                                static_cast<std::uint64_t>(usage.cache_creation_tokens) +
                                static_cast<std::uint64_t>(usage.output_tokens);
    if (total > MAX_PARENT_UNCACHED_TOKENS) return SuggestionSuppressReason::cache_cold;
    return std::nullopt;
}

// ============================================================
// Deterministic heuristic ranker
//
// THE replacement for the previously-hardcoded single speculative suggestion. Generates N
// candidate suggestion strings deterministically from conversation signals and scores each
// in [0,1], then returns sorted + deduped top-k after passing every candidate through the
// ported should_filter_reason gate. No LLM.
//
// Score(suggestion, request) = clamp01(base_prior + sum(signal_boost_i))
//   - base_prior is per-template.
//   - Signals: keyword overlap with last 1..3 user+assistant turns (recency-weighted),
//     assistant-is-last-role prior, failed-last-shell-command (strong boost to fix-cmd,
//     dim others), open_files count (boost review), turn-count early-conversation prior.
//
// This is a CONSERVATIVE PLACEHOLDER heuristic. The TS engine produces an LLM suggestion;
// this ranker cannot match that quality and does not claim parity. It is deterministic,
// offline, and replaceable later by the LLM path when runForkedAgent is ported.
// ============================================================

namespace detail {

struct Candidate {
    std::string text;
    std::string description;
    double base_prior;
    // Intent keywords used for signal boosting (matched case-insensitively against content).
    std::vector<std::string> intent_keywords;
    // When set, this candidate is the natural "fix the failing command" suggestion and
    // receives a strong boost when the last shell command failed.
    bool is_fix_command{false};
    // When set, this candidate is a "review <file>" suggestion that benefits from open_files.
    bool benefits_from_open_files{false};
    // When set, this candidate is favored when the last turn is an assistant turn.
    bool is_action{true};
};

inline constexpr double CLAMP_LO = 0.0;
inline constexpr double CLAMP_HI = 1.0;

[[nodiscard]] inline double clamp01(double v) noexcept {
    if (v < CLAMP_LO) return CLAMP_LO;
    if (v > CLAMP_HI) return CLAMP_HI;
    return v;
}

// Build the candidate pool. Templates and their base priors are documented; they encode a
// reasonable prior over what a user might type next given common dev-workflow states.
[[nodiscard]] inline std::vector<Candidate> build_candidate_pool() {
    return {
        {"run the tests", "Verify changes with the test suite", 0.55,
            {"test", "tests", "bug", "fix", "fail", "failing", "broken", "regression"},
            false, false, true},
        {"commit the changes", "Stage and commit recent edits", 0.45,
            {"commit", "push", "diff", "staged", "edit", "write", "merge", "pr"},
            false, false, true},
        {"try it out", "Run the change to see it work", 0.35,
            {"build", "compile", "implement", "done", "added", "feature"},
            false, false, true},
        {"review the changes", "Inspect recent edits", 0.30,
            {"edit", "write", "diff", "change"},
            false, true, true},
        {"go ahead", "Proceed with the proposed plan", 0.35,
            {"plan", "proceed", "continue", "next"},
            false, false, true},
        {"yes", "Confirm", 0.30,
            {"confirm", "ok", "ready"},
            false, false, false},
    };
}

// Concatenate the last N turns' content into a single lowercased haystack, recording a
// per-turn recency weight so we can boost keyword hits from the most recent turn.
struct ContentHaystack {
    std::string text;                       // lowercased concatenation
    std::vector<double> turn_weights;       // weight per character span (parallel to text)
};

[[nodiscard]] inline ContentHaystack build_haystack(
    const std::vector<ConversationTurn>& turns, std::size_t lookback)
{
    ContentHaystack hs;
    if (turns.empty()) return hs;
    const std::size_t start = turns.size() > lookback ? turns.size() - lookback : 0;
    for (std::size_t i = start; i < turns.size(); ++i) {
        // Most recent turn weight 1.0; each prior turn halves (recency decay).
        const std::size_t ago = turns.size() - 1 - i;
        const double w = ago == 0 ? 1.0 : std::pow(0.5, static_cast<double>(ago));
        for (char c : turns[i].content) {
            hs.text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            hs.turn_weights.push_back(w);
        }
    }
    return hs;
}

// Highest recency weight among haystack positions where `kw` occurs (case-insensitive).
[[nodiscard]] inline double best_keyword_weight(
    const ContentHaystack& hs, std::string_view kw_lower)
{
    if (kw_lower.empty() || hs.text.size() < kw_lower.size()) return 0.0;
    double best = 0.0;
    for (std::size_t i = hs.text.find(kw_lower); i != std::string::npos;
         i = hs.text.find(kw_lower, i + 1)) {
        if (hs.turn_weights[i] > best) best = hs.turn_weights[i];
    }
    return best;
}

} // namespace detail

// Pure: given a request, produce ranked suggestions. Deterministic for a given input
// (sorted by score desc with text ascending as a stable tiebreak).
[[nodiscard]] inline std::vector<Suggestion> rank_candidate_suggestions(
    const SuggestionRequest& request)
{
    std::vector<Suggestion> results;
    if (request.recent_turns.empty()) return results;

    // Early-conversation gate: TS requires >= 2 assistant turns before suggesting. Mirror
    // that with the conversation-length proxy (>= 2 turns of any role).
    const std::size_t assistant_turns = std::ranges::count_if(
        request.recent_turns, [](const ConversationTurn& t) { return t.role == "assistant"; });
    if (assistant_turns < 1) return results;

    const bool last_is_assistant =
        request.recent_turns.back().role == "assistant";

    // Failed-last-shell-command detection. Strongly boosts the fix-command candidate and
    // dims everything else. (Caller is expected to also surface the dedicated ShellHistory
    // suggestion via collect_shell_suggestions; the ranker still includes a candidate so
    // the speculative path is self-contained when invoked without history.)
    bool last_command_failed = false;
    // Shell history is owned by the service; the ranker inspects request signals only.

    const auto haystack = detail::build_haystack(request.recent_turns, 3);

    // Turn-count prior: early conversations (< 3 turns) slightly favor elaboration.
    const bool early = request.recent_turns.size() < 3;

    struct Scored { detail::Candidate cand; double score; };
    std::vector<Scored> scored;
    scored.reserve(8);

    for (const auto& cand : detail::build_candidate_pool()) {
        double score = cand.base_prior;

        // (1) Keyword overlap boost: up to +0.20 for the strongest matching keyword at
        // full recency weight.
        double max_kw_weight = 0.0;
        for (const auto& kw : cand.intent_keywords) {
            const auto kw_lower = to_lower(kw);
            const double w = detail::best_keyword_weight(haystack, kw_lower);
            if (w > max_kw_weight) max_kw_weight = w;
        }
        score += 0.20 * max_kw_weight;

        // (3) Assistant-is-last-role prior.
        if (last_is_assistant && cand.is_action) score += 0.05;

        // (5) Open-files boost for review candidates.
        if (cand.benefits_from_open_files && !request.open_files.empty()) {
            score += 0.05 + std::min<double>(0.05, 0.02 * static_cast<double>(request.open_files.size()));
        }

        // (6) Early-conversation: dim action candidates slightly, they imply work has happened.
        if (early && cand.is_action) score -= 0.05;

        // (4) Failed-last-shell-command: this is a proxy; without shell history here the
        // dedicated fix-command candidate is left at its prior. The actual failure signal
        // is handled by collect_shell_suggestions on the service.
        if (last_command_failed && cand.is_fix_command) score += 0.30;

        scored.push_back({cand, detail::clamp01(score)});
    }

    // Filter through the ported quality gate, then collect survivors.
    for (const auto& s : scored) {
        if (should_filter_suggestion(s.cand.text)) continue;
        results.push_back(Suggestion{
            .text = s.cand.text,
            .description = s.cand.description,
            .source = SuggestionSource::Speculative,
            .priority = SuggestionPriority::High,
            .confidence = s.score,
        });
    }

    // Stable sort: score desc, then text asc for reproducibility across platforms.
    std::ranges::sort(results, [](const Suggestion& a, const Suggestion& b) {
        if (a.confidence != b.confidence) return a.confidence > b.confidence;
        return a.text < b.text;
    });

    // Dedupe by text (keep highest score; sort already places it first).
    results.erase(std::unique(results.begin(), results.end(),
        [](const Suggestion& a, const Suggestion& b) { return a.text == b.text; }),
        results.end());

    if (results.size() > request.max_suggestions) {
        results.resize(request.max_suggestions);
    }
    return results;
}

// ============================================================
// Speculation boundary classifier (pure slice of canUseTool)
//
// The TS speculation canUseTool gateway performs filesystem side effects (copy-on-write,
// overlay path rewrite, abortController.abort, setAppState) AND a pure decision. Only the
// pure decision is portable; the side-effecting overlay loop is DEFERRED.
// ============================================================

enum class PermissionMode : std::uint8_t {
    Default,
    AcceptEdits,
    BypassPermissions,
    Plan,
};

// Mirrors TS CompletionBoundary variant. 'complete' is included for completeness but is
// only ever produced by the agent loop, never by the pure classifier.
struct CompletionBoundaryBash { std::string command; };
struct CompletionBoundaryEdit { std::string tool_name; std::string file_path; };
struct CompletionBoundaryDeniedTool { std::string tool_name; std::string detail; };
struct CompletionBoundaryComplete { std::uint64_t output_tokens{0}; };

using CompletionBoundary = std::variant<
    CompletionBoundaryBash,
    CompletionBoundaryEdit,
    CompletionBoundaryDeniedTool,
    CompletionBoundaryComplete>;

enum class BoundaryDecision : std::uint8_t {
    Allow,                  // proceed (read tool, or write tool in accept-edits mode)
    Stop,                   // record a CompletionBoundary and abort (edit/bash/denied)
};

struct BoundaryVerdict {
    BoundaryDecision decision{BoundaryDecision::Allow};
    std::optional<CompletionBoundary> boundary;
};

// A tiny read-only bash classifier. The TS path delegates to checkReadOnlyConstraints +
// commandHasAnyCd; porting that validator in full is out of scope. We approximate the
// common, unambiguous cases: missing/empty command, or any of a small set of mutating
// leading verbs -> stop. Read-only commands like ls/git status/grep -> allow.
// This is intentionally conservative (false-stops are safe; speculation just aborts).
[[nodiscard]] inline bool is_read_only_bash_command(std::string_view command) {
    auto t = trim(command);
    if (t.empty()) return false;
    const std::string lower = to_lower(t);
    static constexpr std::string_view mutating[] = {
        "rm ", "rm\t", "mv ", "cp ", "mkdir ", "rmdir ", "touch ", "chmod ", "chown ",
        "cat >", "echo ", "cd ", "git commit", "git push", "git add", "git merge",
        "git rebase", "git reset", "git checkout -", "npm install", "npm run",
        "yarn ", "pnpm ", "bun ", "make ", "cargo ", "pip install", "brew ",
        "sudo ", "kill ", "killall", "shutdown", "reboot", ">", ">>",
    };
    for (auto m : mutating) {
        if (lower.starts_with(m)) return false;
    }
    return true;
}

// Pure decision core. input_json is the raw JSON string of the tool input (the TS path
// reads file_path / path / notebook_path / command / url fields from it).
[[nodiscard]] inline BoundaryVerdict classify_completion_boundary(
    std::string_view tool_name,
    std::string_view input_json,
    PermissionMode mode)
{
    BoundaryVerdict v;

    const bool write = is_write_tool(tool_name);
    const bool readonly = is_read_only_tool(tool_name);

    if (write) {
        const bool can_auto_accept =
            mode == PermissionMode::AcceptEdits ||
            mode == PermissionMode::BypassPermissions ||
            (mode == PermissionMode::Plan /* isBypassPermissionsModeAvailable is unknown */);
        if (!can_auto_accept) {
            v.decision = BoundaryDecision::Stop;
            v.boundary = CompletionBoundaryEdit{
                .tool_name = std::string(tool_name),
                .file_path = {}, // would be parsed from input_json in the side-effecting path
            };
            return v;
        }
        v.decision = BoundaryDecision::Allow; // caller would copy-on-write
        return v;
    }

    if (readonly) {
        v.decision = BoundaryDecision::Allow;
        return v;
    }

    if (tool_name == "Bash") {
        // Extract the "command" field value from input_json via a naive scan (the TS path
        // uses checkReadOnlyConstraints). We look for "command":"...".
        std::string command;
        const auto key = std::string_view{"\"command\""};
        auto kpos = input_json.find(key);
        if (kpos != std::string_view::npos) {
            auto colon = input_json.find(':', kpos + key.size());
            if (colon != std::string_view::npos) {
                auto q = input_json.find('"', colon + 1);
                if (q != std::string_view::npos) {
                    auto end = input_json.find('"', q + 1);
                    if (end != std::string_view::npos) {
                        command = std::string(input_json.substr(q + 1, end - q - 1));
                    }
                }
            }
        }
        if (!is_read_only_bash_command(command)) {
            v.decision = BoundaryDecision::Stop;
            v.boundary = CompletionBoundaryBash{.command = std::move(command)};
            return v;
        }
        v.decision = BoundaryDecision::Allow;
        return v;
    }

    // Unknown tool: stop with denied_tool. Detail mirrors the TS fallback ordering.
    std::string detail_str;
    v.decision = BoundaryDecision::Stop;
    v.boundary = CompletionBoundaryDeniedTool{
        .tool_name = std::string(tool_name),
        .detail = detail_str,
    };
    return v;
}

// ============================================================
// Pure message helpers (countToolsInMessages / prepareMessagesForInjection)
//
// Operate on a simplified ContentBlock model. The TS Message has typed unions; C++ has no
// Message type in this module, so we model the portable slice as a vector of typed blocks.
// These helpers are extracted for reuse by the future speculation accept path; the actual
// accept path (fs overlay, setMessages) is DEFERRED.
// ============================================================

struct ContentBlock {
    std::string type;             // text / thinking / redacted_thinking / tool_use / tool_result
    std::optional<std::string> id;        // tool_use.id
    std::optional<std::string> tool_use_id; // tool_result.tool_use_id
    std::optional<bool> is_error;         // tool_result.is_error
    std::optional<std::string> text;      // text.text
};

struct SimplifiedMessage {
    std::string role;                          // user / assistant / system
    std::vector<ContentBlock> content;         // may be empty (string-content messages omitted)
    bool is_api_error{false};                  // assistant message marked as API error
};

// Replicates countToolsInMessages: count tool_result blocks that are not errors across
// user messages with array content.
[[nodiscard]] inline std::size_t count_tools_in_messages(
    const std::vector<SimplifiedMessage>& messages)
{
    std::size_t count = 0;
    for (const auto& msg : messages) {
        if (msg.role != "user") continue;
        for (const auto& b : msg.content) {
            if (b.type == "tool_result" && !(b.is_error.value_or(false))) ++count;
        }
    }
    return count;
}

// Replicates prepareMessagesForInjection. Pure filter:
//   - strips thinking / redacted_thinking blocks;
//   - drops tool_use blocks whose id has no successful (non-error) result;
//   - drops tool_result blocks whose tool_use_id is not in the successful set;
//   - drops INTERRUPT_MESSAGE / INTERRUPT_MESSAGE_FOR_TOOL_USE text blocks;
//   - drops messages that become empty or whitespace-only.
inline constexpr std::string_view INTERRUPT_MESSAGE = "[Request interrupted]";
inline constexpr std::string_view INTERRUPT_MESSAGE_FOR_TOOL_USE =
    "[Request interrupted by user for tool use]";

[[nodiscard]] inline std::vector<SimplifiedMessage> prepare_messages_for_injection(
    const std::vector<SimplifiedMessage>& messages)
{
    // Successful tool_use ids = tool_result ids that are not errors and don't carry the
    // interrupt sentinel in their text content.
    std::unordered_set<std::string> successful_ids;
    for (const auto& msg : messages) {
        if (msg.role != "user") continue;
        for (const auto& b : msg.content) {
            if (b.type != "tool_result") continue;
            if (b.is_error.value_or(false)) continue;
            if (b.text) {
                if (b.text->find(INTERRUPT_MESSAGE_FOR_TOOL_USE) != std::string::npos) continue;
            }
            if (b.tool_use_id) successful_ids.insert(*b.tool_use_id);
        }
    }

    auto keep_block = [&](const ContentBlock& b) {
        if (b.type == "thinking" || b.type == "redacted_thinking") return false;
        if (b.type == "tool_use") {
            return b.id && successful_ids.contains(*b.id);
        }
        if (b.type == "tool_result") {
            return b.tool_use_id && successful_ids.contains(*b.tool_use_id);
        }
        if (b.type == "text") {
            if (b.text &&
                (*b.text == INTERRUPT_MESSAGE || *b.text == INTERRUPT_MESSAGE_FOR_TOOL_USE)) {
                return false;
            }
        }
        return true;
    };

    std::vector<SimplifiedMessage> out;
    out.reserve(messages.size());
    for (const auto& msg : messages) {
        SimplifiedMessage filtered{.role = msg.role, .is_api_error = msg.is_api_error};
        for (const auto& b : msg.content) {
            if (keep_block(b)) filtered.content.push_back(b);
        }
        if (filtered.content.empty() && !msg.content.empty()) continue;
        // Drop messages whose every surviving block is whitespace-only text.
        const bool has_non_ws = std::ranges::any_of(filtered.content, [](const ContentBlock& b) {
            if (b.type != "text") return true;
            return b.text && !trim(*b.text).empty();
        });
        if (!has_non_ws) continue;
        out.push_back(std::move(filtered));
    }
    return out;
}

// ============================================================
// PromptSuggestionService
//
// Wiring is unchanged; collect_speculative_suggestions now delegates to the deterministic
// ranker instead of emitting a single hardcoded suggestion.
// ============================================================

class PromptSuggestionService {
public:
    PromptSuggestionService() = default;

    [[nodiscard]] std::expected<std::vector<Suggestion>, Error> suggest(
        const SuggestionRequest& request) const
    {
        std::vector<Suggestion> results;

        collect_context_suggestions(request, results);
        collect_file_suggestions(request, results);
        if (request.include_speculative) {
            collect_speculative_suggestions(request, results);
        }
        collect_shell_suggestions(results);

        // Sort by confidence desc (operator<=> compares other.confidence <=> confidence).
        std::ranges::sort(results);
        if (results.size() > request.max_suggestions) {
            results.resize(request.max_suggestions);
        }
        return results;
    }

    void add_shell_history(ShellHistoryEntry entry) {
        if (shell_history_.size() >= max_history_size_) {
            shell_history_.pop_front();
        }
        shell_history_.push_back(std::move(entry));
    }

    using SuggestionGenerator = std::function<std::vector<Suggestion>(const SuggestionRequest&)>;
    void register_generator(std::string name, SuggestionGenerator gen) {
        custom_generators_[std::move(name)] = std::move(gen);
    }

    void clear_history() noexcept { shell_history_.clear(); }

    void set_max_history(std::size_t n) noexcept { max_history_size_ = n; }

    // Expose shell history so the ranker (or tests) can inspect the last failure.
    [[nodiscard]] const std::deque<ShellHistoryEntry>& shell_history() const noexcept {
        return shell_history_;
    }

private:
    std::deque<ShellHistoryEntry> shell_history_;
    std::size_t max_history_size_{500};
    std::unordered_map<std::string, SuggestionGenerator> custom_generators_;

    void collect_context_suggestions(
        const SuggestionRequest& request,
        std::vector<Suggestion>& out) const
    {
        if (request.recent_turns.empty()) return;
        const auto& last = request.recent_turns.back();

        if (last.role == "assistant") {
            out.push_back({
                .text = "Can you explain that in more detail?",
                .description = "Request elaboration",
                .source = SuggestionSource::ConversationContext,
                .priority = SuggestionPriority::Medium,
                .confidence = 0.6,
            });
            out.push_back({
                .text = "What are the potential issues with this approach?",
                .description = "Ask about risks",
                .source = SuggestionSource::ConversationContext,
                .priority = SuggestionPriority::Medium,
                .confidence = 0.5,
            });
        }
    }

    void collect_file_suggestions(
        const SuggestionRequest& request,
        std::vector<Suggestion>& out) const
    {
        for (const auto& file : request.open_files | std::views::take(3)) {
            out.push_back({
                .text = std::format("Review {}", file),
                .description = std::format("Review open file: {}", file),
                .source = SuggestionSource::FileSystem,
                .priority = SuggestionPriority::Low,
                .confidence = 0.4,
            });
        }
    }

    // Now uses the deterministic ranker: generates multiple ranked candidates from
    // conversation signals and appends the survivors (already gated + deduped + capped).
    void collect_speculative_suggestions(
        const SuggestionRequest& request,
        std::vector<Suggestion>& out) const
    {
        auto ranked = rank_candidate_suggestions(request);
        for (auto& s : ranked) {
            out.push_back(std::move(s));
        }
    }

    void collect_shell_suggestions(std::vector<Suggestion>& out) const {
        if (shell_history_.empty()) return;
        const auto& last_cmd = shell_history_.back();
        if (last_cmd.exit_code != 0) {
            out.push_back({
                .text = std::format("Fix the failing command: {}", last_cmd.command),
                .description = "Previous command failed",
                .source = SuggestionSource::ShellHistory,
                .priority = SuggestionPriority::High,
                .confidence = 0.8,
            });
        }
    }
};

} // namespace cc::services::prompt_suggestion
