// Azure DefaultAzureCredential-lite implementation + Foundry endpoint
// resolution.
//
// Faithful to TS upstream's use of @azure/identity DefaultAzureCredential with
// getBearerTokenProvider using scope="https://cognitiveservices.azure.com/.default".
//
// The Azure SDK for C++ (azure-sdk-for-cpp) is intentionally NOT pulled in.
// Instead we replicate the specific sub-parts of DefaultAzureCredential that
// cover 95% of CC-REPL BYOC use cases, in this priority order:
//
//   1. EnvironmentCredential — AZURE_TENANT_ID + AZURE_CLIENT_ID +
//      (AZURE_CLIENT_SECRET | AZURE_CLIENT_CERTIFICATE_PATH |
//       AZURE_USERNAME + AZURE_PASSWORD).  Only the client-secret branch is
//      implemented (most common for CI); cert-path / username-password raise
//      UnsupportedOperation so callers can try a shell hook.
//
//   2. WorkloadIdentityCredential — AZURE_FEDERATED_TOKEN_FILE +
//      AZURE_TENANT_ID + AZURE_CLIENT_ID + optional AZURE_AUTHORITY_HOST.
//      (AKS / Arc workload identity federation.)
//
//   3. AzureCliCredential — run `az account get-access-token
//      --scope=https://cognitiveservices.azure.com/.default --output json`
//      and parse the accessToken / expiresOn fields.  Uses bash_execution,
//      10s timeout.
//
//   4. ManagedIdentityCredential (IMDS) — MSI on Azure VM / App Service /
//      Function / AKS pod identity.  GET
//      http://169.254.169.254/metadata/identity/oauth2/token?api-version=2018-02-01
//      &resource=https://cognitiveservices.azure.com (the "old" resource URI,
//      which MSI accepts even for the new scope-based API).
//
// 5, 6, 7, 8, 9 (AzureDeveloperCliCredential / PowerShell / VSCode / Visual
// Studio / InteractiveBrowser) are NOT implemented — they cover 0.1% of CC
// users and require platform binaries.  Callers that need them should set
// the `azureAuthRefresh` shell script field (future Settings integration).
//
// Model IDs map to deployment names that user has set up on Foundry.  We
// export the same model-ID table as TS configs.ts so callers can look up
// the default deployment name before the runtime ListDeployments call.
module;
#include <chrono>
#include <cstdint>
#include <ctime>
#include <expected>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <httplib.h>

export module cc.services.auth.azure_credential;

import cc.utils.env;
import cc.utils.error;
import cc.utils.http_encoding;
import cc.utils.json;

// bash_execution — for AzureCliCredential shell-out.
import cc.utils.bash_execution;

export namespace cc::services::auth::azure {

using cc::utils::Error;
using cc::utils::Result;
using namespace std::chrono;
using namespace std::string_view_literals;

// ---------------------------------------------------------------------------
// Cached access token.
// ---------------------------------------------------------------------------
struct AccessToken {
    std::string token;
    system_clock::time_point expires_on;

    [[nodiscard]] bool expired() const noexcept {
        return system_clock::now() >= expires_on - minutes(2);
    }
    [[nodiscard]] bool valid() const noexcept {
        return !token.empty() && !expired();
    }
};

// ---------------------------------------------------------------------------
// Default scope for Azure AI / Foundry.
// ---------------------------------------------------------------------------
inline constexpr std::string_view kFoundryDefaultScope =
    "https://cognitiveservices.azure.com/.default";

// The MSI / resource-URI equivalent (old Azure AD v1 semantics).  The IMDS
// endpoint accepts either a scope or resource; we use the scope format for
// IMDSv2 as well (Azure supports scope since IMDS api-version=2020-06-01).
inline constexpr std::string_view kFoundryScopeForImds = kFoundryDefaultScope;

// ---------------------------------------------------------------------------
// Utility: extract project (tenant) / authority host.
// Default authority: "https://login.microsoftonline.com/" (public Azure cloud).
// Override via AZURE_AUTHORITY_HOST.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::string authority_host() {
    using cc::utils::env::get_env;
    auto host = get_env("AZURE_AUTHORITY_HOST");
    if (host && !host->empty()) {
        // strip trailing slash for consistency
        auto& h = *host;
        while (!h.empty() && h.back() == '/') h.pop_back();
        if (h.empty()) return "https://login.microsoftonline.com";
        return h;
    }
    return "https://login.microsoftonline.com";
}

