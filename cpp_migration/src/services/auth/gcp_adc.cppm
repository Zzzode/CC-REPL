// GCP Vertex AI Application Default Credentials (ADC) implementation.
//
// Faithful to TS upstream's use of google-auth-library GoogleAuth with
// scope=https://www.googleapis.com/auth/cloud-platform.  The flow is:
//
//   1. GOOGLE_APPLICATION_CREDENTIALS env → path to service-account JSON.
//      For a service account, we build a self-signed JWT (RS256 using the
//      private key) and exchange it for an access_token via
//      https://oauth2.googleapis.com/token (grant_type=urn:ietf:params:oauth:
//      grant-type:jwt-bearer).  The resulting access_token is valid for
//      3600s and cached until ~5 min before expiry.
//      Alternatively, if the JSON payload has "type": "authorized_user",
//      we perform the standard refresh_token exchange.
//
//   2. gcloud ADC JSON at ~/.config/gcloud/application_default_credentials.json.
//      This file is created by `gcloud auth application-default login` and
//      has type=authorized_user with client_id / client_secret / refresh_token.
//      Exchange via https://oauth2.googleapis.com/token.
//
//   3. GKE Workload Identity — federated token via the
//      /computeMetadata/v1/instance/service-accounts/default/token endpoint
//      on the metadata server at http://metadata.google.internal (also
//      reachable at 169.254.169.254).  The Metadata-Flavor: Google header
//      MUST be present, and a short connection timeout guards against hangs
//      when running outside GCP.
//
// We intentionally do NOT pull in google-cloud-cpp.  This module implements
// the exact 3-step OAuth2 flow that google-auth-library performs internally,
// keeping the BYOC surface small and auditable.
module;
#include <chrono>
#include <cstdint>
#include <cstring>
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
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

export module cc.services.auth.gcp_adc;

import cc.utils.crypto;
import cc.utils.env;
import cc.utils.error;
import cc.utils.http_encoding;
import cc.utils.json;

export namespace cc::services::auth::gcp {

using cc::utils::Error;
using cc::utils::Result;
using namespace std::chrono;
using namespace std::string_view_literals;

// ---------------------------------------------------------------------------
// Access token cache entry.
// ---------------------------------------------------------------------------
struct AccessToken {
    std::string token;          // bearer token, attached as Authorization
    std::string token_type;     // "Bearer"
    system_clock::time_point expires_at;
    std::string scope;          // requested scope (stored for cache key)
    std::string project_id;     // from ADC JSON or metadata server
    std::string quota_project;  // from ADC JSON x-goog-user-project, optional
    bool quota_from_adc = false;

    [[nodiscard]] bool expired() const noexcept {
        return system_clock::now() >= expires_at - minutes(5);
    }
    [[nodiscard]] bool valid() const noexcept {
        return !token.empty() && !expired();
    }
};

// ---------------------------------------------------------------------------
// Parse a GCP service-account / authorized-user / external-account JSON.
// Only the two most common types are implemented:
//   * service_account — client_email + private_key + token_uri + project_id
//   * authorized_user  — client_id + client_secret + refresh_token (+ quota_project_id)
// Other types (external_account, impersonated_service_account, gdch_service_account)
// are returned as-is with a type string so callers can fall back to shell hooks.
// ---------------------------------------------------------------------------
enum class AdcType {
    Unknown,
    ServiceAccount,
    AuthorizedUser,
    ExternalAccount, // workload identity federation / workforce — not implemented
};
struct AdcCredentials {
    AdcType type = AdcType::Unknown;
    std::string project_id;
    std::string quota_project_id;
    // service_account fields:
    std::string client_email;
    std::string private_key_pem;  // full "-----BEGIN PRIVATE KEY-----\n...\n-----END PRIVATE KEY-----\n"
    std::string token_uri;        // default: "https://oauth2.googleapis.com/token"
    // authorized_user fields:
    std::string client_id;
    std::string client_secret;
    std::string refresh_token;
    // for reporting:
    std::string source_path;
};

[[nodiscard]] inline AdcCredentials parse_adc_json(std::string_view json_text,
                                                    std::string_view source_path) {
    using namespace cc::utils::json;
    AdcCredentials out;
    out.source_path = std::string(source_path);
    auto parsed = parse(json_text);
    if (!parsed) return out;
    auto root = parsed->root();
    auto t = root.get_string("type");
    if (t == "service_account") {
        out.type = AdcType::ServiceAccount;
        out.project_id = root.get_string("project_id");
        out.client_email = root.get_string("client_email");
        out.private_key_pem = root.get_string("private_key");
        auto token_uri = root.get_string("token_uri");
        out.token_uri = token_uri.empty()
            ? std::string("https://oauth2.googleapis.com/token")
            : token_uri;
    } else if (t == "authorized_user") {
        out.type = AdcType::AuthorizedUser;
        out.client_id = root.get_string("client_id");
        out.client_secret = root.get_string("client_secret");
        out.refresh_token = root.get_string("refresh_token");
        out.quota_project_id = root.get_string("quota_project_id");
    } else if (t == "external_account") {
        out.type = AdcType::ExternalAccount;
    }
    return out;
}

[[nodiscard]] inline std::optional<AdcCredentials>
load_adc_from_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::stringstream buf;
    buf << f.rdbuf();
    auto creds = parse_adc_json(buf.str(), path);
    if (creds.type == AdcType::Unknown) return std::nullopt;
    return creds;
}

