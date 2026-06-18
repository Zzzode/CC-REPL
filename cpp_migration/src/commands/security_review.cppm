/// @file security_review.cppm
/// @brief SecurityReviewCommand implementing the /security-review slash command.
///
/// Performs a security-focused review of pending branch changes.  The checklist
/// mirrors the TS implementation and covers the OWASP Top 10 categories
/// (injection, auth, crypto, XSS, SSRF, etc.) plus hardcoded-secret scanning.
///
/// Implementation notes:
///   * Hardcoded-secret detection REUSES `cc.services.team_memory.secret_scanner`
///     — the regex list is NOT duplicated here.
///   * The LLM call itself is NOT made here.  The command builds a structured
///     prompt and injects it via `CommandResult::inject`, which flows through
///     the public `query_engine` entry point in the main loop.
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>
#include <sstream>
#include <utility>

export module cc.commands.security_review;

import cc.types.types;
import cc.commands.command;
import cc.utils.exec_sync;
import cc.services.team_memory.secret_scanner;

export namespace cc::commands {

using namespace cc::core;

// ============================================================
// Finding types
// ============================================================

enum class SecuritySeverity : std::uint8_t {
    Info,
    Low,
    Medium,
    High,
    Critical,
};

/// A single security finding emitted by the scanner or LLM.
struct SecurityFinding {
    std::string file;
    std::uint32_t line{0};
    std::string type;            ///< e.g. "sql_injection", "hardcoded_secret", "xss"
    SecuritySeverity severity{SecuritySeverity::Info};
    std::string evidence;        ///< Snippet or textual evidence
    std::string recommendation;  ///< How to fix it
};

/// Convert severity to display label
[[nodiscard]] constexpr std::string_view severity_label(SecuritySeverity s) noexcept {
    switch (s) {
        case SecuritySeverity::Critical: return "CRITICAL";
        case SecuritySeverity::High:     return "HIGH";
        case SecuritySeverity::Medium:   return "MEDIUM";
        case SecuritySeverity::Low:      return "LOW";
        case SecuritySeverity::Info:     return "INFO";
    }
    return "UNKNOWN";
}

// ============================================================
// OWASP checklist — driven by the prompt, not by static regex
// ============================================================

/// Static list of OWASP Top 10 (2021) categories the prompt will tell the
/// LLM to check.  Exposed as a constant so other modules can reference it.
inline constexpr std::array<std::string_view, 10> OWASP_TOP_10_CATEGORIES = {
    "A01_Broken_Access_Control",
    "A02_Cryptographic_Failures",
    "A03_Injection",
    "A04_Insecure_Design",
    "A05_Security_Misconfiguration",
    "A06_Vulnerable_and_Outdated_Components",
    "A07_Identification_and_Authentication_Failures",
    "A08_Software_and_Data_Integrity_Failures",
    "A09_Security_Logging_and_Monitoring_Failures",
    "A10_Server_Side_Request_Forgery",
};

/// Sensitive-operation patterns the LLM is told to look for *in addition* to
/// the standard OWASP list.  These complement (not replace) the regex-based
/// secret scanner.
inline constexpr std::array<std::string_view, 8> SENSITIVE_OP_PATTERNS = {
    "eval() or dynamic code execution",
    "command-line invocation with user input",
    "SQL query string concatenation",
    "deserialization of untrusted data",
    "CORS wildcard (*) origins",
    "JWT tokens without signature verification",
    "file path concatenation without traversal checks",
    "HTTP requests to user-supplied URLs (SSRF risk)",
};

// ============================================================
// Diff collection
// ============================================================

/// Collect the full branch diff (vs origin/HEAD or specified base).
/// Returns the diff content plus a short status line.
[[nodiscard]] inline Result<std::string> collect_branch_diff(std::string_view base_ref = {}) {
    const std::string ref = base_ref.empty() ? "origin/HEAD" : std::string(base_ref);

    // 1) status
    auto status = cc::utils::exec_sync("git status -sb");
    // 2) files
    auto files = cc::utils::exec_sync(std::format("git diff --name-only {}...HEAD", ref));
    // 3) log
    auto log = cc::utils::exec_sync(std::format("git log --no-decorate {}...HEAD", ref));
    // 4) diff
    auto diff = cc::utils::exec_sync(std::format("git diff {}...HEAD", ref));

    if (!diff) {
        // Fall back to local-only (no origin)
        auto fall = cc::utils::exec_sync("git diff HEAD");
        if (!fall) return std::unexpected(Error::make(
            ErrorCode::ToolExecutionFailed,
            std::format("Cannot collect branch diff: {}", fall.error())
        ));
        diff = std::move(fall);
    }

    std::ostringstream out;
    out << "## GIT STATUS\n\n```\n";
    if (status) out << *status;
    out << "\n```\n\n";

    out << "## FILES MODIFIED\n\n```\n";
    if (files) out << *files;
    out << "\n```\n\n";

    out << "## COMMITS\n\n```\n";
    if (log) out << *log;
    out << "\n```\n\n";

    out << "## DIFF CONTENT\n\n```diff\n";
    out << *diff;
    out << "\n```\n";

    return out.str();
}

// ============================================================
// Secret scanner (REUSE — NOT reimplemented)
// ============================================================

/// Run the team-memory secret scanner across all modified-file contents.
/// Returns a list of SecurityFindings for any matches.
///
/// IMPORTANT: the regex list lives in secret_scanner.cppm.  We do NOT copy it
/// here — the scan is delegated.
[[nodiscard]] inline std::vector<SecurityFinding> scan_diff_for_secrets(
    std::string_view diff_content
) {
    std::vector<SecurityFinding> findings;

    auto matches = cc::services::team_memory::scan_for_secrets(diff_content);
    if (matches.empty()) return findings;

    // Convert each SecretMatch into a SecurityFinding.
    // We don't have file/line resolution from the simple scanner, so we tag
    // them as "unknown file" with a generic location; the LLM downstream will
    // refine these with exact locations.
    for (const auto& m : matches) {
        findings.push_back(SecurityFinding{
            .file = "(detected in diff — see LLM refinement below)",
            .line = 0,
            .type = std::string("hardcoded_secret:") + m.ruleId,
            .severity = SecuritySeverity::High,
            .evidence = std::format("Secret pattern '{}' ({}) matched in branch diff.", m.ruleId, m.label),
            .recommendation = "Move this secret to a secrets manager (1Password, AWS Secrets Manager, "
                              "Vault, or environment variables) and revoke the leaked value."
        });
    }
    return findings;
}

// ============================================================
// Prompt builder
// ============================================================

/// Build the full security-review prompt.
/// Combines: OWASP checklist + pre-scanned secret findings + exclusions.
[[nodiscard]] inline std::string build_security_prompt(
    const std::string& diff_block,
    const std::vector<SecurityFinding>& pre_scanned_secrets
) {
    std::ostringstream out;
    out << R"(# Security Review

You are a senior security engineer conducting a **focused, high-confidence** review of
the code changes on this branch.  You report ONLY concrete vulnerabilities — not
theoretical issues, style concerns, or best-practice reminders.

---

## Branch context

)";
    out << diff_block;
    out << "\n---\n\n";