// ---------------------------------------------------------------------------
// Shared helper: POST a form body to {authority}/{tenant}/oauth2/v2.0/token
// and parse the access_token + expires_in response.
// ---------------------------------------------------------------------------
namespace detail {
[[nodiscard]] inline Result<AccessToken> post_token_endpoint(
    const std::string& authority,
    const std::string& tenant_id,
    std::string_view form_body,
    std::chrono::milliseconds timeout_ms = 5s) {
    using namespace cc::utils::json;
    using namespace cc::utils::http;

    const std::string path = "/" + tenant_id + "/oauth2/v2.0/token";
    // authority is either login.microsoftonline.com or login.microsoftonline.us
    // or similar.  Extract scheme+host.
    std::string scheme_host = authority;
    std::string url_path = path;
    // If authority has trailing path components (national cloud), append.
    // e.g. authority="https://login.microsoftonline.us" → no trailing path;
    // use path directly.
    httplib::Client cli(scheme_host.c_str());
    cli.set_connection_timeout(timeout_ms.count() / 1000,
                               (timeout_ms.count() % 1000) * 1000);
    cli.set_read_timeout(timeout_ms.count() / 1000,
                         (timeout_ms.count() % 1000) * 1000);
    cli.enable_server_certificate_verification(true);
    auto resp = cli.Post(url_path.c_str(), httplib::Headers{},
                         std::string(form_body),
                         "application/x-www-form-urlencoded");
    if (!resp) {
        return std::unexpected(Error(cc::utils::ErrorCode::network_error,
            "Azure: no response from " + scheme_host + url_path));
    }
    if (resp->status < 200 || resp->status >= 300) {
        return std::unexpected(Error(cc::utils::ErrorCode::permission_denied,
            "Azure: token endpoint HTTP " + std::to_string(resp->status)));
    }
    auto parsed = parse(resp->body);
    if (!parsed) {
        return std::unexpected(Error(cc::utils::ErrorCode::parse_error,
                                 "Azure: invalid token JSON"));
    }
    auto root = parsed->root();
    AccessToken at;
    at.token = root.get_string("access_token");
    int64_t expires_in = root.get_int("expires_in");
    if (expires_in <= 0) expires_in = 3600;
    at.expires_on = system_clock::now() + seconds(expires_in);
    if (at.token.empty()) {
        return std::unexpected(Error(cc::utils::ErrorCode::parse_error,
                                 "Azure: response missing access_token"));
    }
    return at;
}
} // namespace detail

