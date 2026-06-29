// Enterprise Auth dispatch (P0-5 BYOC).  Centralized provider detection +
// credential resolution + auth header attachment, per TS client.ts 4-way
// branch.
//
// Provider selection priority (same as `providers.ts::getAPIProvider()`):
//     1. BEDROCK    — CLAUDE_CODE_USE_BEDROCK
//     2. VERTEX     — CLAUDE_CODE_USE_VERTEX
//     3. FOUNDRY    — CLAUDE_CODE_USE_FOUNDRY
//     4. firstParty — default, api.anthropic.com
//
// Note: upstream TS has a BUG where `client.ts` branches BEDROCK > FOUNDRY >
// VERTEX while `providers.ts` says BEDROCK > VERTEX > FOUNDRY.  We choose the
// providers.ts ordering (more "cloud-native providers first") because it is
// the ordering used by `categorizeRetryableAPIError` and other downstream
// code that dispatches on `getAPIProvider()`.  If both VERTEX and FOUNDRY are
// set, VERTEX wins.  The bug is on the TS side and is being tracked there.
//
// Authentication per provider:
//
//   * Bedrock — SigV4 signed request (service=bedrock) OR Bearer token
//               (AWS_BEARER_TOKEN_BEDROCK / skip_auth proxy).
//   * Vertex  — GCP OAuth2 Bearer from ADC + Authorization header, plus
//               optional x-goog-user-project from quota_project_id.
//   * Foundry — Entra Bearer from DefaultAzureCredential, OR Ocp-Apim-Subscription-Key
//              when ANTHROPIC_FOUNDRY_API_KEY is set.
//   * 1P      — x-api-key + Authorization (OAuth Pro) pattern already used.
//
// The module exports a class `EnterpriseAuthContext` that:
//   - holds state for per-provider credential cache (so we don't re-fetch
//     access tokens on every request),
//   - exposes `resolve_auth_for_request(...)` which returns the exact
//     headers the HTTP client must attach to the call,
//   - exposes `resolve_base_url(...)` for provider-specific endpoint,
//   - exposes `resolve_model_id(...)` for provider-specific model string.
module;
#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.services.auth.provider_selector;

import cc.services.auth.sigv4;
import cc.services.auth.gcp_adc;
import cc.services.auth.azure_credential;
import cc.services.api.models;

import cc.utils.env;
import cc.utils.error;
import cc.utils.http_encoding;
import cc.utils.model.providers;

export namespace cc::services::auth::byoc {

using cc::utils::Error;
using cc::utils::Result;
using namespace std::string_view_literals;

// ---------------------------------------------------------------------------
// Provider enum (unified — superset of the 3 existing disparate provider
// enums in the codebase).  Higher-number = higher dispatch priority per the
// ordering documented above.
// ---------------------------------------------------------------------------
enum class EnterpriseProvider {
    FirstParty = 0,
    Foundry    = 1,
    Vertex     = 2,
    Bedrock    = 3,
};

// ---------------------------------------------------------------------------
// Detect the active provider from environment variables.  Call once at
// startup; environment variables are not re-read mid-session (same contract
// as TS upstream).
// ---------------------------------------------------------------------------
[[nodiscard]] inline EnterpriseProvider detect_active_provider() {
    using cc::utils::env::is_env_truthy;
    using cc::utils::env::get_env;
    auto bedrock = is_env_truthy("CLAUDE_CODE_USE_BEDROCK");
    auto vertex  = is_env_truthy("CLAUDE_CODE_USE_VERTEX");
    auto foundry = is_env_truthy("CLAUDE_CODE_USE_FOUNDRY");
    // Priority: Bedrock > Vertex > Foundry > FirstParty
    if (bedrock) return EnterpriseProvider::Bedrock;
    if (vertex)  return EnterpriseProvider::Vertex;
    if (foundry) return EnterpriseProvider::Foundry;
    return EnterpriseProvider::FirstParty;
}

// ---------------------------------------------------------------------------
// Per-request auth result: headers the HTTP client MUST attach to the
// outgoing request verbatim.  Authorization header value included.
// For Bedrock, also contains the canonical endpoint URL (host + path + query)
// because SigV4 signs over the full URI — any rewrite by a proxy after
// signing would invalidate the signature.
// ---------------------------------------------------------------------------
struct ResolvedAuth {
    EnterpriseProvider provider = EnterpriseProvider::FirstParty;
    // Headers map (lowercase name → value).  These are ADDITIVE on top of any
    // provider-agnostic headers (User-Agent, x-app, X-Claude-Code-Session-Id,
    // Content-Type, Accept).  If a header here collides with one in the
    // agnostic set, this set wins.
    std::vector<std::pair<std::string, std::string>> headers;
    // Fully-resolved base URL (scheme + host + optional port).  The path
    // portion (e.g. /v1/messages) is appended by the caller.
    std::string base_url;
    // Fully-resolved model string for this provider.
    std::string model_id;
    // Bedrock-only: full URL path (e.g. /model/arn:.../invoke-with-response-stream)
    // plus the request body bytes, so callers don't have to pass the body
    // through twice.  The path must match what's actually POSTed byte-for-byte.
    std::string bedrock_path;
    // Endpoint host (used by SigV4 "host" header + endpoint validation).
    std::string endpoint_host;
};

// ---------------------------------------------------------------------------
// Bedrock endpoint URL helpers.
// Path shape (matches Bedrock SDK):
//   /model/{model-id-or-arn}/invoke                      (non-streaming)
//   /model/{model-id-or-arn}/invoke-with-response-stream (streaming)
// Host shape (matches env_utils resolve_aws_region / env override):
//   https://bedrock-runtime.{region}.amazonaws.com
// Custom base_url (ANTHROPIC_BEDROCK_BASE_URL) overrides host.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::string bedrock_base_url(std::string_view region) {
    using cc::utils::env::get_env;
    if (auto u = get_env("ANTHROPIC_BEDROCK_BASE_URL"); u && !u->empty()) {
        auto r = *u;
        while (!r.empty() && r.back() == '/') r.pop_back();
        return r;
    }
    return std::string("https://bedrock-runtime.") + std::string(region) +
           ".amazonaws.com";
}

