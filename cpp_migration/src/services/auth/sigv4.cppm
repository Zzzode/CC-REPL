// AWS Signature Version 4 (SigV4) self-contained signer + credential chain.
//
// Faithful to the TS upstream's use of @smithy/signature-v4 via the Bedrock
// SDK.  Service name for Bedrock Runtime is "bedrock".  Signing algorithm is
// AWS4-HMAC-SHA256.  All rules (canonical request construction, string to
// sign, signature derivation, signed header ordering) match the AWS SigV4
// spec exactly so requests are byte-for-byte identical to what AWS SDKs
// produce — essential because AWS enforces strict byte-level signature
// validation.
//
// Credential chain (same priority as TS upstream):
//   1. Static credentials from caller (handed via `AwsCredentials` struct).
//   2. Environment variables AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY /
//      AWS_SESSION_TOKEN.
//   3. Shared INI files (~/.aws/credentials + ~/.aws/config) with optional
//      AWS_PROFILE selection.  Only basic key/secret/token lines are parsed;
//      role_arn / source_profile / sso_session are NOT supported here (those
//      require STS calls, which are part of the shell hooks handled by the
//      query engine's refresh pipeline, not this signer).
//   4. IMDSv2 (EC2 Instance Metadata Service) via PUT /latest/api/token then
//      GET /latest/meta-data/iam/security-credentials/<role>.  TTL 6h.
//
// Provider call sites (AnthropicClient / QueryEngine) are responsible for
// calling `refresh_aws_credentials()` (the shell hooks equivalent of TS
// `refreshAndGetAwsCredentials()`) before signing, if user scripts need to
// run first.  This module does NOT spawn user shells.
module;
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <expected>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <httplib.h>

export module cc.services.auth.sigv4;

import cc.utils.crypto;
import cc.utils.env;
import cc.utils.error;
import cc.utils.http_encoding;
import cc.utils.json;

export namespace cc::services::auth::aws {

using cc::utils::Error;
using cc::utils::Result;
using namespace std::chrono;
using namespace std::string_view_literals;
namespace http = cc::utils::http;

// ---------------------------------------------------------------------------
// Credentials
// ---------------------------------------------------------------------------
struct AwsCredentials {
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token; // optional, non-empty = assumed-role / STS

    [[nodiscard]] bool valid() const noexcept {
        return !access_key_id.empty() && !secret_access_key.empty();
    }
};

// Data class returned by IMDSv2 GetRole call — includes expiration so the
// caller can invalidate the cache.
struct IamRoleCredentials : AwsCredentials {
    system_clock::time_point expiration;
    std::string code; // "Success" on valid response
    std::string type; // "AWS-HMAC"
    std::string role_arn;
    [[nodiscard]] bool expired() const noexcept {
        return system_clock::now() >= expiration - minutes(5);
    }
};

// ---------------------------------------------------------------------------
// Time helpers — SigV4 uses yyyyMMddTHHmmssZ (long) + yyyyMMdd (short) in
// UTC, WITHOUT dashes/colons.
// ---------------------------------------------------------------------------
struct SigV4Timestamps {
    std::string ymd_hms; // e.g. 20250115T134530Z
    std::string ymd;     // e.g. 20250115
};

[[nodiscard]] inline SigV4Timestamps make_timestamps(
    system_clock::time_point tp = system_clock::now()) {
    auto tt = system_clock::to_time_t(tp);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &tt);
#else
    gmtime_r(&tt, &utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y%m%dT%H%M%SZ");
    std::string ymd_hms = oss.str();
    std::string ymd = ymd_hms.substr(0, 8);
    return {std::move(ymd_hms), std::move(ymd)};
}

// ---------------------------------------------------------------------------
// Canonical request helpers.
// AWS mandates:
//   * Headers lowercased, sorted by lowercase name.
//   * Multi-value headers joined with ','.
//   * signed_headers list (semicolon-separated lowercase names, sorted).
//   * Body: payload SHA-256 hex (or UNSIGNED-PAYLOAD literal for streaming,
//     though Bedrock sends a complete body so we always hash real payload).
//   * Query params: URI-encoded key=value pairs sorted by encoded KEY then
//     value, joined with '&'; empty query -> empty string, NOT '?'.
// ---------------------------------------------------------------------------
using HeaderMap = std::map<std::string, std::string>;
using QueryParams = std::vector<std::pair<std::string, std::string>>;

// Lowercase ASCII header names.  SigV4 only operates on ASCII headers.
[[nodiscard]] inline std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return out;
}