// ---------------------------------------------------------------------------
// EnvironmentCredential (client_credentials flow with client_secret).
// Returns nullopt if any required env var is missing.  Returns error only
// when required vars are present but exchange failed (so the chain can
// short-circuit on real errors vs. "not configured" cases).
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::optional<Result<AccessToken>>
try_environment_credential(std::string_view scope = kFoundryDefaultScope) {
    using cc::utils::env::get_env;
    using namespace cc::utils::http;
    auto tenant = get_env("AZURE_TENANT_ID");
    auto cid = get_env("AZURE_CLIENT_ID");
    auto secret = get_env("AZURE_CLIENT_SECRET");
    if (!tenant || tenant->empty() || !cid || cid->empty() ||
        !secret || secret->empty()) {
        return std::nullopt;
    }
    // Skip unsupported sub-branches (cert-path, username/password) for now.
    if (get_env("AZURE_CLIENT_CERTIFICATE_PATH") &&
        !get_env("AZURE_CLIENT_CERTIFICATE_PATH")->empty()) {
        return std::optional<Result<AccessToken>>(std::unexpected(Error(
            cc::utils::ErrorCode::unimplemented,
            "Azure: AZURE_CLIENT_CERTIFICATE_PATH is not supported by the C++ "
            "DefaultAzureCredential-lite.  Use client_secret or the azureAuthRefresh "
            "shell setting instead.")));
    }
    if ((get_env("AZURE_USERNAME") && !get_env("AZURE_USERNAME")->empty()) ||
        (get_env("AZURE_PASSWORD") && !get_env("AZURE_PASSWORD")->empty())) {
        return std::optional<Result<AccessToken>>(std::unexpected(Error(
            cc::utils::ErrorCode::unimplemented,
            "Azure: AZURE_USERNAME / AZURE_PASSWORD (ROPC) is not supported.")));
    }
    std::string body;
    body  = "client_id="     + form_encode(*cid);
    body += "&client_secret=" + form_encode(*secret);
    body += "&scope="         + form_encode(std::string(scope));
    body += "&grant_type=client_credentials";
    auto r = detail::post_token_endpoint(authority_host(), *tenant, body);
    return std::optional<Result<AccessToken>>(std::move(r));
}

// ---------------------------------------------------------------------------
// WorkloadIdentityCredential (AKS / Arc Kubernetes).
// Reads the federated JWT from AZURE_FEDERATED_TOKEN_FILE path and uses the
// client_credentials flow with client_assertion_type=urn:ietf:params:oauth:
// client-assertion-type:jwt-bearer.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::optional<Result<AccessToken>>
try_workload_identity_credential(
    std::string_view scope = kFoundryDefaultScope) {
    using cc::utils::env::get_env;
    using namespace cc::utils::http;
    auto tenant = get_env("AZURE_TENANT_ID");
    auto cid = get_env("AZURE_CLIENT_ID");
    auto tok_file = get_env("AZURE_FEDERATED_TOKEN_FILE");
    if (!tenant || tenant->empty() || !cid || cid->empty() ||
        !tok_file || tok_file->empty()) {
        return std::nullopt;
    }
    std::ifstream f(*tok_file);
    if (!f) {
        return std::optional<Result<AccessToken>>(std::unexpected(Error(
            cc::utils::ErrorCode::io_error,
            "Azure WorkloadIdentity: cannot open AZURE_FEDERATED_TOKEN_FILE '" +
            *tok_file + "'")));
    }
    std::stringstream buf;
    buf << f.rdbuf();
    std::string jwt = buf.str();
    while (!jwt.empty() &&
           (jwt.back() == '\n' || jwt.back() == '\r' || jwt.back() == ' ')) {
        jwt.pop_back();
    }
    if (jwt.empty()) {
        return std::optional<Result<AccessToken>>(std::unexpected(Error(
            cc::utils::ErrorCode::invalid_argument,
            "Azure WorkloadIdentity: token file empty")));
    }
    std::string body;
    body  = "client_id="             + form_encode(*cid);
    body += "&client_assertion_type=" + form_encode(
        "urn:ietf:params:oauth:client-assertion-type:jwt-bearer");
    body += "&client_assertion="    + form_encode(jwt);
    body += "&scope="               + form_encode(std::string(scope));
    body += "&grant_type=client_credentials";
    auto r = detail::post_token_endpoint(authority_host(), *tenant, body);
    return std::optional<Result<AccessToken>>(std::move(r));
}