[[nodiscard]] inline std::string bedrock_path_for_model(
    std::string_view provider_model_id, bool streaming) {
    using namespace cc::utils::http;
    // The model identifier itself is URL-encoded for the path (it may contain
    // '/' characters in cross-region inference ARNs, so we encode it safely).
    return std::string("/model/") + uri_encode(std::string(provider_model_id)) +
           (streaming ? "/invoke-with-response-stream" : "/invoke");
}

// ---------------------------------------------------------------------------
// EnterpriseAuthContext — stateful cache + auth resolver.
// Intended to be owned by AnthropicClient (one per client instance).
// NOT thread-safe — QueryEngine / AnthropicClient already serialize the
// request loop via the streaming mutex.
// ---------------------------------------------------------------------------
class EnterpriseAuthContext {
public:
    EnterpriseAuthContext() : provider_(detect_active_provider()) {
        if (provider_ == EnterpriseProvider::Bedrock) {
            bedrock_mode_ = aws::detect_bedrock_mode();
            bedrock_region_ = aws::resolve_region();
        } else if (provider_ == EnterpriseProvider::Vertex) {
            vertex_mode_ = gcp::detect_vertex_mode();
            vertex_region_ = gcp::resolve_vertex_region();
            gcp_provider_ = std::make_unique<gcp::GcpAccessTokenProvider>();
        } else if (provider_ == EnterpriseProvider::Foundry) {
            foundry_mode_ = azure::detect_foundry_mode();
            azure_cred_ = std::make_unique<azure::DefaultAzureCredential>();
            foundry_base_url_ = azure::resolve_foundry_base_url(foundry_mode_);
        }
    }

    // Accessor: which provider was selected at construction time.
    [[nodiscard]] EnterpriseProvider provider() const noexcept { return provider_; }

    // Invalidate all caches (called from retry loop on 401 / 403 errors).
    void invalidate_all() {
        if (gcp_provider_) gcp_provider_->invalidate();
        if (azure_cred_) azure_cred_->invalidate();
        cached_aws_creds_ = std::nullopt;
    }