// Normalize header values per spec: strip leading/trailing whitespace,
// collapse interior runs of whitespace to a single space.  Quote-aware so
// quoted strings (ETag, Content-Disposition) preserve internal spaces.
[[nodiscard]] inline std::string normalize_header_value(std::string_view v) {
    std::string out;
    out.reserve(v.size());
    bool in_quote = false;
    bool in_ws = false;
    for (std::size_t i = 0; i < v.size(); ++i) {
        char c = v[i];
        if (c == '"') in_quote = !in_quote;
        if (!in_quote && (c == ' ' || c == '\t')) {
            in_ws = true;
            continue;
        }
        if (in_ws && !out.empty()) out.push_back(' ');
        in_ws = false;
        out.push_back(c);
    }
    return out;
}

// Take a header multimap (name → value) and collapse into lowercased,
// sorted std::map with comma-joined multi-values.
[[nodiscard]] inline HeaderMap canonicalize_headers(
    const std::vector<std::pair<std::string, std::string>>& raw_headers) {
    std::map<std::string, std::vector<std::string>> acc;
    for (const auto& [name, value] : raw_headers) {
        acc[lower_ascii(name)].push_back(normalize_header_value(value));
    }
    HeaderMap out;
    for (auto& [k, vs] : acc) {
        if (vs.size() == 1) {
            out[k] = std::move(vs.front());
        } else {
            std::string joined;
            for (std::size_t i = 0; i < vs.size(); ++i) {
                if (i) joined.push_back(',');
                joined += vs[i];
            }
            out[k] = std::move(joined);
        }
    }
    return out;
}

// The host header is mandatory for SigV4.  So is x-amz-date.  Any request
// passing through this signer MUST have both already added before signing.
// (Helper that appends them before canonicalization is provided below.)
[[nodiscard]] inline std::string build_signed_headers_list(
    const HeaderMap& m,
    std::initializer_list<std::string_view> extra_required = {}) {
    std::string out;
    auto append_name = [&](std::string_view lc_name) {
        if (!out.empty()) out.push_back(';');
        out += lc_name;
    };
    for (const auto& [k, _] : m) append_name(k);
    // extra_required names that are already in m are skipped (already added).
    for (std::string_view n : extra_required) {
        if (m.find(std::string(n)) == m.end()) append_name(n);
    }
    return out;
}

// Canonical query string.  Input is unsorted key/value pairs (already decoded
// or raw — callers are expected to pass raw query fragments).  We sort by
// encoded key then encoded value, which AWS requires.
[[nodiscard]] inline std::string canonical_query_string(QueryParams params) {
    if (params.empty()) return {};
    // Encode first, then sort lexicographically (stable: encoded form is
    // what AWS compares).
    std::vector<std::pair<std::string, std::string>> enc;
    enc.reserve(params.size());
    for (auto& [k, v] : params) {
        enc.emplace_back(http::uri_encode(k), http::uri_encode(v));
    }
    std::sort(enc.begin(), enc.end());
    std::string out;
    for (std::size_t i = 0; i < enc.size(); ++i) {
        if (i) out.push_back('&');
        out += enc[i].first;
        out.push_back('=');
        out += enc[i].second;
    }
    return out;
}

