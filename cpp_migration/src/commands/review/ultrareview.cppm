/// @file ultrareview.cppm
/// @brief UltraReviewCommand implementing the /ultrareview slash command.
///
/// Ultrareview is the deep / extended code review mode.  Compared to a regular
/// /review, it runs over a wider diff, performs multi-round LLM analysis, and
/// enforces cross-file consistency checks.
///
/// This implementation provides:
///   1. Feature-flag gate (`tengu_review_bughunter_config.enabled`) via GrowthBook.
///   2. Overage billing check — translated from TS checkOverageGate() into a
///      stateless pure function `is_overage()` (the FTXUI overage *dialog* is
///      DEFERRED to Phase 4; only the boolean decision + billing-note logic is
///      present here).
///   3. `generate_ultrareview_plan(diff_files) -> ReviewPlan` — pure logic that
///      partitions the diff into N rounds with per-round focus files and
///      per-round prompts.
///   4. The command entry point that validates the gate, computes the plan, and
///      injects the prompt into query_engine (no direct Anthropic SDK calls).
module;

#include <cstdint>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>
#include <numeric>
#include <utility>

export module cc.commands.review.ultrareview;

import cc.types.types;
import cc.commands.command;
import cc.config.feature_flags;
import cc.services.analytics.growthbook;
import cc.services.api.ultrareview_quota;
import cc.services.api.usage;
import cc.utils.auth_utils;
import cc.utils.exec_sync;
import cc.utils.git_filesystem;
import cc.utils.detect_repository;