    // Resolve authentication for a single HTTP call.  Parameters:
    //   canonical_first_party_id — "claude-sonnet-4-20250514-v1:0" style
    //   method   — "GET" / "POST" (upper case).  All providers POST for messages.
    //   request_body_bytes — payload to send (used for SigV4 payload hash,
    //                         and stored back in ResolvedAuth.bedrock_path).
    //   streaming — true for invoke-with-response-stream, false for invoke.
    //   extra_agnostic_headers — headers already attached by the agnostic
    //          layer (User-Agent, x-app, session-id, anthropic-version).  These
    //          are re-passed through the signer so SigV4 covers them.
    [[nodiscard]] Result<ResolvedAuth> resolve_auth_for_request(
        std::string_view canonical_first_party_id,
        std::string_view method,
        std::string_view request_body_bytes,
        bool streaming,
        const std::vector<std::pair<std::string, std::string>>& extra_agnostic_headers) {
        ResolvedAuth out;
        out.provider = provider_;

        switch (provider_) {
        case EnterpriseProvider::FirstParty:
            return resolve_first_party(canonical_first_party_id, std::move(out));
        case EnterpriseProvider::Bedrock:
            return resolve_bedrock(canonical_first_party_id, method,
                                   request_body_bytes, streaming,
                                   extra_agnostic_headers, std::move(out));
        case EnterpriseProvider::Vertex:
            return resolve_vertex(canonical_first_party_id, std::move(out));
        case EnterpriseProvider::Foundry:
            return resolve_foundry(canonical_first_party_id, std::move(out));
        }
        return std::unexpected(Error(
            cc::utils::ErrorCode::internal_error,
            "Unknown enterprise provider"));
    }

    // Accessors for tests / logging.
    [[nodiscard]] std::string_view bedrock_region() const noexcept { return bedrock_region_; }
    [[nodiscard]] std::string_view vertex_region()  const noexcept { return vertex_region_; }
    [[nodiscard]] const std::string& foundry_base_url() const noexcept { return foundry_base_url_; }

private:
    EnterpriseProvider provider_;

    // Bedrock state.
    aws::BedrockAuthMode bedrock_mode_;
    std::string bedrock_region_;
    mutable std::optional<aws::AwsCredentials> cached_aws_creds_;

    // Vertex state.
    gcp::VertexAuthMode vertex_mode_;
    std::string vertex_region_;
    std::unique_ptr<gcp::GcpAccessTokenProvider> gcp_provider_;

    // Foundry state.
    azure::FoundryAuthMode foundry_mode_;
    std::string foundry_base_url_;
    std::unique_ptr<azure::DefaultAzureCredential> azure_cred_;

    // -----------------------------------------------------------------------
    // Per-provider resolution helpers.
    // -----------------------------------------------------------------------
    Result<ResolvedAuth> resolve_first_party(
        std::string_view canonical_id, ResolvedAuth out) {
        using cc::utils::env::get_env;
        using cc::utils::Provider;
        using cc::utils::get_api_base_url;
        out.base_url = get_api_base_url(Provider::Anthropic);
        out.model_id = std::string(canonical_id);
        out.endpoint_host = extract_host_from_url(out.base_url);
        return out;
    }

    Result<ResolvedAuth> resolve_bedrock(
        std::string_view canonical_id,
        std::string_view method,
        std::string_view body_bytes,
        bool streaming,
        const std::vector<std::pair<std::string, std::string>>& extra_headers,
        ResolvedAuth out) {
        out.base_url = bedrock_base_url(bedrock_region_);
        out.model_id = resolve_bedrock_model_id(canonical_id);
        out.bedrock_path = bedrock_path_for_model(out.model_id, streaming);
        out.endpoint_host = extract_host_from_url(out.base_url);

        // Bearer bypass (highest priority) — TS client.ts L175.
        if (bedrock_mode_.bearer_token) {
            out.headers.emplace_back("Authorization",
                                     "Bearer " + *bedrock_mode_.bearer_token);
            return out;
        }
        // Skip auth — proxy/no-op.
        if (bedrock_mode_.skip_auth) {
            return out;
        }
        // SigV4 path.  Resolve credentials with cache.
        if (!cached_aws_creds_ || !cached_aws_creds_->valid()) {
            cached_aws_creds_ = aws::resolve_default_credentials();
        }
        if (!cached_aws_creds_ || !cached_aws_creds_->valid()) {
            return std::unexpected(Error(
                cc::utils::ErrorCode::permission_denied,
                "AWS Bedrock: no valid credentials after the full chain. "
                "Set AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY, configure "
                "~/.aws/credentials, attach an IAM role to the EC2 instance, "
                "or set the `awsAuthRefresh` / `awsCredentialExport` setting."));
        }
        auto signed_r = aws::sign_request(
            method, out.endpoint_host, out.bedrock_path,
            /*query=*/{}, body_bytes, extra_headers,
            *cached_aws_creds_, bedrock_region_, "bedrock");
        if (!signed_r) return std::unexpected(signed_r.error());
        // Attach Authorization + all headers the signer canonicalized (host,
        // x-amz-date, x-amz-content-sha256, x-amz-security-token).
        out.headers.emplace_back("Authorization", signed_r->authorization);
        for (const auto& [k, v] : signed_r->headers) {
            // Skip host (it's set by the HTTP library from the URL, not as a
            // user-provided header — httplib will complain if we pass it).
            if (k == "host") continue;
            out.headers.emplace_back(k, v);
        }
        return out;
    }

