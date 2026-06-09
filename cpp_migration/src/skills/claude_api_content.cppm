/// @file claude_api_content.cppm
/// @brief Claude API content skill — auto-detects project language, injects
///        a curated Reading Guide, model metadata, and cross-references to
///        shared / language-specific reference docs for Anthropic SDK usage.
///
/// This is the C++20 port of `src/skills/bundled/claudeApiContent.ts` plus the
/// runtime logic from `src/skills/bundled/claudeApi.ts`. The TS version
/// lazy-loads 247 KB of inlined `.md` strings; here we embed the structural
/// parts (Reading Guide, model constants, doc-route table, common pitfalls)
/// as raw string literals and rely on cc.tools.web_fetch + cc.tools.file_read
/// for any docs that must be resolved at runtime.
module;

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.skills.claude_api_content;

import cc.skills.skill;

export namespace cc::skills::claude_api_content {

// ============================================================
// Model identifiers — these substitute into {{OPUS_ID}} style placeholders
// inside prompt fragments. Keep in sync with SKILL_MODEL_VARS in the TS
// source; the C++ equivalent is a plain constexpr map.
// ============================================================
struct ModelVar {
    std::string_view key;
    std::string_view value;
};

constexpr std::array<ModelVar, 7> SKILL_MODEL_VARS{{
    {"OPUS_ID",         "claude-opus-4-6"},
    {"OPUS_NAME",       "Claude Opus 4.6"},
    {"SONNET_ID",       "claude-sonnet-4-6"},
    {"SONNET_NAME",     "Claude Sonnet 4.6"},
    {"HAIKU_ID",        "claude-haiku-4-5"},
    {"HAIKU_NAME",      "Claude Haiku 4.5"},
    {"PREV_SONNET_ID",  "claude-sonnet-4-5"},
}};

/// Replace every `{{KEY}}` occurrence in `text` with the matching value from
/// SKILL_MODEL_VARS. Unknown keys are left untouched (matches TS behavior).
[[nodiscard]] inline std::string substitute_model_vars(std::string text) {
    static const std::regex placeholder(R"(\{\{(\w+)\}\})");
    std::string out;
    out.reserve(text.size());
    auto begin = std::sregex_iterator(text.begin(), text.end(), placeholder);
    auto end = std::sregex_iterator();
    std::size_t last = 0;
    for (auto it = begin; it != end; ++it) {
        const auto& m = *it;
        out.append(text, last, static_cast<std::size_t>(m.position()) - last);
        std::string key = m[1].str();
        bool found = false;
        for (const auto& [k, v] : SKILL_MODEL_VARS) {
            if (k == key) {
                out.append(v.data(), v.size());
                found = true;
                break;
            }
        }
        if (!found) {
            out.append(m[0].first, m[0].second);
        }
        last = static_cast<std::size_t>(m.position()) +
               static_cast<std::size_t>(m.length());
    }
    out.append(text, last, text.size() - last);
    return out;
}

/// Strip HTML `<!-- ... -->` comments, looping to correctly handle the
/// (rare) nested case. Port of TS `processContent` comment-removal loop.
[[nodiscard]] inline std::string strip_html_comments(std::string text) {
    static const std::regex html_comment(R"(<!--[\s\S]*?-->\n?)");
    std::string prev;
    do {
        prev = text;
        text = std::regex_replace(text, html_comment, "");
    } while (text != prev);
    return text;
}

/// Apply the full TS `processContent` pipeline: strip comments, then
/// substitute `{{MODEL_VAR}}` placeholders.
[[nodiscard]] inline std::string process_content(std::string md) {
    return substitute_model_vars(strip_html_comments(std::move(md)));
}

// ============================================================
// Language detection — mirror of TS `detectLanguage()` +
// `LANGUAGE_INDICATORS`. Scans `cwd` for file / directory names that
// strongly imply a primary project language.
// ============================================================
enum class DetectedLanguage {
    Python,
    TypeScript,
    Java,
    Go,
    Ruby,
    CSharp,
    Php,
    Curl,
    Unknown
};

struct LangIndicator {
    DetectedLanguage lang;
    std::vector<std::string_view> markers; // filename or suffix (".xxx")
};

constexpr std::array<LangIndicator, 8> LANGUAGE_INDICATORS{{
    {DetectedLanguage::Python,     {".py", "requirements.txt", "pyproject.toml", "setup.py", "Pipfile"}},
    {DetectedLanguage::TypeScript, {".ts", ".tsx", "tsconfig.json", "package.json"}},
    {DetectedLanguage::Java,       {".java", "pom.xml", "build.gradle"}},
    {DetectedLanguage::Go,         {".go", "go.mod"}},
    {DetectedLanguage::Ruby,       {".rb", "Gemfile"}},
    {DetectedLanguage::CSharp,     {".cs", ".csproj"}},
    {DetectedLanguage::Php,        {".php", "composer.json"}},
    {DetectedLanguage::Curl,       {}},
}};

[[nodiscard]] inline std::string_view to_string(DetectedLanguage l) {
    switch (l) {
        case DetectedLanguage::Python:     return "python";
        case DetectedLanguage::TypeScript: return "typescript";
        case DetectedLanguage::Java:       return "java";
        case DetectedLanguage::Go:         return "go";
        case DetectedLanguage::Ruby:       return "ruby";
        case DetectedLanguage::CSharp:     return "csharp";
        case DetectedLanguage::Php:        return "php";
        case DetectedLanguage::Curl:       return "curl";
        case DetectedLanguage::Unknown:    return "unknown";
    }
    return "unknown";
}

/// Scan entries of `dir` against LANGUAGE_INDICATORS. Returns the first
/// matched language or Unknown. Mirrors `detectLanguage()` semantics:
///   * marker starting with "." -> suffix match on any entry
///   * otherwise               -> exact filename match
[[nodiscard]] inline DetectedLanguage detect_language(
    const std::filesystem::path& dir) noexcept {
    std::vector<std::filesystem::path> entries;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        entries.push_back(e.path().filename());
    }
    for (const auto& [lang, markers] : LANGUAGE_INDICATORS) {
        if (markers.empty()) continue;
        for (const auto& marker : markers) {
            const bool is_suffix =
                !marker.empty() && marker.front() == '.';
            const auto match = [&](const std::filesystem::path& f) {
                if (is_suffix) {
                    auto ext = f.extension().string();
                    std::ranges::transform(ext, ext.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    return ext == marker;
                }
                return f.filename().string() == marker;
            };
            if (std::ranges::any_of(entries, match)) return lang;
        }
    }
    return DetectedLanguage::Unknown;
}

// ============================================================
// Reading guide — port of INLINE_READING_GUIDE in TS. Accepts a language
// label ("python", "typescript", …) and returns the routing table with
// `{lang}` placeholders resolved.
// ============================================================
[[nodiscard]] inline std::string build_reading_guide(std::string_view lang) {
    // NOTE: the TS source references docs that live in `SKILL_FILES`
    // (247 KB inlined). At runtime this C++ port builds the same routing
    // table and delegates resolution to WebFetchTool against the URLs in
    // `shared/live-sources.md` when a concrete doc body is needed.
    const auto tmpl = std::string_view(R"raw(## Reference Documentation (Reading Guide)

The relevant documentation for your detected language ("{lang}") is cross-referenced below. Each entry names the source path that answers a specific task. Fetch concrete doc bodies via the WebFetch tool using URLs from `shared/live-sources.md`, or read them from disk if a local `claude-api/` checkout exists.

### Quick Task Reference

**Single text classification / summarization / extraction / Q&A:**
  → `{lang}/claude-api/README.md` — basic Messages API, system prompts, max_tokens, temperature.

**Chat UI or real-time response display (streaming tokens):**
  → `{lang}/claude-api/README.md` + `{lang}/claude-api/streaming.md` — server-sent events, delta accumulation.

**Long-running conversations (may exceed context window):**
  → `{lang}/claude-api/README.md` — see the **Compaction** section for message pruning + summarization strategy.

**Prompt caching / "why is my cache hit rate low" / caching optimization:**
  → `shared/prompt-caching.md` (cache-control ephemeral headers, breakpoint strategy)
  → `{lang}/claude-api/README.md` — language-specific cache-control helpers.

**Function calling / tool use / agent loops:**
  → `{lang}/claude-api/README.md` — defining tools in request
  → `shared/tool-use-concepts.md` — role alternation, tool_use/tool_result pairing, parallel tool calls
  → `{lang}/claude-api/tool-use.md` — language-specific examples.

**Batch processing (non-latency-sensitive, async Message Batches API):**
  → `{lang}/claude-api/README.md` + `{lang}/claude-api/batches.md`.

**File uploads reused across multiple API requests:**
  → `{lang}/claude-api/README.md` + `{lang}/claude-api/files-api.md`.

**Agent with built-in tools (file / web / terminal) — Python & TypeScript only:**
  → `{lang}/agent-sdk/README.md` + `{lang}/agent-sdk/patterns.md`.

**Error handling / 4xx / 5xx semantics / retry semantics:**
  → `shared/error-codes.md`.

**Latest docs at runtime (when offline / bundled docs feel stale):**
  → Fetch via `WebFetch` using URLs enumerated in `shared/live-sources.md`.

### Canonical model identifiers (for substitution)

| Human name       | API model id           |
|------------------|------------------------|
| {{OPUS_NAME}}    | {{OPUS_ID}}            |
| {{SONNET_NAME}}  | {{SONNET_ID}}          |
| {{HAIKU_NAME}}   | {{HAIKU_ID}}           |
)raw");
    return substitute_model_vars(std::regex_replace(
        std::string(tmpl), std::regex(R"(\{lang\})"), std::string(lang)));
}

// ============================================================
// When-to-Use-WebFetch + Common Pitfalls sections. The TS original slices
// these out of SKILL.md by heading anchor. We embed them verbatim so the
// prompt sections stay reachable even when the source .md snapshot is
// stripped.
// ============================================================
constexpr std::string_view WHEN_TO_USE_WEB_FETCH = R"raw(
## When to Use WebFetch

Fetch the live Anthropic docs **when**:
  1. The user asks about a *recently shipped* API feature (betas, new models,
     new SDK methods) that may postdate this skill's bundled snapshot.
  2. The user reports an error string you cannot map with confidence from
     `shared/error-codes.md`.
  3. The user shares a curl / Python snippet against a non-canonical endpoint
     and you need to verify route shape.

Preferred source URLs (from `shared/live-sources.md`):
  * API reference:    https://docs.anthropic.com/en/api/messages
  * SDK docs root:    https://docs.anthropic.com/en/docs/intro-to-claude
  * Prompt caching:   https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching
  * Tool use guide:   https://docs.anthropic.com/en/docs/use-cases/tool-use

**Always** prefer bundled `shared/*.md` and `{lang}/**/*.md` content *first*;
fall back to WebFetch *only* when bundled content is insufficient or stale.

## Common Pitfalls

1. **Forgetting the `anthropic-version` header.** Raw HTTP requests always
   need `anthropic-version: 2023-06-01` (or later); the SDK sets this for you.
2. **Role alternation bug.** Messages must strictly alternate `user` and
   `assistant`; two consecutive same-role messages return 400. Concatenate
   same-role turns into a single content array instead.
3. **Tool result id mismatch.** Each `tool_result` content block must carry
   the exact `tool_use_id` that the assistant emitted on the prior turn.
   Mismatches silently break tool-use state.
4. **`max_tokens` too low.** Outputs are clipped without warning. Start at
   4096 and tune *down* once latency is measured.
5. **Using a model id with a date suffix.** Alias model ids
   (e.g. `{{SONNET_ID}}`) are *not* dates. Never append `-2024xxyy`.
   (Counter-example: `{{PREV_SONNET_ID}}` was the prior Sonnet id — this is
   the *only* style of date-looking id you will ever see.)
6. **Streaming JSON accumulation.** SDK streaming yields per-event deltas;
   accumulate `content_block_delta.text` into a single buffer, **do not**
   join with whitespace.
7. **Prompt caching scope errors.** Only `system`, `messages[i].content`,
   and `tools` arrays accept `cache_control: {"type": "ephemeral"}`. Putting
   the header anywhere else is a no-op, not an error.
)raw";