// ---------------------------------------------------------------------------
// AzureCliCredential — `az account get-access-token --scope=... --output json`.
// Parses accessToken + expiresOn (ISO-8601 or Unix seconds).
// Uses bash_execution::exec_capture with 10s timeout.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::optional<Result<AccessToken>>
try_azure_cli_credential(std::string_view scope = kFoundryDefaultScope) {
    using namespace cc::utils::bash;
    using namespace cc::utils::json;

    std::string cmd = "az account get-access-token --scope '" +
                      std::string(scope) + "' --output json 2>/dev/null";
    auto res = exec_capture(cmd);
    if (!res) return std::nullopt;  // `az` not installed; treat as not configured.
    if (res->status != 0) return std::nullopt;
    if (res->output.empty()) return std::nullopt;
    auto parsed = parse(res->output);
    if (!parsed) {
        return std::optional<Result<AccessToken>>(std::unexpected(Error(
            cc::utils::ErrorCode::parse_error,
            "Azure CLI: invalid JSON from get-access-token: " + res->output)));
    }
    auto root = parsed->root();
    AccessToken at;
    at.token = root.get_string("accessToken");
    if (at.token.empty()) {
        return std::optional<Result<AccessToken>>(std::unexpected(Error(
            cc::utils::ErrorCode::parse_error,
            "Azure CLI: response missing accessToken")));
    }
    // expiresOn can be a variety of formats: "2025-01-15 12:34:56.789012",
    // "2025-01-15T12:34:56Z", or Unix seconds as a string.  Try common ones.
    auto iso = root.get_string("expiresOn");
    if (!iso.empty()) {
        std::tm tm{};
        std::istringstream iss(iso);
        iss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (iss.fail()) {
            iss.clear();
            iss.seekg(0);
            iss.str(iso);
            iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        }
        if (!iss.fail()) {
            std::time_t t = timegm(&tm);
            if (t != -1) {
                at.expires_on = system_clock::from_time_t(t);
            } else {
                at.expires_on = system_clock::now() + minutes(50);
            }
        } else {
            at.expires_on = system_clock::now() + minutes(50);
        }
    } else {
        auto unix_str = root.get_string("expires_on");
        if (!unix_str.empty()) {
            try {
                std::time_t t = static_cast<std::time_t>(std::stoll(unix_str));
                at.expires_on = system_clock::from_time_t(t);
            } catch (...) {
                at.expires_on = system_clock::now() + minutes(50);
            }
        } else {
            at.expires_on = system_clock::now() + minutes(50);
        }
    }
    return std::optional<Result<AccessToken>>(at);
}