// Build the canonical request string per AWS spec.
[[nodiscard]] inline std::string canonical_request(
    std::string_view method,
    std::string_view uri_path,
    std::string_view canonical_query,
    const HeaderMap& canonical_headers,
    std::string_view signed_headers_list,
    std::string_view payload_sha256_hex) {
    std::string out;
    // Line 1: METHOD
    out += method;
    out.push_back('\n');
    // Line 2: URI-encoded canonical path (keep '/' separators unencoded)
    out += http::uri_encode_path(uri_path);
    out.push_back('\n');
    // Line 3: canonical query string (empty string allowed — line stays)
    out += canonical_query;
    out.push_back('\n');
    // Lines 4..N: each header name:value\n (already sorted + lowercased)
    for (const auto& [k, v] : canonical_headers) {
        out += k;
        out.push_back(':');
        out += v;
        out.push_back('\n');
    }
    out.push_back('\n'); // blank line separating headers block from signed-list
    // Signed headers list
    out += signed_headers_list;
    out.push_back('\n');
    // Payload hex
    out += payload_sha256_hex;
    return out;
}

// ---------------------------------------------------------------------------
// Signature derivation.  Key schedule:
//   kDate    = HMAC("AWS4" + secret,             date_ymd)
//   kRegion  = HMAC(kDate,                       region)
//   kService = HMAC(kRegion,                     service)
//   kSigning = HMAC(kService,                    "aws4_request")
//   signature = HEX(HMAC(kSigning, string_to_sign))
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::array<uint8_t, 32> derive_signing_key(
    std::string_view secret_access_key,
    std::string_view date_ymd,
    std::string_view region,
    std::string_view service) {
    using cc::utils::crypto::hmac_sha256;
    std::string k_secret = std::string("AWS4") + std::string(secret_access_key);
    auto k_date = hmac_sha256(
        std::string_view(k_secret).substr(0, k_secret.size()),
        date_ymd);
    auto k_region = hmac_sha256(
        std::span<const uint8_t>(k_date.data(), k_date.size()),
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(region.data()), region.size()));
    auto k_service = hmac_sha256(
        std::span<const uint8_t>(k_region.data(), k_region.size()),
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(service.data()), service.size()));
    static constexpr std::string_view k_aws4_request = "aws4_request";
    return hmac_sha256(
        std::span<const uint8_t>(k_service.data(), k_service.size()),
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(k_aws4_request.data()), k_aws4_request.size()));
}

// Convenience: build the HMAC key-inputs as string_view views over the
// byte arrays returned from the prior HMAC.
[[nodiscard]] inline std::array<uint8_t, 32> hmac_bytes(
    const std::array<uint8_t, 32>& key_bytes,
    std::string_view msg_chars) {
    return cc::utils::crypto::hmac_sha256(
        std::span<const uint8_t>(key_bytes.data(), key_bytes.size()),
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(msg_chars.data()),
            msg_chars.size()));
}

// String to sign (AWS SigV4 "Credential Scope" form):
//   "AWS4-HMAC-SHA256\n" + ymd_hms + "\n" + ymd/region/service/aws4_request
//   + "\n" + SHA256_HEX(canonical_request)
[[nodiscard]] inline std::string string_to_sign(
    std::string_view ymd_hms,
    std::string_view ymd,
    std::string_view region,
    std::string_view service,
    std::string_view canonical_request_str) {
    using cc::utils::crypto::sha256;
    std::string scope;
    scope.reserve(ymd.size() + 1 + region.size() + 1 + service.size() +
                  1 + 12);
    scope += ymd;
    scope.push_back('/');
    scope += region;
    scope.push_back('/');
    scope += service;
    scope += "/aws4_request";

    std::string sts;
    sts.reserve(32 + ymd_hms.size() + scope.size() + 64 + 4);
    sts += "AWS4-HMAC-SHA256\n";
    sts += ymd_hms;
    sts.push_back('\n');
    sts += scope;
    sts.push_back('\n');
    sts += sha256(canonical_request_str);
    return sts;
}