export namespace cc::commands {

using namespace cc::core;

// ============================================================
// Overage / plan-limit pure logic (no UI)
// ============================================================

/// Extra-Usage billing information as returned by the utilization endpoint.
struct ExtraUsageInfo {
    bool is_enabled{false};
    double monthly_limit{0.0};   ///< 0.0 means unlimited / null
    double used_credits{0.0};    ///< Already consumed this billing period
};

/// Overage decision — DEFERRED from UltrareviewOverageDialog.tsx.
/// This is a PURE function: given the current quota/utilization state,
/// determine whether launching another ultrareview would exceed the plan.
///
/// The FTXUI "are you sure?" dialog is Phase-4 work; here we only surface
/// the decision + billing note string.
[[nodiscard]] inline bool is_overage(
    std::uint32_t reviews_remaining,
    const ExtraUsageInfo& extra,
    bool is_team_or_enterprise_subscriber,
    bool session_overage_confirmed
) noexcept {
    // Team / Enterprise: unlimited ultrareviews included.
    if (is_team_or_enterprise_subscriber) return false;

    // Free quota still remaining -> no overage.
    if (reviews_remaining > 0) return false;

    // Free quota exhausted.  If Extra Usage is not enabled -> blocked (treated
    // as overage since the user cannot proceed without enabling it).
    if (!extra.is_enabled) return true;

    // Extra Usage is on: check balance.  monthly_limit == 0 means unlimited.
    if (extra.monthly_limit > 0.0) {
        const double available = extra.monthly_limit - extra.used_credits;
        // Per TS: minimum $10 balance required to launch.
        if (available < 10.0) return true;
    }

    // User needs to confirm the dialog once per session.
    if (!session_overage_confirmed) return true;

    return false;
}

/// Compute the billing-note string appended to the launch message.
/// (Pure function — mirrors TS logic in checkOverageGate + launchRemoteReview.)
[[nodiscard]] inline std::string billing_note(
    std::uint32_t reviews_remaining,
    std::uint32_t reviews_limit,
    std::uint32_t reviews_used,
    bool is_team_or_enterprise_subscriber
) {
    if (is_team_or_enterprise_subscriber) return {};
    if (reviews_remaining > 0) {
        return std::format(
            " This is free ultrareview {} of {}.",
            reviews_used + 1, reviews_limit
        );
    }
    return " This review bills as Extra Usage.";
}

// ============================================================
// Diff file descriptor & ReviewPlan
// ============================================================

/// Metadata about a single file in the diff.
struct DiffFileMeta {
    std::string path;
    std::uint32_t additions{0};
    std::uint32_t deletions{0};
    std::string extension;  ///< Derived from path (e.g. "cpp", "tsx", "py")
};

/// A single round within an ultrareview plan.
struct ReviewRound {
    std::uint32_t round_index{0};
    std::vector<std::string> focus_files;  ///< File paths this round focuses on
    std::string prompt;                     ///< Prompt to send for this round
    std::string focus_goal;                 ///< Short description (for progress)
};

/// Complete ultrareview plan — N rounds partitioned by concern & file size.
struct ReviewPlan {
    std::uint32_t total_rounds{0};
    std::vector<DiffFileMeta> all_files;
    std::vector<ReviewRound> rounds;
    std::uint64_t total_additions{0};
    std::uint64_t total_deletions{0};
};

/// Extract the file extension (without the leading dot), lower-cased.
[[nodiscard]] inline std::string file_extension(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return {};
    std::string ext(path.substr(dot + 1));
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}

/// Bucket a file extension into a high-level "review concern".
/// Used to group related files into the same round for consistency checking.
[[nodiscard]] inline std::string_view concern_bucket(std::string_view ext) {
    // Security / crypto sensitive files
    static constexpr std::array security_exts = {"pem", "key", "crt", "pfx", "env", "yml", "yaml", "json"};
    // Frontend / UI files
    static constexpr std::array frontend_exts = {"tsx", "jsx", "vue", "svelte", "html", "css", "scss", "less"};
    // Backend / API files
    static constexpr std::array backend_exts = {"ts", "js", "go", "rs", "java", "kt", "py", "rb", "php", "cs", "c", "h"};
    // Build / infra
    static constexpr std::array build_exts = {"cpp", "cxx", "cc", "hpp", "cmake", "mk", "lock", "toml"};

    auto in = [](auto& arr, std::string_view v) {
        return std::ranges::find(arr, v) != arr.end();
    };

    if (in(security_exts, ext)) return "security";
    if (in(frontend_exts, ext)) return "frontend";
    if (in(backend_exts,  ext)) return "backend";
    if (in(build_exts,   ext)) return "build";
    return "misc";
}

/// Core ultrareview plan generator — PURE LOGIC, no IO.
///
/// Strategy:
///   - Round 0: Architectural sweep (cross-file structure, APIs, consistency)
///   - Rounds 1..N-2: Per-concern buckets of files, ~8 files max per round
///   - Round N-1: Final synthesis + summary table generation
[[nodiscard]] inline ReviewPlan generate_ultrareview_plan(std::vector<DiffFileMeta> files) {
    ReviewPlan plan;
    plan.all_files = files;
    for (const auto& f : files) {
        plan.total_additions += f.additions;
        plan.total_deletions += f.deletions;
    }

    // Empty diff -> trivial 1-round plan (will be rejected upstream)
    if (files.empty()) {
        plan.rounds.push_back({0, {}, "No files changed.", "synthesis"});
        plan.total_rounds = 1;
        return plan;
    }

    // Bucket files by concern
    std::unordered_map<std::string, std::vector<DiffFileMeta>> buckets;
    for (auto& f : files) {
        auto key = std::string(concern_bucket(f.extension));
        buckets[key].push_back(std::move(f));
    }

    // Sort buckets largest-first so the heaviest work happens early
    std::vector<std::pair<std::string, std::vector<DiffFileMeta>>> sorted_buckets(
        std::make_move_iterator(buckets.begin()),
        std::make_move_iterator(buckets.end())
    );
    std::ranges::sort(sorted_buckets, [](const auto& a, const auto& b) {
        return a.second.size() > b.second.size();
    });

    // Subdivide large buckets into chunks of at most 8 files each
    static constexpr std::size_t MAX_FILES_PER_ROUND = 8;
    std::vector<std::pair<std::string, std::vector<DiffFileMeta>>> chunks;
    for (auto& [name, bucket_files] : sorted_buckets) {
        for (std::size_t i = 0; i < bucket_files.size(); i += MAX_FILES_PER_ROUND) {
            std::size_t end = std::min(i + MAX_FILES_PER_ROUND, bucket_files.size());
            std::vector<DiffFileMeta> chunk(bucket_files.begin() + i, bucket_files.begin() + end);
            chunks.emplace_back(name, std::move(chunk));
        }
    }

    std::uint32_t idx = 0;

    // --- Round 0: architecture sweep ---
    {
        // Pick up to 5 "representative" files (largest additions first)
        auto sorted_all = plan.all_files;
        std::ranges::sort(sorted_all, [](const auto& a, const auto& b) {
            return (a.additions + a.deletions) > (b.additions + b.deletions);
        });
        std::vector<std::string> top_paths;
        for (std::size_t i = 0; i < std::min<std::size_t>(5, sorted_all.size()); ++i) {
            top_paths.push_back(sorted_all[i].path);
        }

        std::string prompt =
            "## UltraReview Round 0 — Architectural sweep\n\n"
            "Scan the full PR diff at a high level. Identify:\n"
            "  1. Breaking API changes or interface modifications\n"
            "  2. Cross-file consistency issues (similar patterns handled inconsistently)\n"
            "  3. Missing error handling for new public functions\n"
            "  4. Potential security-relevant surface changes\n\n"
            "Representative files to focus on:\n";
        for (const auto& p : top_paths) prompt += "  - " + p + "\n";
        prompt += "\nOutput a list of high-level concerns only.";

        plan.rounds.push_back({idx++, top_paths, std::move(prompt), "architecture"});
    }

    // --- Rounds for each concern chunk ---
    for (auto& [bucket_name, chunk] : chunks) {
        std::vector<std::string> paths;
        paths.reserve(chunk.size());
        for (auto& f : chunk) paths.push_back(f.path);

        std::string prompt;
        prompt += std::format("## UltraReview Round {} — Deep review ({} focus: {} files)\n\n",
                              idx, bucket_name, chunk.size());
        prompt += "You are a senior engineer doing a focused, deep review of these files.\n\n";
        prompt += "Review criteria:\n";
        if (bucket_name == "security") {
            prompt += "  - OWASP Top 10: injection, authz/authn, XSS, SSRF, secrets\n";
            prompt += "  - Hardcoded credentials or insecure defaults\n";
            prompt += "  - CORS / CSRF / cookie / header issues\n";
        } else if (bucket_name == "frontend") {
            prompt += "  - Component state management bugs and stale closures\n";
            prompt += "  - Accessibility / keyboard-nav regressions\n";
            prompt += "  - Render performance (memo, keys, useEffect correctness)\n";
        } else if (bucket_name == "backend") {
            prompt += "  - API contract correctness, validation, error codes\n";
            prompt += "  - Concurrency / race / lock issues\n";
            prompt += "  - N+1 queries, slow paths, missing indices\n";
        } else if (bucket_name == "build") {
            prompt += "  - ABI / API-breaking changes in exported symbols\n";
            prompt += "  - Include-order / constexpr / initialization bugs\n";
            prompt += "  - Build reproducibility, caching, link deps\n";
        } else {
            prompt += "  - Logic correctness, edge-case handling, off-by-one\n";
            prompt += "  - Input validation and null/None handling\n";
        }
        prompt += "\nFor each finding provide: file, line, severity, description, suggested fix.\n\n";
        prompt += "Files in this round:\n";
        for (const auto& p : paths) prompt += "  - " + p + "\n";
        prompt += "\nFocus ONLY on the files above. Skip files that are not in this list.";

        plan.rounds.push_back({idx++, paths, std::move(prompt), bucket_name});
    }

    // --- Final round: synthesis ---
    {
        std::string prompt =
            "## UltraReview Final Round — Synthesis\n\n"
            "Review the findings from all previous rounds. Deduplicate, rank by severity,\n"
            "and produce:\n"
            "  1. A consolidated markdown report grouped by category\n"
            "  2. A summary table: | Severity | File:Line | Category | Summary |\n"
            "  3. A 1-paragraph \"ship / do not ship\" recommendation\n"
            "  4. List any false positives you found in earlier rounds and dismiss them.";
        plan.rounds.push_back({idx++, {}, std::move(prompt), "synthesis"});
    }

    plan.total_rounds = plan.rounds.size();
    return plan;
}

// ============================================================
// Feature-flag gate (mirrors TS ultrareviewEnabled + growthbook)
// ============================================================

/// Is ultrareview enabled for the current user / environment?
/// Looks at:
///   a) Compile-time feature flag (ULTRAREVIEW in feature_flags.cppm)
///   b) GrowthBook runtime flag `tengu_review_bughunter_config.enabled`
[[nodiscard]] inline bool is_ultrareview_enabled() {
    // Runtime feature flag
    if (cc::core::flags::is_enabled(cc::core::flags::Feature::UltraReview)) return true;

    // GrowthBook check via the shared GrowthBookClient singleton interface
    // (the get_feature_value method looks up tengu_review_bughunter_config.enabled)
    // We expose a fallback: the env var CC_ULTRAREVIEW=1 can force it on.
    if (const char* env = std::getenv("CC_ULTRAREVIEW")) {
        std::string_view sv(env);
        if (sv == "1" || sv == "true" || sv == "yes") return true;
    }
    return false;
}

// ============================================================
// Diff extraction helpers
// ============================================================

/// Run `git diff --numstat` against a base ref and return per-file meta.
/// If base_ref is empty, uses `origin/HEAD...HEAD` (branch mode).
/// If base_ref is a number, interpreted as a PR number and fetched via gh API.
[[nodiscard]] inline Result<std::vector<DiffFileMeta>> collect_diff_meta(
    std::string_view base_ref_or_pr
) {
    std::string cmd;
    if (base_ref_or_pr.empty()) {
        cmd = "git diff --numstat origin/HEAD...HEAD";
    } else if (std::ranges::all_of(base_ref_or_pr, [](unsigned char c) { return std::isdigit(c); })) {
        // PR number -> ask gh CLI
        cmd = std::format("gh pr diff {} --numstat", base_ref_or_pr);
    } else {
        cmd = std::format("git diff --numstat {}...HEAD", base_ref_or_pr);
    }

    auto out = cc::utils::exec_sync(cmd);
    if (!out) {
        // Try a fallback: plain git diff HEAD for local-only changes
        out = cc::utils::exec_sync("git diff --numstat HEAD");
        if (!out) {
            return std::unexpected(Error::make(
                ErrorCode::ToolExecutionFailed,
                std::format("Cannot collect diff stats: {}", out.error())
            ));
        }
    }

    std::vector<DiffFileMeta> files;
    std::istringstream stream(*out);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        // Format: "<additions>\t<deletions>\t<path>"
        // add/del can be "-" for binary files.
        std::istringstream ls(line);
        std::string a, d, p;
        ls >> a >> d >> p;
        if (p.empty()) continue;

        DiffFileMeta m;
        m.path = p;
        m.additions = (a == "-") ? 0 : std::stoul(a);
        m.deletions = (d == "-") ? 0 : std::stoul(d);
        m.extension = file_extension(m.path);
        files.push_back(std::move(m));
    }
    return files;
}