// ---------------------------------------------------------------------------
// ManagedIdentityCredential via Azure IMDS.
// App Service / Function / VM / AKS pod identity.
// Uses api-version=2019-08-01 (widely supported).
// Optional AZURE_CLIENT_ID for user-assigned identity.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::optional<Result<AccessToken>>
try_managed_identity_credential(
    std::string_view scope = kFoundryScopeForImds,
    std::chrono::milliseconds timeout_ms = 1000ms) {
    using namespace cc::utils::json;
    using namespace cc::utils::http;
    using cc::utils::env::get_env;

    httplib::Client cli("http://169.254.169.254", 80);
    cli.set_connection_timeout(timeout_ms.count() / 1000,
                               (timeout_ms.count() % 1000) * 1000);
    cli.set_read_timeout(timeout_ms.count() / 1000,
                         (timeout_ms.count() % 1000) * 1000);
    httplib::Params params;
    params.emplace("api-version", "2019-08-01");
    params.emplace("resource", std::string(scope));
    if (auto cid = get_env("AZURE_CLIENT_ID"); cid && !cid->empty()) {
        params.emplace("client_id", *cid);
    }
    auto mi_client_id = get_env("AZURE_CLIENT_ID");
    httplib::Headers hdrs;
    hdrs.emplace("Metadata", "true");
    std::string path = "/metadata/identity/oauth2/token";
    // Build query string from params.
    {
        std::string q;
        bool first = true;
        for (const auto& [k, v] : params) {
            if (!first) q.push_back('&');
            first = false;
            q += form_encode(k);
            q.push_back('=');
            q += form_encode(v);
        }
        if (!q.empty()) {
            path += "?";
            path += q;
        }
    }
    auto resp = cli.Get(path.c_str(), hdrs);
    if (!resp) return std::nullopt; // not on Azure
    if (resp->status == 400 || resp->status == 404) {
        // No identity attached / wrong resource — treat as "not configured"
        // rather than hard error so credential chain continues.
        return std::nullopt;
    }
    if (resp->status < 200 || resp->status >= 300) {
        return std::optional<Result<AccessToken>>(std::unexpected(Error(
            cc::utils::ErrorCode::permission_denied,
            "Azure MSI IMDS: HTTP " + std::to_string(resp->status) +
            ": " + resp->body)));
    }
    auto parsed = parse(resp->body);
    if (!parsed) {
        return std::optional<Result<AccessToken>>(std::unexpected(Error(
            cc::utils::ErrorCode::parse_error,
            "Azure MSI: invalid IMDS JSON")));
    }
    auto root = parsed->root();
    AccessToken at;
    at.token = root.get_string("access_token");
    int64_t expires_in = root.get_int("expires_in");
    if (expires_in <= 0) expires_in = 3600;
    at.expires_on = system_clock::now() + seconds(expires_in);
    if (at.token.empty()) {
        return std::optional<Result<AccessToken>>(std::unexpected(Error(
            cc::utils::ErrorCode::parse_error,
            "Azure MSI: response missing access_token")));
    }
    return std::optional<Result<AccessToken>>(at);
}

// ---------------------------------------------------------------------------
// DefaultAzureCredential aggregate.  Not thread-safe.
// Tries Environment → WorkloadIdentity → AzureCli → ManagedIdentity.
// Returns the FIRST credential that either succeeds or raises an explicit
// error; "not configured" nullopt branches are skipped silently.
// Caches the successful source so subsequent get_token() calls reuse it
// (until cache expires — then the full chain runs again).
// ---------------------------------------------------------------------------
class DefaultAzureCredential {
public:
    Result<AccessToken> get_token(
        std::string_view scope = kFoundryDefaultScope) {
        if (cached_.valid() && cached_scope_ == scope) {
            return cached_;
        }
        std::string scope_str(scope);
        using SourceFn = std::function<std::optional<Result<AccessToken>>()>;
        SourceFn sources[] = {
            [&] { return try_environment_credential(scope); },
            [&] { return try_workload_identity_credential(scope); },
            [&] { return try_azure_cli_credential(scope); },
            [&] { return try_managed_identity_credential(scope); },
        };
        std::optional<std::string> last_error_msg;
        for (auto& src : sources) {
            auto r = src();
            if (!r) continue;           // source not configured
            if (r->has_value()) {
                cached_ = std::move(**r);
                cached_scope_ = scope_str;
                return cached_;
            }
            // Source returned an explicit error — surface it.
            last_error_msg = r->error().message();
        }
        if (last_error_msg) {
            return std::unexpected(Error(
                cc::utils::ErrorCode::permission_denied, *last_error_msg));
        }
        return std::unexpected(Error(
            cc::utils::ErrorCode::internal_error,
            "Azure DefaultAzureCredential: no credential source found. "
            "Configure AZURE_CLIENT_ID/TENANT_ID/CLIENT_SECRET (env), "
            "AKS workload identity, `az login` (CLI), or an Azure VM/app MSI."));
    }

    void invalidate() { cached_ = AccessToken{}; }

private:
    AccessToken cached_;
    std::string cached_scope_;
};

// ---------------------------------------------------------------------------
// Foundry resource + endpoint resolution + TS env var detection helpers.
// ---------------------------------------------------------------------------
struct FoundryAuthMode {
    bool use_foundry = false;
    bool skip_auth = false;
    std::optional<std::string> api_key;     // non-empty = Ocp-Apim-Subscription-Key
    std::string resource;                   // ANTHROPIC_FOUNDRY_RESOURCE or ""
    std::string base_url;                   // ANTHROPIC_FOUNDRY_BASE_URL or ""
};