// ---------------------------------------------------------------------------
// Public signer entry point.  Returns the Authorization header value and the
// list of signed headers (so callers can sanity-check that host/date are
// included).  Also optionally appends x-amz-security-token when creds have a
// session token.
// ---------------------------------------------------------------------------
struct SignedRequest {
    // Authorization header value (no "Authorization:" prefix).
    std::string authorization;
    // Headers that MUST be included verbatim with the HTTP request — includes
    // at minimum host, x-amz-date, content-type (if set), x-amz-content-sha256,
    // x-amz-security-token (if session token is present), and any user-supplied
    // headers the caller passed in.
    HeaderMap headers;
    // Semicolon-separated lowercased header names, for debugging.
    std::string signed_headers;
    // ymd_hms used, for debugging / logging.
    std::string request_date;
};

// Required inputs:
//   method — "GET", "POST", etc. (UPPERCASE; AWS rejects anything else)
//   endpoint_host — "bedrock-runtime.us-east-1.amazonaws.com" (NO scheme, NO port defaulted when scheme standard)
//   uri_path — "/model/anthropic.claude-sonnet-4-20250514-v1:0/invoke"
//   query — sorted later; callers can pass raw unsorted key/val pairs
//   body_bytes — POST payload (will be SHA-256 hashed; hex is BOTH added to
//                x-amz-content-sha256 AND used as payload line in canonical req)
//   extra_headers — caller headers (User-Agent, Content-Type, accept etc.)
//                   in ORIGINAL case.  They are lowercased internally.
//   creds — access/secret/session (session optional)
//   region — "us-east-1", "eu-west-1" etc.
//   service — "bedrock" for Bedrock Runtime, "sts" for STS, etc.
//   tp — optional override timestamp (for tests)
[[nodiscard]] inline Result<SignedRequest> sign_request(
    std::string_view method,
    std::string_view endpoint_host,
    std::string_view uri_path,
    const QueryParams& query,
    std::string_view body_bytes,
    const std::vector<std::pair<std::string, std::string>>& extra_headers,
    const AwsCredentials& creds,
    std::string_view region,
    std::string_view service,
    std::optional<system_clock::time_point> tp = std::nullopt) {
    using cc::utils::crypto::sha256;
    using cc::utils::crypto::sha256_bytes_to_hex;

    if (!creds.valid()) {
        return std::unexpected(Error(
            cc::utils::ErrorCode::invalid_argument,
            "AWS SigV4: missing AccessKeyId / SecretAccessKey"));
    }
    auto ts = make_timestamps(tp.value_or(system_clock::now()));

    // Build the FULL header set that will be canonicalized.  Start with
    // user-provided headers (copied verbatim into a vector of pairs), then
    // append the required AWS headers.
    std::vector<std::pair<std::string, std::string>> all_headers = extra_headers;
    // host (lowercased host portion of URL)
    all_headers.emplace_back("host", std::string(endpoint_host));
    // x-amz-date
    all_headers.emplace_back("x-amz-date", ts.ymd_hms);
    // x-amz-content-sha256
    const std::string payload_hex = sha256(body_bytes);
    all_headers.emplace_back("x-amz-content-sha256", payload_hex);
    // x-amz-security-token (if STS session)
    if (!creds.session_token.empty()) {
        all_headers.emplace_back("x-amz-security-token", creds.session_token);
    }

    auto canon_hdrs = canonicalize_headers(all_headers);
    // NOTE: canonical_headers *must* include host/date before
    // build_signed_headers_list, or we'd add them twice.  The way we built
    // them above guarantees they are present.
    std::string signed_list;
    {
        bool first = true;
        for (const auto& [k, _] : canon_hdrs) {
            if (!first) signed_list.push_back(';');
            first = false;
            signed_list += k;
        }
    }

    std::string canon_q = canonical_query_string(query);
    std::string canon_req = canonical_request(
        method, uri_path, canon_q, canon_hdrs, signed_list, payload_hex);

    std::string sts = string_to_sign(
        ts.ymd_hms, ts.ymd, region, service, canon_req);

    // Derive signing key
    auto signing_key = derive_signing_key(
        creds.secret_access_key, ts.ymd, region, service);
    auto sig_bytes = hmac_bytes(signing_key, sts);
    std::string sig_hex = sha256_bytes_to_hex(sig_bytes);

    // Authorization =
    //   AWS4-HMAC-SHA256 Credential=<akid>/<scope>,
    //   SignedHeaders=<signed_list>, Signature=<hex>
    std::string scope;
    scope.reserve(ts.ymd.size() + 1 + region.size() + 1 + service.size() +
                  1 + 12);
    scope += ts.ymd;
    scope.push_back('/');
    scope += region;
    scope.push_back('/');
    scope += service;
    scope += "/aws4_request";

    std::string authz;
    authz.reserve(128 + creds.access_key_id.size() + scope.size() +
                  signed_list.size() + sig_hex.size());
    authz += "AWS4-HMAC-SHA256 Credential=";
    authz += creds.access_key_id;
    authz.push_back('/');
    authz += scope;
    authz += ", SignedHeaders=";
    authz += signed_list;
    authz += ", Signature=";
    authz += sig_hex;

    SignedRequest out;
    out.authorization = std::move(authz);
    out.headers = std::move(canon_hdrs);
    out.signed_headers = std::move(signed_list);
    out.request_date = std::move(ts.ymd_hms);
    return out;
}