    Result<ResolvedAuth> resolve_vertex(std::string_view canonical_id,
                                         ResolvedAuth out) {
        // Model string via Vertex lookup.
        out.model_id = resolve_vertex_model_id(canonical_id);
        // Resolve project_id: cached provider ->last_project_id, else env,
        // else fallback (detect_vertex_mode already set it for the 12s case).
        std::string project_id;
        if (gcp_provider_) {
            if (auto _ = gcp_provider_->last_project_id(); !_.empty()) project_id = _;
        }
        if (project_id.empty()) {
            using cc::utils::env::get_env;
            if (auto p = get_env("GOOGLE_CLOUD_PROJECT"); p && !p->empty()) project_id = *p;
            else if (auto p = get_env("GCLOUD_PROJECT"); p && !p->empty()) project_id = *p;
            else if (auto p = get_env("google_cloud_project"); p && !p->empty()) project_id = *p;
            else if (auto p = get_env("gcloud_project"); p && !p->empty()) project_id = *p;
            else if (vertex_mode_.fallback_project_id) project_id = *vertex_mode_.fallback_project_id;
        }
        if (project_id.empty()) {
            // Last-ditch: try fetching metadata token (fills project_id too).
            if (!vertex_mode_.skip_auth && gcp_provider_) {
                auto tok = gcp_provider_->get_token();
                if (tok) project_id = tok->project_id;
            }
        }
        if (project_id.empty()) {
            return std::unexpected(Error(
                cc::utils::ErrorCode::internal_error,
                "GCP Vertex: no project_id resolved.  Set GOOGLE_CLOUD_PROJECT, "
                "GCLOUD_PROJECT, or ANTHROPIC_VERTEX_PROJECT_ID, attach a GKE "
                "Workload Identity, or set `gcpAuthRefresh` in settings."));
        }
        out.base_url = gcp::make_vertex_base_url(vertex_region_, project_id);
        out.endpoint_host = extract_host_from_url(out.base_url);

        if (vertex_mode_.skip_auth) return out;

        auto tok = gcp_provider_->get_token();
        if (!tok) return std::unexpected(tok.error());

        out.headers.emplace_back("Authorization",
                                 "Bearer " + tok->token);
        if (tok->quota_from_adc && !tok->quota_project.empty()) {
            out.headers.emplace_back("x-goog-user-project", tok->quota_project);
        }
        return out;
    }

    Result<ResolvedAuth> resolve_foundry(std::string_view canonical_id,
                                          ResolvedAuth out) {
        using namespace azure;
        // Default deployment name + override via modelOverrides (handled by
        // caller — here we just return the default).
        std::string_view dep = lookup_default_deployment(canonical_id);
        if (dep.empty()) dep = canonical_id;
        out.model_id = std::string(dep);

        if (foundry_base_url_.empty()) {
            return std::unexpected(Error(
                cc::utils::ErrorCode::internal_error,
                "Azure Foundry: no endpoint.  Set ANTHROPIC_FOUNDRY_BASE_URL "
                "or ANTHROPIC_FOUNDRY_RESOURCE."));
        }
        out.base_url = foundry_base_url_;
        out.endpoint_host = extract_host_from_url(out.base_url);

        // API-key mode — short-circuit.
        if (foundry_mode_.api_key) {
            out.headers.emplace_back("Ocp-Apim-Subscription-Key",
                                     *foundry_mode_.api_key);
            return out;
        }
        if (foundry_mode_.skip_auth) return out;
        if (!azure_cred_) {
            return std::unexpected(Error(
                cc::utils::ErrorCode::internal_error,
                "Azure Foundry: credential provider not initialized."));
        }
        auto tok = azure_cred_->get_token(kFoundryDefaultScope);
        if (!tok) return std::unexpected(tok.error());
        out.headers.emplace_back("Authorization", "Bearer " + tok->token);
        return out;
    }