/// Apply {{MODEL_VAR}} substitution on the pitfalls block.
[[nodiscard]] inline std::string when_to_use_web_fetch_section() {
    return substitute_model_vars(std::string(WHEN_TO_USE_WEB_FETCH));
}

// ============================================================
// Base prompt (SKILL.md header). Keep this short — the Reading Guide above
// carries the bulk of the instruction. Matches TS `basePrompt` (everything
// before `## Reading Guide` in SKILL.md).
// ============================================================
constexpr std::string_view BASE_PROMPT = R"raw(# claude-api — Build apps with the Claude API / Anthropic SDKs

You are an expert assistant for **Anthropic's Claude platform**. Help the user
build, debug, and optimize applications that call the Claude Messages API
directly or via the official SDKs.

## Mandatory Response Style

1. **Quote the exact route + HTTP verb** whenever you describe an API call
   (e.g. `POST https://api.anthropic.com/v1/messages`).
2. **Always include a fully runnable code sample** in the user's project
   language (auto-detected; see Reading Guide below). If language is unknown,
   show both `curl` and `python` snippets.
3. **Every tool-use example must show:**
   - the `tools` array definition
   - a sample assistant `tool_use` block
   - the matching user `tool_result` block
   - the next-turn request containing the accumulated messages array.
4. **Always show error handling.** Wrap SDK calls in `try/catch`; show
   status-code branching for raw HTTP.