// ---------------------------------------------------------------------------
// RS256 signer — used for the self-signed JWT in the service-account flow.
// Header: {"alg":"RS256","typ":"JWT"}
// Payload: {iss,scope,aud,iat,exp}
// Signature: RSA-SHA256 over base64url(header) + "." + base64url(payload)
// Final JWT: header_b64.payload_b64.signature_b64
// ---------------------------------------------------------------------------
namespace detail {
// RAII wrapper around OpenSSL EVP_PKEY loaded from PEM text.
struct PemKeyDeleter {
    void operator()(EVP_PKEY* p) const noexcept { if (p) EVP_PKEY_free(p); }
};
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, PemKeyDeleter>;

[[nodiscard]] inline EvpPkeyPtr load_private_key_pem(std::string_view pem) {
    if (pem.empty()) return nullptr;
    // PEM_read_bio_PrivateKey takes a BIO wrapping the PEM text.
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return nullptr;
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return EvpPkeyPtr(key);
}

// Returns base64url (no padding) of a raw byte span.  Reuses crypto base64
// encoder by encoding then swapping +→- /→_ and trimming padding '='.
[[nodiscard]] inline std::string b64url_nopad(std::span<const uint8_t> bytes) {
    auto b64 = cc::utils::crypto::base64_encode(bytes);
    std::string out;
    out.reserve(b64.size());
    for (char c : b64) {
        if (c == '+') out.push_back('-');
        else if (c == '/') out.push_back('_');
        else if (c == '=') break;
        else out.push_back(c);
    }
    return out;
}
[[nodiscard]] inline std::string b64url_nopad(std::string_view s) {
    return b64url_nopad(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

// Sign data with RSA-SHA256.  Returns raw signature bytes or error.
[[nodiscard]] inline Result<std::vector<uint8_t>> rsa_sha256_sign(
    EVP_PKEY* key, std::string_view data) {
    if (!key) {
        return std::unexpected(Error(cc::utils::ErrorCode::invalid_argument,
                                     "GCP RSA: no private key loaded"));
    }
    std::unique_ptr<EVP_MD_CTX, void(*)(EVP_MD_CTX*)> ctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) {
        return std::unexpected(Error(cc::utils::ErrorCode::internal_error,
                                     "GCP RSA: EVP_MD_CTX_new failed"));
    }
    if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key) != 1) {
        return std::unexpected(Error(cc::utils::ErrorCode::internal_error,
                                     "GCP RSA: DigestSignInit failed"));
    }
    if (EVP_DigestSignUpdate(ctx.get(), data.data(), data.size()) != 1) {
        return std::unexpected(Error(cc::utils::ErrorCode::internal_error,
                                     "GCP RSA: DigestSignUpdate failed"));
    }
    std::size_t sig_len = 0;
    if (EVP_DigestSignFinal(ctx.get(), nullptr, &sig_len) != 1) {
        return std::unexpected(Error(cc::utils::ErrorCode::internal_error,
                                     "GCP RSA: DigestSignFinal size failed"));
    }
    std::vector<uint8_t> sig(sig_len);
    if (EVP_DigestSignFinal(ctx.get(), sig.data(), &sig_len) != 1) {
        return std::unexpected(Error(cc::utils::ErrorCode::internal_error,
                                     "GCP RSA: DigestSignFinal failed"));
    }
    sig.resize(sig_len);
    return sig;
}
} // namespace detail

