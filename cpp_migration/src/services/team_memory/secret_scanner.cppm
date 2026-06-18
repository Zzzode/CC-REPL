/// @file secret_scanner.cppm
/// @brief Client-side secret scanner for team memory (PSR M22174).
///
/// Mirrors src/services/teamMemorySync/secretScanner.ts. Scans content for
/// credentials before upload so secrets never leave the user's machine. Uses
/// a curated subset of high-confidence gitleaks rules
/// (https://github.com/gitleaks/gitleaks, MIT license) — only rules with
/// distinctive prefixes that have near-zero false-positive rates are included.
///
/// Rule IDs and regex sources are taken directly from the TS implementation,
/// which in turn sources them from the public gitleaks config. Trailing
/// boundary alternations like (?:[\x60'"\s;]|\\[nr]|$) from the Go regex
/// originals are kept (std::regex ECMAScript supports them, including `$`
/// end-of-string). Inline (?i)/(?-i:) groups are rewritten with explicit
/// character classes per the TS notes.
module;
#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
export module cc.services.team_memory.secret_scanner;
export namespace cc::services::team_memory {

/// A detected secret. Matches SecretMatch in TS: only the gitleaks rule id and
/// a human-readable label are exposed — the matched text is intentionally NOT
/// returned so secret values are never logged or displayed.
struct SecretMatch {
    std::string ruleId;  // gitleaks rule id, e.g. "github-pat"
    std::string label;   // human-readable label, e.g. "GitHub PAT"
};

namespace detail {

struct SecretRule {
    std::string id;       // gitleaks rule id (kebab-case)
    std::string source;   // regex source, ECMAScript syntax
    bool case_insensitive = false;
};

// Anthropic API key prefix, assembled at runtime so the literal byte sequence
// is not present verbatim (mirrors the TS excluded-strings dodge).
inline std::string anthropic_key_prefix() {
    return std::string{"sk"} + "-" + "ant" + "-" + "api";
}

// Portable subset of the TS SECRET_RULES. std::regex ECMAScript syntax does
// not support inline (?i) flags, so case-insensitive rules set the flag bit
// instead and use explicit character classes where the TS does.
inline const std::vector<SecretRule>& secret_rules() {
    static const std::vector<SecretRule> rules = {
        // — Cloud providers —
        {"aws-access-token",
         R"(\b((?:A3T[A-Z0-9]|AKIA|ASIA|ABIA|ACCA)[A-Z2-7]{16})\b)"},
        {"gcp-api-key",
         R"(\b(AIza[\w-]{35})(?:[\x60'"\s;]|\\[nr]|$))"},
        {"azure-ad-client-secret",
         R"((?:^|[\\'"`\s>=:(,)])([a-zA-Z0-9_~.]{3}\dQ~[a-zA-Z0-9_~.-]{31,34})(?:$|[\\'"`\s<),]))"},
        {"digitalocean-pat",
         R"(\b(dop_v1_[a-f0-9]{64})(?:[\x60'"\s;]|\\[nr]|$))"},
        {"digitalocean-access-token",
         R"(\b(doo_v1_[a-f0-9]{64})(?:[\x60'"\s;]|\\[nr]|$))"},

        // — AI APIs —
        {"anthropic-api-key",
         "\\b(" + anthropic_key_prefix() +
             R"(03-[a-zA-Z0-9_\-]{93}AA)(?:[\x60'"\s;]|\\[nr]|$))"},
        {"anthropic-admin-api-key",
         R"(\b(sk-ant-admin01-[a-zA-Z0-9_\-]{93}AA)(?:[\x60'"\s;]|\\[nr]|$))"},
        {"openai-api-key",
         R"(\b(sk-(?:proj|svcacct|admin)-(?:[A-Za-z0-9_-]{74}|[A-Za-z0-9_-]{58})T3BlbkFJ(?:[A-Za-z0-9_-]{74}|[A-Za-z0-9_-]{58})\b|sk-[a-zA-Z0-9]{20}T3BlbkFJ[a-zA-Z0-9]{20})(?:[\x60'"\s;]|\\[nr]|$))"},
        {"huggingface-access-token",
         R"(\b(hf_[a-zA-Z]{34})(?:[\x60'"\s;]|\\[nr]|$))"},

        // — Version control —
        {"github-pat", "ghp_[0-9a-zA-Z]{36}"},
        {"github-fine-grained-pat", "github_pat_\\w{82}"},
        {"github-app-token", "(?:ghu|ghs)_[0-9a-zA-Z]{36}"},
        {"github-oauth", "gho_[0-9a-zA-Z]{36}"},
        {"github-refresh-token", "ghr_[0-9a-zA-Z]{36}"},
        {"gitlab-pat", "glpat-[\\w-]{20}"},
        {"gitlab-deploy-token", "gldt-[0-9a-zA-Z_\\-]{20}"},

        // — Communication —
        {"slack-bot-token", "xoxb-[0-9]{10,13}-[0-9]{10,13}[a-zA-Z0-9-]*"},
        {"slack-user-token", "xox[pe](?:-[0-9]{10,13}){3}-[a-zA-Z0-9-]{28,34}"},
        {"slack-app-token", "xapp-\\d-[A-Z0-9]+-\\d+-[a-z0-9]+", /*ci=*/true},
        {"twilio-api-key", "SK[0-9a-fA-F]{32}"},
        {"sendgrid-api-token",
         R"(\b(SG\.[a-zA-Z0-9=_\-.]{66})(?:[\x60'"\s;]|\\[nr]|$))"},

        // — Dev tooling —
        {"npm-access-token",
         R"(\b(npm_[a-zA-Z0-9]{36})(?:[\x60'"\s;]|\\[nr]|$))"},
        {"pypi-upload-token", "pypi-AgEIcHlwaS5vcmc[\\w-]{50,1000}"},
        {"databricks-api-token",
         R"(\b(dapi[a-f0-9]{32}(?:-\d)?)(?:[\x60'"\s;]|\\[nr]|$))"},
        {"hashicorp-tf-api-token", "[a-zA-Z0-9]{14}\\.atlasv1\\.[a-zA-Z0-9\\-_=]{60,70}"},
        {"pulumi-api-token",
         R"(\b(pul-[a-f0-9]{40})(?:[\x60'"\s;]|\\[nr]|$))"},
        {"postman-api-token",
         R"(\b(PMAK-[a-fA-F0-9]{24}-[a-fA-F0-9]{34})(?:[\x60'"\s;]|\\[nr]|$))"},

        // — Observability —
        {"grafana-api-key",
         R"(\b(eyJrIjoi[A-Za-z0-9+/]{70,400}={0,3})(?:[\x60'"\s;]|\\[nr]|$))"},
        {"grafana-cloud-api-token",
         R"(\b(glc_[A-Za-z0-9+/]{32,400}={0,3})(?:[\x60'"\s;]|\\[nr]|$))"},
        {"grafana-service-account-token",
         R"(\b(glsa_[A-Za-z0-9]{32}_[A-Fa-f0-9]{8})(?:[\x60'"\s;]|\\[nr]|$))"},
        {"sentry-user-token",
         R"(\b(sntryu_[a-f0-9]{64})(?:[\x60'"\s;]|\\[nr]|$))"},

        // — Payment / commerce —
        {"stripe-access-token",
         R"(\b((?:sk|rk)_(?:test|live|prod)_[a-zA-Z0-9]{10,99})(?:[\x60'"\s;]|\\[nr]|$))"},
        {"shopify-access-token", "shpat_[a-fA-F0-9]{32}"},
        {"shopify-shared-secret", "shpss_[a-fA-F0-9]{32}"},

        // — Crypto —
        {"private-key",
         "-----BEGIN[ A-Z0-9_-]{0,100}PRIVATE KEY(?: BLOCK)?-----[\\s\\S-]{64,}?-----END[ A-Z0-9_-]{0,100}PRIVATE KEY(?: BLOCK)?-----",
         /*ci=*/true},
    };
    return rules;
}