5. **Default to `model: "{{SONNET_ID}}"`** unless the task explicitly calls
   for reasoning-heavy work (use `{{OPUS_ID}}`) or extremely low-latency
   volume processing (use `{{HAIKU_ID}}`).
)raw";

[[nodiscard]] inline std::string base_prompt_section() {
    return substitute_model_vars(std::string(BASE_PROMPT));
}

// ============================================================
// build_prompt() — top-level prompt assembly. Mirrors TS `buildPrompt()`:
//   base prompt -> reading guide per language -> fallback language hint ->
//   when-to-use-webfetch + pitfalls -> user request args.
// ============================================================
[[nodiscard]] inline std::string build_prompt(
    DetectedLanguage lang,
    std::string_view args) {

    std::vector<std::string> parts;
    parts.push_back(base_prompt_section());

    const auto lang_label = to_string(lang);
    if (lang != DetectedLanguage::Unknown) {
        parts.push_back(build_reading_guide(lang_label));
    } else {
        parts.push_back(build_reading_guide("unknown"));
        parts.emplace_back(
            "No project language was auto-detected from the working directory. "
            "Ask the user which language / SDK they are targeting, then refer "
            "to the matching docs in the Reading Guide above. Until they "
            "reply, default to `curl` + `python` examples.");
    }

    parts.push_back(when_to_use_web_fetch_section());

    if (!args.empty()) {
        parts.push_back(std::format("## User Request\n\n{}", args));
    }

    // Join with double newlines (matches TS `parts.join('\n\n')`)
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "\n\n";
        out += parts[i];
    }
    return out;
}