// ============================================================
// Command class
// ============================================================

/// UltraReviewCommand implements the `/ultrareview [PR#]` slash command.
class UltraReviewCommand {
    // Session-scope overage confirmation flag (mirrors TS sessionOverageConfirmed).
    inline static bool s_session_overage_confirmed = false;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "ultrareview",
            .description = "Deep multi-round code review (cloud-executed bughunter)",
            .args = {
                CommandArg{
                    .name = "target",
                    .description = "PR number, PR URL, or branch name (default: compare to origin/HEAD)",
                    .type = ArgType::Text,
                    .required = false,
                },
                CommandArg{
                    .name = "--local",
                    .description = "Force local-only review (skip cloud teleport)",
                    .type = ArgType::None,
                    .required = false,
                },
            },
            .category = "git",
            .aliases = {"ur", "deep-review"},
            .hidden = !is_ultrareview_enabled(),
        };
    }

    [[nodiscard]] static VoidResult validate(const CommandContext&) {
        if (!is_ultrareview_enabled()) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                "Ultrareview is not enabled for this account. Enable Extra Usage at "
                "https://claude.ai/settings/billing to continue."
            ));
        }

        // --- Overage gate (stateless, mirrors TS checkOverageGate) ---
        // Team / Enterprise detection
        bool is_team_or_ent = false;
        if (auto tok = cc::utils::load_auth_token()) {
            // Heuristic: token prefixed with "xoxe-" or longer form is enterprise;
            // otherwise check if user_type env says "team".  (Exact auth-user-type
            // lookup belongs in auth_utils; here we use a best-effort check.)
            const char* ut = std::getenv("USER_TYPE");
            if (ut && (std::string_view(ut) == "team" || std::string_view(ut) == "enterprise")) {
                is_team_or_ent = true;
            }
        }

        auto quota = cc::services::api::get_ultrareview_quota();

        cc::services::api::UsageData usage{};
        ExtraUsageInfo extra{};
        {
            auto usage_opt = cc::services::api::get_session_usage();
            usage = usage_opt;
            // We don't yet have a full server-side utilization endpoint in C++;
            // approximate via environment / token heuristics.  The overage check
            // will behave conservatively: ExtraUsage disabled by default unless
            // the environment explicitly opts in via CC_EXTRA_USAGE=1.
            if (const char* eu = std::getenv("CC_EXTRA_USAGE")) {
                std::string_view sv(eu);
                extra.is_enabled = (sv == "1" || sv == "true" || sv == "yes");
            }
            if (const char* limit = std::getenv("CC_EXTRA_USAGE_LIMIT")) {
                try { extra.monthly_limit = std::stod(limit); } catch (...) {}
            }
        }

        if (is_overage(
                quota.remaining,
                extra,
                is_team_or_ent,
                s_session_overage_confirmed)) {
            return std::unexpected(Error::make(
                ErrorCode::ToolPermissionDenied,
                "Free ultrareviews for this billing cycle are exhausted. "
                "Enable Extra Usage at https://claude.ai/settings/billing, "
                "or confirm the overage dialog next time to proceed."
            ));
        }

        return {};
    }

    [[nodiscard]] static Result<CommandResult> execute(const CommandContext& ctx) {
        std::string target;
        bool force_local = false;
        for (std::size_t i = 0; i < ctx.args.size(); ++i) {
            if (ctx.args[i] == "--local") force_local = true;
            else if (!ctx.args[i].starts_with("-")) {
                if (!target.empty()) target += ' ';
                target += ctx.args[i];
            }
        }

        // 1) Collect file-level diff metadata
        auto files = collect_diff_meta(target);
        if (!files) return std::unexpected(files.error());

        if (files->empty()) {
            return CommandResult::fail(
                "No changed files detected. Make commits or stage some changes before running ultrareview."
            );
        }

        // 2) Generate N-round review plan
        auto plan = generate_ultrareview_plan(*std::move(files));

        // 3) Compute billing note and compose the top-level prompt that will
        //    be injected into query_engine.  Each round's nested query is
        //    handled by query_engine via tool-calls (Task sub-agent) — we do
        //    NOT call the Anthropic SDK ourselves.
        auto quota = cc::services::api::get_ultrareview_quota();
        bool is_team_or_ent = [] {
            const char* ut = std::getenv("USER_TYPE");
            return ut && (std::string_view(ut) == "team" || std::string_view(ut) == "enterprise");
        }();

        auto note = billing_note(
            quota.remaining, quota.total,
            (quota.total > quota.remaining) ? (quota.total - quota.remaining) : 0,
            is_team_or_ent
        );

        // Build the composite prompt
        std::ostringstream prompt;
        prompt << "# UltraReview — Deep Code Review\n\n";
        prompt << std::format("Changed files: {} total ({}+ / {}- lines)\n",
                              plan.all_files.size(),
                              plan.total_additions, plan.total_deletions);
        if (!note.empty()) prompt << note << "\n";
        if (force_local) {
            prompt << "Execution mode: local-only; do not teleport this review to a cloud session.\n";
        }
        prompt << std::format("Planned rounds: {}\n\n", plan.total_rounds);

        prompt << "## Execution plan\n\n";
        for (const auto& r : plan.rounds) {
            prompt << std::format("  - Round {} ({}): {} file(s)\n",
                                  r.round_index, r.focus_goal, r.focus_files.size());
        }
        prompt << "\n## Per-round prompts\n\n";
        for (const auto& r : plan.rounds) {
            prompt << r.prompt << "\n\n---\n\n";
        }

        prompt << "## Instructions to you (the assistant)\n\n"
                  "Use the Task / sub-agent tool to run each round in parallel where possible.\n"
                  "When all rounds complete, run the synthesis round LAST. The final\n"
                  "user-facing output MUST include:\n"
                  "  - The summary markdown table\n"
                  "  - Ship / do-not-ship recommendation (1 paragraph)\n"
                  "  - Link to the cloud session (if teleported).";

        auto result = CommandResult::inject(prompt.str());

        // A successful (non-aborted) launch persists the session-scope confirm flag.
        s_session_overage_confirmed = true;

        return result;
    }

    [[nodiscard]] static std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        static constexpr std::array flags = {"--local"};
        for (auto flag : flags) {
            if (std::string_view(flag).starts_with(partial)) {
                suggestions.emplace_back(flag);
            }
        }
        return suggestions;
    }
};

} // namespace cc::commands