// Build a service-account self-signed JWT and exchange it for an access token
// via the OAuth2 token endpoint.  Scope default is cloud-platform (matches TS).
[[nodiscard]] inline Result<AccessToken>
exchange_service_account_token(
    const AdcCredentials& creds,
    std::string_view scope = "https://www.googleapis.com/auth/cloud-platform") {
    using namespace cc::utils::json;
    using namespace cc::utils::http;
    if (creds.type != AdcType::ServiceAccount) {
        return std::unexpected(Error(cc::utils::ErrorCode::invalid_argument,
                                     "GCP: not a service_account ADC"));
    }
    auto key = detail::load_private_key_pem(creds.private_key_pem);
    if (!key) {
        return std::unexpected(Error(cc::utils::ErrorCode::invalid_argument,
            "GCP: failed to load service-account private key PEM"));
    }
    // Build JWT header + payload using JsonObject.
    using namespace cc::utils::json;
    JsonObject hdr_obj;
    hdr_obj.set("alg", "RS256");
    hdr_obj.set("typ", "JWT");
    JsonObject payload_obj;
    payload_obj.set("iss", creds.client_email);
    payload_obj.set("scope", std::string(scope));
    payload_obj.set("aud",
        creds.token_uri.empty() ? std::string("https://oauth2.googleapis.com/token")
                                : creds.token_uri);
    auto now = system_clock::now();
    auto exp = now + hours(1);
    std::time_t iat_t = system_clock::to_time_t(now);
    std::time_t exp_t = system_clock::to_time_t(exp);
    payload_obj.set("iat", static_cast<int64_t>(iat_t));
    payload_obj.set("exp", static_cast<int64_t>(exp_t));
    std::string signing_input = detail::b64url_nopad(hdr_obj.serialize()) + "." +
                                 detail::b64url_nopad(payload_obj.serialize());
    auto sig = detail::rsa_sha256_sign(key.get(), signing_input);
    if (!sig) return std::unexpected(sig.error());
    std::string jwt = signing_input + "." + detail::b64url_nopad(*sig);

    // Exchange: POST token_uri with form-urlencoded body.
    std::string body_str;
    body_str += "grant_type=";
    body_str += form_encode("urn:ietf:params:oauth:grant-type:jwt-bearer");
    body_str += "&assertion=";
    body_str += form_encode(jwt);

    httplib::Client cli("https://oauth2.googleapis.com");
    cli.enable_server_certificate_verification(true);
    httplib::Headers hdrs;
    hdrs.emplace("Content-Type", "application/x-www-form-urlencoded");
    auto resp = cli.Post("/token", hdrs, body_str, "application/x-www-form-urlencoded");
    if (!resp) {
        return std::unexpected(Error(cc::utils::ErrorCode::network_error,
            "GCP: no response from oauth2.googleapis.com/token"));
    }
    if (resp->status < 200 || resp->status >= 300) {
        return std::unexpected(Error(cc::utils::ErrorCode::permission_denied,
            "GCP: service-account token exchange failed (HTTP " +
                std::to_string(resp->status) + "): " + resp->body));
    }
    auto parsed = parse(resp->body);
    if (!parsed) {
        return std::unexpected(Error(cc::utils::ErrorCode::parse_error,
                                     "GCP: invalid token response JSON"));
    }
    auto root = parsed->root();
    AccessToken at;
    at.token = root.get_string("access_token");
    at.token_type = root.get_string("token_type");
    if (at.token_type.empty()) at.token_type = "Bearer";
    int64_t expires_in = root.get_int("expires_in");
    if (expires_in <= 0) expires_in = 3600;
    at.expires_at = system_clock::now() + seconds(expires_in);
    at.scope = std::string(scope);
    at.project_id = creds.project_id;
    if (at.token.empty()) {
        return std::unexpected(Error(cc::utils::ErrorCode::parse_error,
                                     "GCP: token response missing access_token"));
    }
    return at;
}