// ============================================================
// SkillDefinition factory — registered via BundledSkills in bundled.cppm.
//
// Trigger patterns follow the TS registration description:
//   TRIGGER when: code imports `anthropic` / `@anthropic-ai/sdk` /
//   `claude_agent_sdk`, or user asks to use Claude API, Anthropic SDKs,
//   or Agent SDK.
//   DO NOT TRIGGER when: code imports `openai` / other AI SDK, general
//   programming, or ML/data-science tasks unrelated to Claude.
// ============================================================
[[nodiscard]] inline SkillDefinition make_claude_api_content_skill() {
    return SkillDefinition{
        .name = "claude-api-content",
        .description =
            "Build apps with the Claude API or Anthropic SDK.\n"
            "TRIGGER when: code imports `anthropic` / `@anthropic-ai/sdk` / "
            "`claude_agent_sdk`, or user asks about Claude API, Messages "
            "endpoint, tool_use, prompt caching, streaming, batches, or "
            "files-api.\n"
            "DO NOT TRIGGER when: code imports `openai` / other AI SDK, "
            "general programming, or ML/data-science tasks.",
        .trigger_patterns = {
            // Explicit skill invocation or product-name mentions
            R"(/claude-api\b)",
            R"(\bclaude\s*api\b)",
            R"(\banthropic\s*api\b)",
            R"(\banthropic\s+sdk\b)",
            R"(\bclaude\s+sdk\b)",

            // API surface keywords
            R"(\bmessages\s+endpoint\b)",
            R"(v1/messages\b)",
            R"(\btool_use\b)",
            R"(\btool_result\b)",
            R"(prompt\s+cach(?:ing|e))",
            R"(cache_control\b)",
            R"(stream(?:ing)?\s*(?:tokens|response|sse))",
            R"(\bmessage\s+batch(?:es)?\b)",
            R"(\bfiles\s+api\b)",
            R"(\bclaude\s*agent\s*sdk\b)",
            R"(\bagent\s+sdk\b.*(?:python|typescript|ts))",

            // Import patterns (codebase context is matched elsewhere; these
            // catch user questions that *contain* the import literal)
            R"(import\s+.*\banthropic\b)",
            R"(from\s+['"]?anthropic['"]?)",
            R"(require\s*\(\s*['"]@anthropic-ai)",
            R"(ClaudeAgent\b)",
        },
        .content =
            // Default content (no args, cwd unknown). build_prompt() is
            // exposed separately so callers that *do* know the language or
            // user args can assemble a tailored prompt at runtime.
            build_prompt(DetectedLanguage::Unknown, ""),
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

} // namespace cc::skills::claude_api_content