// ---------------------------------------------------------------------------
// Credential chain (lite implementation — same priority as TS upstream).
// We intentionally do NOT attempt STS AssumeRole, SSO OIDC, credential_process
// — those require network calls and/or external binaries, and TS upstream
// covers them via the user-facing awsAuthRefresh / awsCredentialExport shell
// hooks.  The C++ chain covers the 90% "it just works on laptop / CI / EC2"
// case.
// ---------------------------------------------------------------------------

// Build a credentials struct by inspecting env vars first (highest priority
// after any user scripts have already run).
[[nodiscard]] inline std::optional<AwsCredentials>
credentials_from_env() {
    using cc::utils::env::get_env;
    auto akid = get_env("AWS_ACCESS_KEY_ID");
    auto sk = get_env("AWS_SECRET_ACCESS_KEY");
    if (!akid || !sk || akid->empty() || sk->empty()) return std::nullopt;
    AwsCredentials c{std::move(*akid), std::move(*sk), {}};
    if (auto st = get_env("AWS_SESSION_TOKEN"); st && !st->empty()) {
        c.session_token = std::move(*st);
    }
    return c;
}

// Parse the "standard" INI format for ~/.aws/credentials + ~/.aws/config.
// Supports: [section_name] headers, key = value lines, ; comments, # comments,
// blank lines, leading whitespace.  Returns flat map: section->key->value.
// We do NOT handle prefix "profile " stripping for config — callers pass
// profile name with or without prefix as needed.
using IniSections = std::unordered_map<std::string, std::unordered_map<std::string, std::string>>;
[[nodiscard]] inline IniSections parse_ini_file(const std::string& path) {
    IniSections out;
    std::ifstream f(path);
    if (!f) return out;
    std::string current;
    std::string line;
    while (std::getline(f, line)) {
        // strip trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // strip leading whitespace
        std::size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        char first = line[start];
        if (first == ';' || first == '#') continue;
        if (first == '[') {
            auto end = line.find(']', start + 1);
            if (end == std::string::npos) continue;
            current = line.substr(start + 1, end - start - 1);
            continue;
        }
        auto eq = line.find('=', start);
        if (eq == std::string::npos) continue;
        std::string key = line.substr(start, eq - start);
        // rstrip key
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        std::string val = line.substr(eq + 1);
        std::size_t vs = val.find_first_not_of(" \t");
        if (vs != std::string::npos) val = val.substr(vs);
        // rstrip value
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        if (!current.empty()) {
            out[current][std::move(key)] = std::move(val);
        }
    }
    return out;
}