// authorized_user refresh-token exchange.
[[nodiscard]] inline Result<AccessToken>
exchange_authorized_user_token(const AdcCredentials& creds) {
    using namespace cc::utils::json;
    using namespace cc::utils::http;
    if (creds.type != AdcType::AuthorizedUser) {
        return std::unexpected(Error(cc::utils::ErrorCode::invalid_argument,
                                     "GCP: not an authorized_user ADC"));
    }
    std::string body_str;
    body_str  = "client_id="     + form_encode(creds.client_id);
    body_str += "&client_secret=" + form_encode(creds.client_secret);
    body_str += "&refresh_token=" + form_encode(creds.refresh_token);
    body_str += "&grant_type=refresh_token";

    httplib::Client cli("https://oauth2.googleapis.com");
    cli.enable_server_certificate_verification(true);
    auto resp = cli.Post("/token", body_str,
                         "application/x-www-form-urlencoded");
    if (!resp) {
        return std::unexpected(Error(cc::utils::ErrorCode::network_error,
            "GCP: no response from oauth2.googleapis.com/token (authorized_user)"));
    }
    if (resp->status < 200 || resp->status >= 300) {
        return std::unexpected(Error(cc::utils::ErrorCode::permission_denied,
            "GCP: authorized_user token exchange failed (HTTP " +
                std::to_string(resp->status) + "): " + resp->body));
    }
    auto parsed = parse(resp->body);
    if (!parsed) {
        return std::unexpected(Error(cc::utils::ErrorCode::parse_error,
                                     "GCP: invalid authorized_user JSON"));
    }
    auto root = parsed->root();
    AccessToken at;
    at.token = root.get_string("access_token");
    at.token_type = root.get_string("token_type");
    if (at.token_type.empty()) at.token_type = "Bearer";
    int64_t expires_in = root.get_int("expires_in");
    if (expires_in <= 0) expires_in = 3600;
    at.expires_at = system_clock::now() + seconds(expires_in);
    at.scope = root.get_string("scope");
    at.quota_project = creds.quota_project_id;
    at.quota_from_adc = !at.quota_project.empty();
    if (at.token.empty()) {
        return std::unexpected(Error(cc::utils::ErrorCode::parse_error,
                                     "GCP: authorized_user response missing access_token"));
    }
    return at;
}

// Metadata-server (GCE / GKE) token retrieval.  Same short-timeout guards as
// SigV4 IMDS — 1.5s per HTTP step — so we don't hang on laptops.
[[nodiscard]] inline Result<AccessToken>
fetch_metadata_token(std::chrono::milliseconds timeout_ms = 1500ms,
                     std::string_view sa_name = "default",
                     std::string_view scope = "https://www.googleapis.com/auth/cloud-platform") {
    using namespace cc::utils::json;
    httplib::Client cli("http://metadata.google.internal", 80);
    cli.set_connection_timeout(timeout_ms.count() / 1000,
                               (timeout_ms.count() % 1000) * 1000);
    cli.set_read_timeout(timeout_ms.count() / 1000,
                         (timeout_ms.count() % 1000) * 1000);
    httplib::Headers hdrs;
    hdrs.emplace("Metadata-Flavor", "Google");
    std::string url = "/computeMetadata/v1/instance/service-accounts/" +
                      std::string(sa_name) + "/token?scopes=" +
                      std::string(scope);
    auto resp = cli.Get(url.c_str(), hdrs);
    if (!resp) {
        return std::unexpected(Error(cc::utils::ErrorCode::network_error,
                                 "GCP: metadata server unreachable"));
    }
    if (resp->status == 404) {
        return std::unexpected(Error(cc::utils::ErrorCode::internal_error,
                                 "GCP: no service account attached"));
    }
    if (resp->status < 200 || resp->status >= 300) {
        return std::unexpected(Error(cc::utils::ErrorCode::permission_denied,
            "GCP: metadata token fetch HTTP " + std::to_string(resp->status)));
    }
    auto parsed = parse(resp->body);
    if (!parsed) {
        return std::unexpected(Error(cc::utils::ErrorCode::parse_error,
                                 "GCP: metadata response JSON parse failed"));
    }
    auto root = parsed->root();
    AccessToken at;
    at.token = root.get_string("access_token");
    at.token_type = root.get_string("token_type");
    if (at.token_type.empty()) at.token_type = "Bearer";
    int64_t expires_in = root.get_int("expires_in");
    if (expires_in <= 0) expires_in = 1800;
    at.expires_at = system_clock::now() + seconds(expires_in);
    at.scope = std::string(scope);
    // Try to fetch project ID from metadata server as well (best-effort).
    auto proj_r = cli.Get(
        "/computeMetadata/v1/project/project-id", hdrs);
    if (proj_r && proj_r->status == 200 && !proj_r->body.empty()) {
        at.project_id = proj_r->body;
        // strip trailing newline
        while (!at.project_id.empty() &&
               (at.project_id.back() == '\n' || at.project_id.back() == '\r')) {
            at.project_id.pop_back();
        }
    }
    if (at.token.empty()) {
        return std::unexpected(Error(cc::utils::ErrorCode::parse_error,
                                 "GCP: metadata response missing access_token"));
    }
    return at;
}