    // Helper: extract host (no scheme, no path) from a URL like
    // "https://bedrock-runtime.us-east-1.amazonaws.com" — returns the part
    // between "://" and the next '/' (if any).  Used for both endpoint_host
    // and sanity checks.
    static std::string extract_host_from_url(std::string_view url) {
        auto scheme_end = url.find("://");
        std::string_view remainder = (scheme_end != std::string_view::npos)
            ? url.substr(scheme_end + 3)
            : url;
        auto slash = remainder.find('/');
        auto host = remainder.substr(0, slash);
        // Strip optional :port.
        auto colon = host.rfind(':');
        if (colon != std::string_view::npos &&
            host.find(']') == std::string_view::npos) {
            host = host.substr(0, colon);
        }
        return std::string(host);
    }

    // --- Provider-specific default model ID tables.
    // These mirror TS ALL_MODEL_CONFIGS tables for Bedrock + Vertex.
    // Foundry handled in azure_credential module via lookup_default_deployment.
    // We keep them here (rather than in each auth module) because they are a
    // lookup property of the BYOC dispatch, not the auth protocol itself.
    static std::string resolve_vertex_model_id(std::string_view canonical) {
        static const std::pair<std::string_view, std::string_view> kTable[] = {
            {"claude-3-5-sonnet-20241022-v2:0", "claude-3-5-sonnet-v2@20241022"},
            {"claude-3-7-sonnet-20250219",       "claude-3-7-sonnet@20250219"},
            {"claude-sonnet-4-20250514-v1:0",    "claude-sonnet-4@20250514"},
            {"claude-sonnet-4-5-20250929-v1:0",  "claude-sonnet-4-5@20250929"},
            {"claude-sonnet-4-6",                "claude-sonnet-4-6"},
            {"claude-3-5-haiku-20241022-v1:0",   "claude-3-5-haiku@20241022"},
            {"claude-haiku-4-5-20251001-v1:0",   "claude-haiku-4-5@20251001"},
            {"claude-opus-4-20250514-v1:0",      "claude-opus-4@20250514"},
            {"claude-opus-4-1-20250805-v1:0",    "claude-opus-4-1@20250805"},
            {"claude-opus-4-5-20251101-v1:0",    "claude-opus-4-5@20251101"},
            {"claude-opus-4-6-v1",               "claude-opus-4-6"},
        };
        for (const auto& [k, v] : kTable) {
            if (canonical == k) return std::string(v);
            if (canonical.find(k) != std::string_view::npos) return std::string(v);
        }
        return std::string(canonical);
    }

    static std::string resolve_bedrock_model_id(std::string_view canonical) {
        static const std::pair<std::string_view, std::string_view> kTable[] = {
            {"claude-3-5-sonnet-20241022-v2:0", "anthropic.claude-3-5-sonnet-20241022-v2:0"},
            {"claude-3-7-sonnet-20250219",       "us.anthropic.claude-3-7-sonnet-20250219-v1:0"},
            {"claude-sonnet-4-20250514-v1:0",    "us.anthropic.claude-sonnet-4-20250514-v1:0"},
            {"claude-sonnet-4-5-20250929-v1:0",  "us.anthropic.claude-sonnet-4-5-20250929-v1:0"},
            {"claude-sonnet-4-6",                "us.anthropic.claude-sonnet-4-6"},
            {"claude-3-5-haiku-20241022-v1:0",   "us.anthropic.claude-3-5-haiku-20241022-v1:0"},
            {"claude-haiku-4-5-20251001-v1:0",   "us.anthropic.claude-haiku-4-5-20251001-v1:0"},
            {"claude-opus-4-20250514-v1:0",      "us.anthropic.claude-opus-4-20250514-v1:0"},
            {"claude-opus-4-1-20250805-v1:0",    "us.anthropic.claude-opus-4-1-20250805-v1:0"},
            {"claude-opus-4-5-20251101-v1:0",    "us.anthropic.claude-opus-4-5-20251101-v1:0"},
            {"claude-opus-4-6-v1",               "us.anthropic.claude-opus-4-6-v1"},
        };
        for (const auto& [k, v] : kTable) {
            if (canonical == k) return std::string(v);
            if (canonical.find(k) != std::string_view::npos) return std::string(v);
        }
        return std::string(canonical);
    }
};

} // namespace cc::services::auth::byoc