    out << "## Pre-scan results (from secret scanner)\n\n";
    if (pre_scanned_secrets.empty()) {
        out << "No hardcoded-secret patterns matched during the pre-scan. Continue with manual analysis.\n\n";
    } else {
        out << "The following findings came from the automated secret scanner. "
               "Refine them: give exact file + line + snippet, remove false positives, "
               "add any the scanner missed.\n\n";
        for (const auto& f : pre_scanned_secrets) {
            out << std::format("  - [{}] {}: {} — {}\n",
                               severity_label(f.severity), f.type, f.file, f.recommendation);
        }
        out << "\n";
    }

    out << R"(## Categories to examine (OWASP Top 10 + sensitive operations)

For EVERY modified file, check each of the following categories and only
report issues you are >80% confident are real and exploitable.

### 1 — Input validation & injection
- SQL injection via unsanitized user input
- Command injection in system calls / subprocesses
- XXE injection in XML parsing
- Template engine injection
- NoSQL injection
- Path traversal in file operations

### 2 — Authentication & authorization
- Authentication bypass logic
- Privilege escalation paths
- Session management flaws (cookie flags, expiration)
- JWT algorithm confusion, missing signature checks
- Authorization logic bypasses on endpoints

### 3 — Cryptography & secrets
- Weak / deprecated algorithms (MD5, SHA1, DES, RC4, ECB)
- Insecure randomness (non-crypto RNGs for tokens)
- Hardcoded keys / passwords / tokens (cross-check with pre-scan)
- Certificate validation bypasses
- Improper key storage / management

### 4 — Injection & code execution
- Deserialization of untrusted data (pickle, Java, YAML::Load)
- Eval / Function() / dynamic code execution with user input
- XSS: reflected, stored, DOM-based (NOT in React/Angular unless
  dangerouslySetInnerHTML / bypassSecurityTrustHtml is used)

### 5 — Data exposure
- PII / credentials logged in plaintext
- Debug endpoints / stack traces exposed in production
- Sensitive fields echoed in responses

### 6 — Server-side request forgery (SSRF)
- HTTP calls to a user-controllable HOST or PROTOCOL
- Path-only control is NOT SSRF; only host/protocol control counts

### 7 — Sensitive operation patterns to cross-check
)";
    for (auto p : SENSITIVE_OP_PATTERNS) out << "  - " << p << "\n";
    out << "\n";

    out << R"(## Hard exclusions — DO NOT report these