// ---------------------------------------------------------------------------
// High-level cache + full ADC chain.  NOT thread-safe — callers serialize.
// ---------------------------------------------------------------------------
class GcpAccessTokenProvider {
public:
    // Runs the full ADC chain once.  On success, caches until 5 min before
    // expiry and returns the cached token on subsequent calls.  On failure,
    // returns the error so the query engine can surface it to the user.
    Result<AccessToken> get_token(
        std::string_view scope = "https://www.googleapis.com/auth/cloud-platform") {
        std::string scope_str(scope);
        // Cache hit?
        if (cached_.valid() && cached_.scope == scope_str) {
            return cached_;
        }
        // Step 1: GOOGLE_APPLICATION_CREDENTIALS file.
        using cc::utils::env::get_env;
        std::optional<AdcCredentials> adc;
        if (auto f = get_env("GOOGLE_APPLICATION_CREDENTIALS"); f && !f->empty()) {
            adc = load_adc_from_file(*f);
        }
        // Step 2: well-known gcloud ADC path.
        if (!adc) {
            const char* home_env = std::getenv("HOME");
            std::string home = home_env ? home_env : "";
            if (!home.empty()) {
                adc = load_adc_from_file(home +
                    "/.config/gcloud/application_default_credentials.json");
            }
        }

        if (adc) {
            Result<AccessToken> tok = std::unexpected(
                Error(cc::utils::ErrorCode::internal_error,
                      "GCP ADC chain: no token resolved"));
            switch (adc->type) {
            case AdcType::ServiceAccount:
                tok = exchange_service_account_token(*adc, scope);
                break;
            case AdcType::AuthorizedUser:
                tok = exchange_authorized_user_token(*adc);
                if (tok && tok->scope.empty()) tok->scope = scope_str;
                break;
            default:
                return std::unexpected(Error(
                    cc::utils::ErrorCode::unimplemented,
                    "GCP: unsupported ADC type '" + std::string(
                        adc->type == AdcType::ExternalAccount ? "external_account" : "unknown") +
                    "' — see 'gcpAuthRefresh' setting for custom shell refresh scripts."));
            }
            if (tok) {
                cached_ = std::move(*tok);
                cached_.scope = scope_str;
                return cached_;
            }
            return tok;
        }
        // Step 3: metadata server.
        auto tok = fetch_metadata_token(std::chrono::milliseconds(1500), "default", scope);
        if (tok) {
            cached_ = std::move(*tok);
            return cached_;
        }
        return tok;
    }

    void invalidate() { cached_ = AccessToken{}; }

    // Convenience: return the most recently resolved project_id, if any.
    [[nodiscard]] const std::string& last_project_id() const noexcept {
        return cached_.project_id;
    }

private:
    AccessToken cached_;
};