// Lazily compiled regex cache — compiled once on first scan (parity with the
// TS compiledRules memo).
struct CompiledRule {
    std::string id;
    std::regex re;
};
inline std::shared_ptr<const std::vector<CompiledRule>> compiled_rules() {
    static std::shared_ptr<const std::vector<CompiledRule>> cache;
    static std::once_flag flag;
    std::call_once(flag, [] {
        auto out = std::make_shared<std::vector<CompiledRule>>();
        for (const auto& r : secret_rules()) {
            auto flags = std::regex::ECMAScript;
            if (r.case_insensitive) flags |= std::regex::icase;
            out->push_back({r.id, std::regex(r.source, flags)});
        }
        cache = out;
    });
    return cache;
}

/// Title-case a single hyphen word-part (e.g. "pat" -> "Pat").
inline std::string title_case(std::string_view s) {
    std::string out(s);
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

/// Convert a gitleaks rule id (kebab-case) to a human-readable label.
/// Mirrors ruleIdToLabel in secretScanner.ts.
[[nodiscard]] inline std::string rule_id_to_label(std::string_view rule_id) {
    static const std::unordered_map<std::string_view, std::string> special_case = {
        {"aws", "AWS"},         {"gcp", "GCP"},     {"api", "API"},
        {"pat", "PAT"},         {"ad", "AD"},       {"tf", "TF"},
        {"oauth", "OAuth"},     {"npm", "NPM"},     {"pypi", "PyPI"},
        {"jwt", "JWT"},         {"github", "GitHub"}, {"gitlab", "GitLab"},
        {"openai", "OpenAI"},   {"digitalocean", "DigitalOcean"},
        {"huggingface", "HuggingFace"}, {"hashicorp", "HashiCorp"},
        {"sendgrid", "SendGrid"},
    };
    std::string out;
    std::size_t start = 0;
    while (start <= rule_id.size()) {
        std::size_t dash = rule_id.find('-', start);
        std::string_view part =
            rule_id.substr(start, dash == std::string_view::npos ? std::string_view::npos
                                                                 : dash - start);
        if (!part.empty()) {
            auto it = special_case.find(part);
            out += (it != special_case.end()) ? it->second : title_case(part);
            if (dash != std::string_view::npos) out += ' ';
        }
        if (dash == std::string_view::npos) break;
        start = dash + 1;
    }
    return out;
}

} // namespace detail

/// Scan a string for potential secrets.
///
/// Returns one match per rule that fired (deduplicated by rule id, matching
/// scanForSecrets in TS). The matched text is intentionally NOT returned.
[[nodiscard]] inline std::vector<SecretMatch> scan_for_secrets(std::string_view content) {
    std::vector<SecretMatch> matches;
    std::unordered_set<std::string_view> seen;
    const auto rules = detail::compiled_rules();
    for (const auto& rule : *rules) {
        if (seen.contains(rule.id)) continue;
        if (std::regex_search(content.begin(), content.end(), rule.re)) {
            seen.insert(rule.id);
            matches.push_back({rule.id, detail::rule_id_to_label(rule.id)});
        }
    }
    return matches;
}

/// Convenience: non-empty when at least one rule fires.
[[nodiscard]] inline bool contains_secrets(std::string_view content) {
    return !scan_for_secrets(content).empty();
}

/// Get a human-readable label for a gitleaks rule id (parity with
/// getSecretLabel in TS).
[[nodiscard]] inline std::string get_secret_label(std::string_view rule_id) {
    return detail::rule_id_to_label(rule_id);
}

} // namespace cc::services::team_memory