// Read credentials from shared INI.  Honors AWS_SHARED_CREDENTIALS_FILE and
// AWS_CONFIG_FILE env overrides (same filenames as boto3 / AWS SDK).
// Profile lookup: AWS_PROFILE env, else "default".  Config file sections
// use "profile NAME" prefix per AWS spec — we try both the raw name and the
// "profile " prefix variant.
[[nodiscard]] inline std::optional<AwsCredentials>
credentials_from_ini() {
    using cc::utils::env::get_env;
    auto profile = get_env("AWS_PROFILE").value_or("default");
    const char* home_env = std::getenv("HOME");
    std::string home = home_env ? home_env : "";
    auto cred_file = get_env("AWS_SHARED_CREDENTIALS_FILE")
        .value_or(home + "/.aws/credentials");
    auto cfg_file = get_env("AWS_CONFIG_FILE")
        .value_or(home + "/.aws/config");

    const auto lookup = [&](const IniSections& ini, std::string_view pname)
        -> std::optional<AwsCredentials> {
        // Try exact match, then "profile <name>" match for config.
        auto it = ini.find(std::string(pname));
        if (it == ini.end()) {
            it = ini.find("profile " + std::string(pname));
            if (it == ini.end()) return std::nullopt;
        }
        const auto& m = it->second;
        auto ak = m.find("aws_access_key_id");
        auto sk = m.find("aws_secret_access_key");
        if (ak == m.end() || sk == m.end()) return std::nullopt;
        AwsCredentials c{ak->second, sk->second, {}};
        if (auto tok = m.find("aws_session_token"); tok != m.end()) {
            c.session_token = tok->second;
        }
        return c.valid() ? std::optional<AwsCredentials>(std::move(c))
                         : std::nullopt;
    };

    // Credentials file has highest priority (matches SDK chain).
    if (auto creds = lookup(parse_ini_file(cred_file), profile)) return creds;
    if (auto creds = lookup(parse_ini_file(cfg_file), profile)) return creds;
    return std::nullopt;
}

// Fetch IMDSv2 credentials — EC2 / ECS.  Callers should cache the result
// until role_credentials.expiration - 5 min.
// NOTE: This uses the direct httplib Client on 169.254.169.254:80.  In a
// restricted environment (firewall / no IMDS) this returns nullopt.  We
// guard the call with a short timeout (1.5s PUT + 1.5s GET).
[[nodiscard]] inline std::optional<IamRoleCredentials>
credentials_from_imds_v2(std::chrono::milliseconds timeout_ms = 1500ms) {
    static constexpr std::string_view kImdsHost = "169.254.169.254";
    static constexpr std::string_view kTtlHeader = "X-aws-ec2-metadata-token-ttl-seconds";

    try {
        httplib::Client cli(std::string(kImdsHost), 80);
        cli.set_connection_timeout(timeout_ms.count() / 1000,
                                   (timeout_ms.count() % 1000) * 1000);
        cli.set_read_timeout(timeout_ms.count() / 1000,
                             (timeout_ms.count() % 1000) * 1000);
        cli.set_write_timeout(timeout_ms.count() / 1000,
                              (timeout_ms.count() % 1000) * 1000);

        // Step 1: PUT to get TTL token.
        httplib::Headers token_req_headers;
        token_req_headers.emplace(std::string(kTtlHeader), "21600");
        auto put = cli.Put("/latest/api/token", token_req_headers, "",
                           "text/plain");
        if (!put || put->status != 200 || put->body.empty()) return std::nullopt;
        std::string token = put->body;

        // Step 2: GET role name.
        httplib::Headers imds_headers;
        imds_headers.emplace("X-aws-ec2-metadata-token", token);
        auto role_r = cli.Get("/latest/meta-data/iam/security-credentials/",
                              imds_headers);
        if (!role_r || role_r->status != 200 || role_r->body.empty()) {
            return std::nullopt;
        }
        std::string role_name = role_r->body;
        // Strip trailing \n if present (IMDS sometimes appends it).
        while (!role_name.empty() &&
               (role_name.back() == '\n' || role_name.back() == '\r')) {
            role_name.pop_back();
        }
        if (role_name.empty() || role_name.find('\n') != std::string::npos) {
            // Multiple roles listed — pick the first line.
            auto nl = role_name.find('\n');
            if (nl != std::string::npos) role_name = role_name.substr(0, nl);
        }

        // Step 3: GET credentials payload.
        std::string url = "/latest/meta-data/iam/security-credentials/" +
                          role_name;
        auto creds_r = cli.Get(url.c_str(), imds_headers);
        if (!creds_r || creds_r->status != 200) return std::nullopt;

        // Parse JSON.
        using namespace cc::utils::json;
        auto parsed = parse(creds_r->body);
        if (!parsed) return std::nullopt;
        auto root = parsed->root();
        auto code = root.get_string("Code");
        if (code != "Success") return std::nullopt;

        IamRoleCredentials rc;
        rc.code = std::move(code);
        rc.type = root.get_string("Type");
        rc.access_key_id = root.get_string("AccessKeyId");
        rc.secret_access_key = root.get_string("SecretAccessKey");
        rc.session_token = root.get_string("Token");
        rc.role_arn = root.get_string("LastUpdated");
        auto exp = root.get_string("Expiration");
        // Parse ISO-8601: "2025-01-15T12:34:56Z" — std::get_time portably.
        if (!exp.empty()) {
            std::tm tm{};
            std::istringstream iss(exp);
            iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            if (!iss.fail()) {
                std::time_t t = timegm(&tm);
                if (t != -1) {
                    rc.expiration = system_clock::from_time_t(t);
                } else {
                    rc.expiration = system_clock::now() + hours(5);
                }
            } else {
                rc.expiration = system_clock::now() + hours(5);
            }
        } else {
            rc.expiration = system_clock::now() + hours(5);
        }
        if (!rc.valid()) return std::nullopt;
        return rc;
    } catch (...) {
        return std::nullopt;
    }
}