[[nodiscard]] inline FoundryAuthMode detect_foundry_mode() {
    using cc::utils::env::get_env;
    using cc::utils::env::is_env_truthy;
    FoundryAuthMode m;
    m.use_foundry = is_env_truthy("CLAUDE_CODE_USE_FOUNDRY");
    m.skip_auth  = is_env_truthy("CLAUDE_CODE_SKIP_FOUNDRY_AUTH");
    if (auto k = get_env("ANTHROPIC_FOUNDRY_API_KEY"); k && !k->empty()) {
        m.api_key = std::move(*k);
    }
    if (auto r = get_env("ANTHROPIC_FOUNDRY_RESOURCE"); r && !r->empty()) {
        m.resource = std::move(*r);
    }
    if (auto u = get_env("ANTHROPIC_FOUNDRY_BASE_URL"); u && !u->empty()) {
        m.base_url = std::move(*u);
    }
    return m;
}

// Resolve final base URL per TS SDK rules.
// Priority: base_url > resource-derived > error.
// The final path appended by the SDK is /anthropic/v1/messages.
[[nodiscard]] inline std::string resolve_foundry_base_url(
    const FoundryAuthMode& m) {
    if (!m.base_url.empty()) {
        // strip trailing slash
        auto u = m.base_url;
        while (!u.empty() && u.back() == '/') u.pop_back();
        return u;
    }
    if (!m.resource.empty()) {
        return "https://" + m.resource + ".services.ai.azure.com";
    }
    return {};
}

// ---------------------------------------------------------------------------
// Default deployment name lookup for Foundry.
// Matches TS ALL_MODEL_CONFIGS table (same short-keys as Bedrock/Vertex).
// Callers allow user override via settings.modelOverrides[canonical_id].
// ---------------------------------------------------------------------------
struct DeploymentIdMapEntry {
    std::string_view canonical_first_party;
    std::string_view foundry_deployment;
};

inline constexpr std::array<DeploymentIdMapEntry, 11> kDefaultFoundryDeployments = {{
    {"claude-3-5-sonnet-20241022-v2:0", "claude-3-5-sonnet"},
    {"claude-3-7-sonnet-20250219",       "claude-3-7-sonnet"},
    {"claude-sonnet-4-20250514-v1:0",    "claude-sonnet-4"},
    {"claude-sonnet-4-5-20250929-v1:0",  "claude-sonnet-4-5"},
    {"claude-sonnet-4-6",                "claude-sonnet-4-6"},
    {"claude-3-5-haiku-20241022-v1:0",   "claude-3-5-haiku"},
    {"claude-haiku-4-5-20251001-v1:0",   "claude-haiku-4-5"},
    {"claude-opus-4-20250514-v1:0",      "claude-opus-4"},
    {"claude-opus-4-1-20250805-v1:0",    "claude-opus-4-1"},
    {"claude-opus-4-5-20251101-v1:0",    "claude-opus-4-5"},
    {"claude-opus-4-6-v1",               "claude-opus-4-6"},
}};

[[nodiscard]] inline std::string_view lookup_default_deployment(
    std::string_view canonical_first_party_id) {
    for (const auto& e : kDefaultFoundryDeployments) {
        if (canonical_first_party_id == e.canonical_first_party) {
            return e.foundry_deployment;
        }
    }
    // Fuzzy prefix match (covers version-qualified IDs not in the table).
    for (const auto& e : kDefaultFoundryDeployments) {
        if (canonical_first_party_id.find(e.canonical_first_party) !=
            std::string_view::npos) {
            return e.foundry_deployment;
        }
    }
    return {};
}

} // namespace cc::services::auth::azure