1. Denial-of-service, memory/CPU exhaustion, rate-limiting, resource leaks
2. Secrets on disk if they are otherwise secured / in test files only
3. Memory safety issues in Rust or any memory-safe language
4. Outdated / vulnerable third-party libraries (handled by SCA separately)
5. Race conditions unless concretely exploitable
6. SSRF where only the *path* is controlled
7. Prompt injection (AI system prompts receiving user content)
8. Regex injection / ReDoS
9. Insecure documentation (markdown files)
10. Log spoofing concerns
11. GitHub Action workflow issues unless a very specific attack path exists
12. Client-side permission/auth checks in JS/TS (server-side enforces these)
13. Lack of input validation on non-security-critical fields
14. Tabnabbing, XS-Leaks, prototype pollution, open redirects unless extreme confidence
15. Unit test files, ipynb notebooks unless very specific untrusted-input path exists
16. Shell scripts where there is no untrusted user-input flow
17. Issues requiring an attacker to already control environment variables

---

## Analysis methodology

1. **Phase 1 — Repo context** (use Glob/Grep if needed): find existing security
   frameworks, sanitization helpers, validation patterns.
2. **Phase 2 — Comparative analysis**: compare new code against established
   secure patterns in the same codebase.  Flag deviations.
3. **Phase 3 — Per-file trace**: for each modified file, trace data flow from
   user inputs through to sensitive operations.  Identify privilege-boundary
   crossings.
4. **Phase 4 — False-positive filter**: for each candidate finding, ask
   "does a concrete, exploitable attack path exist with specific code lines?"
   Drop anything scoring below 7/10 confidence.

---

## Required output format

Produce your findings in markdown using the template below for each finding.
At the end, include a summary markdown table:

```
| Severity | File:Line | Category | Summary |
|----------|-----------|----------|---------|
```

### Finding template (repeat per issue)

```
# Vuln N: <CATEGORY>: `<file>:<line>`

* Severity: HIGH | MEDIUM | LOW
* Description: <what is wrong + the data flow from input to sink>
* Exploit Scenario: <concrete, step-by-step, URL example, code snippet if possible>
* Recommendation: <exact code change or configuration fix>
* Confidence: <1–10>
```

If you find **zero** HIGH or MEDIUM issues, output exactly:

> No high-confidence security issues found in this diff.  The changes do not
> appear to introduce exploitable vulnerabilities.

---

## Severity guidelines

* **HIGH**: Directly exploitable RCE / data breach / auth bypass with clear attack path
* **MEDIUM**: Exploitable under specific conditions but still significant impact
* **LOW**: Defense-in-depth; would require chained bugs or highly unlikely scenarios

## Now begin

Start with Phase 1.  Your final reply must contain only the findings report — no commentary.
)";

    return out.str();
}

// ============================================================
// Command class
// ============================================================

/// SecurityReviewCommand implements the `/security-review [base-ref]` slash
/// command.  Replaces the stub that only told the user the command "moved to
/// a plugin".
class SecurityReviewCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "security-review",
            .description = "Security audit of pending branch changes (OWASP Top 10 + secrets)",
            .aliases = {"sec-review", "audit"},
            .args = {
                CommandArg{
                    .name = "base_ref",
                    .description = "Git ref to diff against (default: origin/HEAD)",
                    .type = ArgType::Text,
                    .required = false,
                },
                CommandArg{
                    .name = "--json",
                    .description = "Emit structured JSON findings in addition to markdown",
                    .type = ArgType::None,
                    .required = false,
                },
                CommandArg{
                    .name = "--skip-secrets",
                    .description = "Skip the pre-scan secret check (LLM-only analysis)",
                    .type = ArgType::None,
                    .required = false,
                },
            },
            .hidden = false,
            .category = "security",
        };
    }

    [[nodiscard]] static VoidResult validate(const CommandContext&) {
        // No required arguments
        return {};
    }

    [[nodiscard]] static Result<CommandResult> execute(const CommandContext& ctx) {
        std::string base_ref;
        bool skip_secrets = false;
        for (const auto& arg : ctx.args) {
            if (arg == "--skip-secrets") skip_secrets = true;
            else if (arg == "--json") { /* handled in prompt / downstream */ }
            else if (!arg.starts_with("-")) {
                if (!base_ref.empty()) base_ref += ' ';
                base_ref += arg;
            }
        }

        // 1) Collect diff
        auto diff = collect_branch_diff(base_ref);
        if (!diff) return std::unexpected(diff.error());
        if (diff->find("DIFF CONTENT") == std::string::npos ||
            diff->find("FILES MODIFIED") == std::string::npos) {
            return CommandResult::fail(
                "Cannot determine which files changed.  Make sure you are in a git "
                "repo with at least one commit beyond origin/HEAD."
            );
        }

        // 2) Pre-scan secrets (REUSE secret_scanner, do NOT reimplement patterns)
        std::vector<SecurityFinding> pre_scan;
        if (!skip_secrets) {
            pre_scan = scan_diff_for_secrets(*diff);
        }

        // 3) Build prompt and inject.  LLM API calls go through query_engine.
        auto prompt = build_security_prompt(*diff, pre_scan);
        return CommandResult::inject(std::move(prompt));
    }

    [[nodiscard]] static std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        static constexpr std::array flags = {"--json", "--skip-secrets"};
        for (auto flag : flags) {
            if (std::string_view(flag).starts_with(partial)) {
                suggestions.emplace_back(flag);
            }
        }
        return suggestions;
    }
};

} // namespace cc::commands