// Run the full credential chain: env → ini → IMDSv2.
// Returns nullopt if NO provider yields valid credentials.
// Callers that support user scripts should normally prefer:
//   (a) run user's awsAuthRefresh / awsCredentialExport via bash_execution,
//   (b) then call resolve_default_credentials() so env/ini/IMDS fill in any
//       remaining gaps.
[[nodiscard]] inline std::optional<AwsCredentials>
resolve_default_credentials() {
    if (auto c = credentials_from_env()) return c;
    if (auto c = credentials_from_ini()) return c;
    if (auto c = credentials_from_imds_v2()) return AwsCredentials(*c);
    return std::nullopt;
}

// TS client.ts priority for region:
//   ANTHROPIC_SMALL_FAST_MODEL_AWS_REGION (per-model, handled by caller)
//     > AWS_REGION > AWS_DEFAULT_REGION > "us-east-1"
[[nodiscard]] inline std::string resolve_region() {
    using cc::utils::env::get_env;
    if (auto r = get_env("AWS_REGION"); r && !r->empty()) return *r;
    if (auto r = get_env("AWS_DEFAULT_REGION"); r && !r->empty()) return *r;
    return "us-east-1";
}

// Check for the three test-hook env vars that correspond to TS upstream
//   CLAUDE_CODE_USE_BEDROCK        (truthy = use Bedrock provider)
//   CLAUDE_CODE_SKIP_BEDROCK_AUTH  (truthy = skip SigV4, no-op creds)
//   AWS_BEARER_TOKEN_BEDROCK       (non-empty = inject Authorization: Bearer, skip SigV4)
struct BedrockAuthMode {
    bool use_bedrock = false;
    bool skip_auth = false;
    std::optional<std::string> bearer_token; // non-empty overrides everything
};
[[nodiscard]] inline BedrockAuthMode detect_bedrock_mode() {
    using cc::utils::env::get_env;
    using cc::utils::env::is_env_truthy;
    BedrockAuthMode m;
    m.use_bedrock = is_env_truthy("CLAUDE_CODE_USE_BEDROCK");
    m.skip_auth   = is_env_truthy("CLAUDE_CODE_SKIP_BEDROCK_AUTH");
    if (auto bt = get_env("AWS_BEARER_TOKEN_BEDROCK"); bt && !bt->empty()) {
        m.bearer_token = std::move(*bt);
        m.skip_auth = true; // Bearer token replaces SigV4 entirely
    }
    return m;
}

} // namespace cc::services::auth::aws