// ---------------------------------------------------------------------------
// TS provider-detection env-var helpers for Vertex.
// ---------------------------------------------------------------------------
struct VertexAuthMode {
    bool use_vertex = false;
    bool skip_auth = false;
    // If user sets ANTHROPIC_VERTEX_PROJECT_ID explicitly AND no standard
    // GCP project discovery method is present, this should be used as the
    // last-resort fallback (avoids 12s IMDS timeout on laptops).
    std::optional<std::string> fallback_project_id;
};
[[nodiscard]] inline VertexAuthMode detect_vertex_mode() {
    using cc::utils::env::get_env;
    using cc::utils::env::is_env_truthy;
    VertexAuthMode m;
    m.use_vertex  = is_env_truthy("CLAUDE_CODE_USE_VERTEX");
    m.skip_auth   = is_env_truthy("CLAUDE_CODE_SKIP_VERTEX_AUTH");
    // hasProjectEnvVar || hasKeyFile → don't set fallback_project_id.
    const bool has_project_env =
        (get_env("GCLOUD_PROJECT") && !get_env("GCLOUD_PROJECT")->empty()) ||
        (get_env("GOOGLE_CLOUD_PROJECT") && !get_env("GOOGLE_CLOUD_PROJECT")->empty()) ||
        (get_env("gcloud_project") && !get_env("gcloud_project")->empty()) ||
        (get_env("google_cloud_project") && !get_env("google_cloud_project")->empty());
    const bool has_key_file =
        (get_env("GOOGLE_APPLICATION_CREDENTIALS") &&
         !get_env("GOOGLE_APPLICATION_CREDENTIALS")->empty()) ||
        (get_env("google_application_credentials") &&
         !get_env("google_application_credentials")->empty());
    if (!has_project_env && !has_key_file) {
        if (auto pid = get_env("ANTHROPIC_VERTEX_PROJECT_ID");
            pid && !pid->empty()) {
            m.fallback_project_id = std::move(*pid);
        }
    }
    return m;
}

// Vertex region resolution.  Per TS envUtils::getVertexRegionForModel:
//   1. VERTEX_REGION_CLAUDE_<model> per-model override env (12 vars).
//   2. CLOUD_ML_REGION global.
//   3. default: "us-east5".
// We don't have a per-model registry here, so accept a model_id_hint string;
// callers that know the model short key can pass the canonical firstPartyId.
[[nodiscard]] inline std::string resolve_vertex_region(
    std::string_view model_id_hint = {}) {
    using cc::utils::env::get_env;
    // Build known env var names per TS VERTEX_REGION_OVERRIDES.
    static const std::pair<std::string_view, std::string_view> kOverrides[] = {
        {"claude-3-5-sonnet", "VERTEX_REGION_CLAUDE_3_5_SONNET"},
        {"claude-3-7-sonnet", "VERTEX_REGION_CLAUDE_3_7_SONNET"},
        {"claude-sonnet-4",   "VERTEX_REGION_CLAUDE_4_0_SONNET"},
        {"claude-sonnet-4-5", "VERTEX_REGION_CLAUDE_4_5_SONNET"},
        {"claude-sonnet-4-6", "VERTEX_REGION_CLAUDE_4_6_SONNET"},
        {"claude-3-5-haiku",  "VERTEX_REGION_CLAUDE_3_5_HAIKU"},
        {"claude-haiku-4-5",  "VERTEX_REGION_CLAUDE_HAIKU_4_5"},
        {"claude-opus-4",     "VERTEX_REGION_CLAUDE_OPUS_4"},
        {"claude-opus-4-1",   "VERTEX_REGION_CLAUDE_OPUS_4_1"},
        {"claude-opus-4-5",   "VERTEX_REGION_CLAUDE_OPUS_4_5"},
        {"claude-opus-4-6",   "VERTEX_REGION_CLAUDE_OPUS_4_6"},
    };
    for (const auto& [prefix, env_var] : kOverrides) {
        if (model_id_hint.find(prefix) != std::string_view::npos) {
            if (auto r = get_env(std::string(env_var)); r && !r->empty()) {
                return *r;
            }
        }
    }
    if (auto r = get_env("CLOUD_ML_REGION"); r && !r->empty()) return *r;
    return "us-east5";
}

// Build the Vertex REST endpoint prefix.
// Format: https://{REGION}-aiplatform.googleapis.com/v1/projects/{PROJECT}/locations/{REGION}
[[nodiscard]] inline std::string make_vertex_base_url(std::string_view region,
                                                       std::string_view project_id) {
    std::string base;
    base.reserve(64 + region.size() * 2 + project_id.size());
    base += "https://";
    base += region;
    base += "-aiplatform.googleapis.com/v1/projects/";
    base += project_id;
    base += "/locations/";
    base += region;
    return base;
}

} // namespace cc::services::auth::gcp
