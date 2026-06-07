module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <httplib.h>

export module cc.utils.http;


export namespace cc::utils {


enum class ProxyType { http, https, socks5 };


struct ProxyConfig {
    ProxyType type{ProxyType::http};
    std::string host;
    uint16_t port{8080};
    std::optional<std::string> auth_user;
    std::optional<std::string> auth_pass;
};


struct TlsConfig {
    std::optional<std::string> ca_cert_path;
    std::optional<std::string> client_cert_path;
    std::optional<std::string> client_key_path;
    bool verify_peer{true};
};


struct HttpConfig {
    std::optional<ProxyConfig> proxy;
    TlsConfig tls;
    uint32_t timeout_ms{30'000};
    uint32_t max_retries{3};
    uint32_t retry_backoff_ms{250};
    std::string user_agent{"cc-repl/2.0"};
};


struct HttpResponse {
    int status{0};
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::chrono::milliseconds elapsed{0};
    
    [[nodiscard]] auto is_ok() const -> bool { return status >= 200 && status < 300; }
    [[nodiscard]] auto is_rate_limited() const -> bool { return status == 429; }
};


struct HttpError {
    enum Code { connection_failed, timeout, ssl_error, dns_error, cancelled };
    Code code;
    std::string message;
};


struct SseEvent {
    std::string event;
    std::string data;
    std::string id;
};


using SseCallback = std::function<void(const SseEvent&)>;


struct ParsedUrl {
    std::string scheme;       // "http" or "https"
    std::string host;         // "host" or "host:port"
    std::string path;         // "/path/to/resource"
    std::string base_url;     // "https://host:port"
};


[[nodiscard]] inline auto parse_url(std::string_view url) -> std::expected<ParsedUrl, HttpError> {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return std::unexpected(HttpError{HttpError::connection_failed, "URL must include a scheme (http:// or https://)"});
    }

    ParsedUrl parsed;
    parsed.scheme = std::string(url.substr(0, scheme_end));
    if (parsed.scheme != "http" && parsed.scheme != "https") {
        return std::unexpected(HttpError{HttpError::connection_failed, std::format("Unsupported URL scheme: {}", parsed.scheme)});
    }

    auto authority_start = scheme_end + 3;
    auto path_start = url.find('/', authority_start);
    if (path_start == std::string_view::npos) {
        parsed.host = std::string(url.substr(authority_start));
        parsed.path = "/";
    } else {
        parsed.host = std::string(url.substr(authority_start, path_start - authority_start));
        parsed.path = std::string(url.substr(path_start));
    }

    if (parsed.host.empty()) {
        return std::unexpected(HttpError{HttpError::connection_failed, "URL host cannot be empty"});
    }

    parsed.base_url = parsed.scheme + "://" + parsed.host;
    return parsed;
}


class HttpClient {
    HttpConfig config_;


    [[nodiscard]] auto make_client(const std::string& base_url) const -> httplib::Client {
        httplib::Client cli = (config_.tls.client_cert_path && config_.tls.client_key_path)
            ? httplib::Client(base_url, *config_.tls.client_cert_path, *config_.tls.client_key_path)
            : httplib::Client(base_url);

        auto timeout_sec = static_cast<int>(config_.timeout_ms / 1000);
        auto timeout_usec = static_cast<int>((config_.timeout_ms % 1000) * 1000);
        cli.set_connection_timeout(timeout_sec, timeout_usec);
        cli.set_read_timeout(timeout_sec, timeout_usec);
        cli.set_write_timeout(timeout_sec, timeout_usec);
        cli.set_follow_location(true);


        if (config_.proxy) {
            const auto& proxy = *config_.proxy;
            cli.set_proxy(proxy.host, static_cast<int>(proxy.port));
            if (proxy.auth_user) {
                cli.set_proxy_basic_auth(
                    proxy.auth_user.value_or(""),
                    proxy.auth_pass.value_or(""));
            }
        }


        if (config_.tls.ca_cert_path) {
            cli.set_ca_cert_path(config_.tls.ca_cert_path->c_str());
        }
        if (!config_.tls.verify_peer) {
            cli.enable_server_certificate_verification(false);
        }

        return cli;
    }


    [[nodiscard]] static auto classify_error(httplib::Error err) -> HttpError {
        switch (err) {
            case httplib::Error::Connection:
                return {HttpError::connection_failed, std::format("Connection failed: {}", httplib::to_string(err))};
            case httplib::Error::Read:
            case httplib::Error::Write:
                return {HttpError::timeout, std::format("Timeout: {}", httplib::to_string(err))};
            case httplib::Error::SSLConnection:
            case httplib::Error::SSLLoadingCerts:
            case httplib::Error::SSLServerVerification:
                return {HttpError::ssl_error, std::format("SSL error: {}", httplib::to_string(err))};
            case httplib::Error::Canceled:
                return {HttpError::cancelled, "Request cancelled"};
            default:
                return {HttpError::connection_failed, std::format("HTTP error: {}", httplib::to_string(err))};
        }
    }


    [[nodiscard]] auto build_headers(const std::unordered_map<std::string, std::string>& extra) const -> httplib::Headers {
        httplib::Headers h;
        h.emplace("User-Agent", config_.user_agent);
        for (const auto& [k, v] : extra) {
            h.emplace(k, v);
        }
        return h;
    }

public:
    explicit HttpClient(HttpConfig config = {}) : config_(std::move(config)) {}


    [[nodiscard]] auto get(std::string_view url,
        const std::unordered_map<std::string, std::string>& headers = {})
        -> std::expected<HttpResponse, HttpError> {
        if (url.empty()) return std::unexpected(HttpError{HttpError::connection_failed, "empty URL"});

        auto parsed = parse_url(url);
        if (!parsed) return std::unexpected(parsed.error());

        auto start = std::chrono::steady_clock::now();
        auto h = build_headers(headers);


        for (uint32_t attempt = 0; attempt <= config_.max_retries; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.retry_backoff_ms * attempt));
            }

            auto cli = make_client(parsed->base_url);

            auto res = cli.Get(parsed->path, h);
            if (!res) {
                auto err = classify_error(res.error());
                if (attempt == config_.max_retries) return std::unexpected(err);
                continue;
            }

            HttpResponse response;
            response.status = res->status;
            response.body = std::move(res->body);
            for (const auto& [k, v] : res->headers) {
                response.headers[k] = v;
            }
            response.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);


            if ((response.status >= 500 || response.status == 429) && attempt < config_.max_retries) {
                continue;
            }
            return response;
        }

        return std::unexpected(HttpError{HttpError::connection_failed, "max retries exceeded"});
    }


    [[nodiscard]] auto post(std::string_view url, std::string_view body,
        const std::unordered_map<std::string, std::string>& headers = {})
        -> std::expected<HttpResponse, HttpError> {
        if (url.empty()) return std::unexpected(HttpError{HttpError::connection_failed, "empty URL"});

        auto parsed = parse_url(url);
        if (!parsed) return std::unexpected(parsed.error());

        auto start = std::chrono::steady_clock::now();
        auto h = build_headers(headers);


        std::string content_type = "application/json";
        for (const auto& [k, v] : headers) {
            if (k == "Content-Type" || k == "content-type") {
                content_type = v;
                break;
            }
        }

        std::string body_str(body);


        for (uint32_t attempt = 0; attempt <= config_.max_retries; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.retry_backoff_ms * attempt));
            }

            auto cli = make_client(parsed->base_url);

            auto res = cli.Post(parsed->path, h, body_str, content_type);
            if (!res) {
                auto err = classify_error(res.error());
                if (attempt == config_.max_retries) return std::unexpected(err);
                continue;
            }

            HttpResponse response;
            response.status = res->status;
            response.body = std::move(res->body);
            for (const auto& [k, v] : res->headers) {
                response.headers[k] = v;
            }
            response.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);


            if ((response.status >= 500 || response.status == 429) && attempt < config_.max_retries) {
                continue;
            }
            return response;
        }

        return std::unexpected(HttpError{HttpError::connection_failed, "max retries exceeded"});
    }


    [[nodiscard]] auto patch(std::string_view url, std::string_view body,
        const std::unordered_map<std::string, std::string>& headers = {})
        -> std::expected<HttpResponse, HttpError> {
        if (url.empty()) return std::unexpected(HttpError{HttpError::connection_failed, "empty URL"});

        auto parsed = parse_url(url);
        if (!parsed) return std::unexpected(parsed.error());

        auto start = std::chrono::steady_clock::now();
        auto h = build_headers(headers);

        std::string content_type = "application/json";
        for (const auto& [k, v] : headers) {
            if (k == "Content-Type" || k == "content-type") {
                content_type = v;
                break;
            }
        }

        std::string body_str(body);

        for (uint32_t attempt = 0; attempt <= config_.max_retries; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.retry_backoff_ms * attempt));
            }

            auto cli = make_client(parsed->base_url);

            auto res = cli.Patch(parsed->path, h, body_str, content_type);
            if (!res) {
                auto err = classify_error(res.error());
                if (attempt == config_.max_retries) return std::unexpected(err);
                continue;
            }

            HttpResponse response;
            response.status = res->status;
            response.body = std::move(res->body);
            for (const auto& [k, v] : res->headers) {
                response.headers[k] = v;
            }
            response.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);

            if ((response.status >= 500 || response.status == 429) && attempt < config_.max_retries) {
                continue;
            }
            return response;
        }

        return std::unexpected(HttpError{HttpError::connection_failed, "max retries exceeded"});
    }

    [[nodiscard]] auto put(std::string_view url, std::string_view body,
        const std::unordered_map<std::string, std::string>& headers = {})
        -> std::expected<HttpResponse, HttpError> {
        if (url.empty()) return std::unexpected(HttpError{HttpError::connection_failed, "empty URL"});

        auto parsed = parse_url(url);
        if (!parsed) return std::unexpected(parsed.error());

        auto start = std::chrono::steady_clock::now();
        auto h = build_headers(headers);

        std::string content_type = "application/json";
        for (const auto& [k, v] : headers) {
            if (k == "Content-Type" || k == "content-type") {
                content_type = v;
                break;
            }
        }

        std::string body_str(body);

        for (uint32_t attempt = 0; attempt <= config_.max_retries; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.retry_backoff_ms * attempt));
            }

            auto cli = make_client(parsed->base_url);

            auto res = cli.Put(parsed->path, h, body_str, content_type);
            if (!res) {
                auto err = classify_error(res.error());
                if (attempt == config_.max_retries) return std::unexpected(err);
                continue;
            }

            HttpResponse response;
            response.status = res->status;
            response.body = std::move(res->body);
            for (const auto& [k, v] : res->headers) {
                response.headers[k] = v;
            }
            response.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);

            if ((response.status >= 500 || response.status == 429) && attempt < config_.max_retries) {
                continue;
            }
            return response;
        }

        return std::unexpected(HttpError{HttpError::connection_failed, "max retries exceeded"});
    }


    [[nodiscard]] auto delete_request(std::string_view url,
        const std::unordered_map<std::string, std::string>& headers = {})
        -> std::expected<HttpResponse, HttpError> {
        if (url.empty()) return std::unexpected(HttpError{HttpError::connection_failed, "empty URL"});

        auto parsed = parse_url(url);
        if (!parsed) return std::unexpected(parsed.error());

        auto start = std::chrono::steady_clock::now();
        auto h = build_headers(headers);

        for (uint32_t attempt = 0; attempt <= config_.max_retries; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.retry_backoff_ms * attempt));
            }

            auto cli = make_client(parsed->base_url);

            auto res = cli.Delete(parsed->path, h);
            if (!res) {
                auto err = classify_error(res.error());
                if (attempt == config_.max_retries) return std::unexpected(err);
                continue;
            }

            HttpResponse response;
            response.status = res->status;
            response.body = std::move(res->body);
            for (const auto& [k, v] : res->headers) {
                response.headers[k] = v;
            }
            response.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);

            if ((response.status >= 500 || response.status == 429) && attempt < config_.max_retries) {
                continue;
            }
            return response;
        }

        return std::unexpected(HttpError{HttpError::connection_failed, "max retries exceeded"});
    }


    [[nodiscard]] auto stream_sse(std::string_view url,
        const std::unordered_map<std::string, std::string>& headers,
        SseCallback on_event) -> std::expected<void, HttpError> {
        if (url.empty()) return std::unexpected(HttpError{HttpError::connection_failed, "empty URL"});

        auto parsed = parse_url(url);
        if (!parsed) return std::unexpected(parsed.error());

        auto h = build_headers(headers);
        h.emplace("Accept", "text/event-stream");
        h.emplace("Cache-Control", "no-cache");

        auto cli = make_client(parsed->base_url);

        std::string sse_buffer;
        std::string current_event;
        std::string current_data;
        std::string current_id;

        auto process_sse_buffer = [&]() {
            while (true) {
                auto double_nl = sse_buffer.find("\n\n");
                if (double_nl == std::string::npos) break;

                std::string event_block = sse_buffer.substr(0, double_nl);
                sse_buffer.erase(0, double_nl + 2);


                std::string event_type;
                std::string event_data;
                std::string event_id;
                size_t pos = 0;
                while (pos < event_block.size()) {
                    auto nl = event_block.find('\n', pos);
                    std::string line;
                    if (nl == std::string::npos) {
                        line = event_block.substr(pos);
                        pos = event_block.size();
                    } else {
                        line = event_block.substr(pos, nl - pos);
                        pos = nl + 1;
                    }


                    if (line.starts_with(":")) continue;

                    if (line.starts_with("event: ")) {
                        event_type = line.substr(7);
                    } else if (line.starts_with("event:")) {
                        event_type = line.substr(6);
                    } else if (line.starts_with("data: ")) {
                        if (!event_data.empty()) event_data += "\n";
                        event_data += line.substr(6);
                    } else if (line.starts_with("data:")) {
                        if (!event_data.empty()) event_data += "\n";
                        event_data += line.substr(5);
                    } else if (line == "data") {
                        if (!event_data.empty()) event_data += "\n";
                    } else if (line.starts_with("id: ")) {
                        event_id = line.substr(4);
                    } else if (line.starts_with("id:")) {
                        event_id = line.substr(3);
                    }
                }


                if (!event_data.empty() || !event_type.empty()) {
                    if (on_event) {
                        on_event(SseEvent{
                            .event = event_type.empty() ? "message" : event_type,
                            .data = event_data,
                            .id = event_id
                        });
                    }
                }
            }
        };


        auto res = cli.Get(parsed->path, h,
            [&](const char* data, size_t len) -> bool {
                sse_buffer.append(data, len);
                process_sse_buffer();
                return true;
            });


        if (!sse_buffer.empty()) {

            if (!sse_buffer.ends_with("\n\n")) {
                sse_buffer += "\n\n";
            }
            process_sse_buffer();
        }

        if (!res) {
            auto err = classify_error(res.error());

            if (res.error() == httplib::Error::Read) {
                return {};
            }
            return std::unexpected(err);
        }

        if (res->status >= 400) {
            return std::unexpected(HttpError{
                HttpError::connection_failed,
                std::format("SSE connection failed with status {}: {}", res->status, res->body)});
        }

        return {};
    }


    [[nodiscard]] auto post_stream_sse(std::string_view url,
        std::string_view body,
        const std::unordered_map<std::string, std::string>& headers,
        SseCallback on_event) -> std::expected<void, HttpError> {
        if (url.empty()) return std::unexpected(HttpError{HttpError::connection_failed, "empty URL"});

        auto parsed = parse_url(url);
        if (!parsed) return std::unexpected(parsed.error());

        auto h = build_headers(headers);
        h.emplace("Accept", "text/event-stream");
        h.emplace("Cache-Control", "no-cache");

        std::string content_type = "application/json";
        bool has_content_type = false;
        for (const auto& [k, v] : headers) {
            if (k == "Content-Type" || k == "content-type") {
                content_type = v;
                has_content_type = true;
                break;
            }
        }
        if (!has_content_type) {
            h.emplace("Content-Type", content_type);
        }

        auto cli = make_client(parsed->base_url);
        cli.set_read_timeout(static_cast<int>(config_.timeout_ms / 1000),
            static_cast<int>((config_.timeout_ms % 1000) * 1000));

        std::string sse_buffer;
        auto process_sse_buffer = [&]() {
            while (true) {
                auto double_nl = sse_buffer.find("\n\n");
                auto crlf_double_nl = sse_buffer.find("\r\n\r\n");
                std::size_t delimiter = std::string::npos;
                std::size_t delimiter_size = 0;
                if (double_nl != std::string::npos &&
                    (crlf_double_nl == std::string::npos || double_nl < crlf_double_nl)) {
                    delimiter = double_nl;
                    delimiter_size = 2;
                } else if (crlf_double_nl != std::string::npos) {
                    delimiter = crlf_double_nl;
                    delimiter_size = 4;
                }
                if (delimiter == std::string::npos) break;

                std::string event_block = sse_buffer.substr(0, delimiter);
                sse_buffer.erase(0, delimiter + delimiter_size);

                std::string event_type;
                std::string event_data;
                std::string event_id;
                size_t pos = 0;
                while (pos < event_block.size()) {
                    auto nl = event_block.find('\n', pos);
                    std::string line;
                    if (nl == std::string::npos) {
                        line = event_block.substr(pos);
                        pos = event_block.size();
                    } else {
                        line = event_block.substr(pos, nl - pos);
                        pos = nl + 1;
                    }
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }

                    if (line.starts_with(":")) continue;
                    if (line.starts_with("event: ")) {
                        event_type = line.substr(7);
                    } else if (line.starts_with("event:")) {
                        event_type = line.substr(6);
                    } else if (line.starts_with("data: ")) {
                        if (!event_data.empty()) event_data += "\n";
                        event_data += line.substr(6);
                    } else if (line.starts_with("data:")) {
                        if (!event_data.empty()) event_data += "\n";
                        event_data += line.substr(5);
                    } else if (line == "data") {
                        if (!event_data.empty()) event_data += "\n";
                    } else if (line.starts_with("id: ")) {
                        event_id = line.substr(4);
                    } else if (line.starts_with("id:")) {
                        event_id = line.substr(3);
                    }
                }

                if (!event_data.empty() || !event_type.empty()) {
                    if (on_event) {
                        on_event(SseEvent{
                            .event = event_type.empty() ? "message" : event_type,
                            .data = event_data,
                            .id = event_id
                        });
                    }
                }
            }
        };

        httplib::Request req;
        req.method = "POST";
        req.path = parsed->path;
        req.headers = std::move(h);
        req.body = std::string(body);
        req.content_receiver = [&](const char* data, size_t len,
                                   uint64_t /*offset*/, uint64_t /*total*/) -> bool {
            sse_buffer.append(data, len);
            process_sse_buffer();
            return true;
        };

        httplib::Response res;
        httplib::Error err;
        const bool ok = cli.send(req, res, err);

        if (!sse_buffer.empty()) {
            if (!sse_buffer.ends_with("\n\n") && !sse_buffer.ends_with("\r\n\r\n")) {
                sse_buffer += "\n\n";
            }
            process_sse_buffer();
        }

        if (!ok) {
            if (err == httplib::Error::Read) {
                return {};
            }
            return std::unexpected(classify_error(err));
        }

        if (res.status >= 400) {
            return std::unexpected(HttpError{
                HttpError::connection_failed,
                std::format("SSE POST failed with status {}: {}", res.status, res.body)});
        }

        return {};
    }


    void configure(HttpConfig config) { config_ = std::move(config); }
    void set_proxy(ProxyConfig proxy) { config_.proxy = std::move(proxy); }
    void set_tls(TlsConfig tls) { config_.tls = std::move(tls); }
    
    auto with_retry(uint32_t max_attempts, uint32_t backoff_ms) -> HttpClient& {
        config_.max_retries = max_attempts;
        config_.retry_backoff_ms = backoff_ms;
        return *this;
    }

    [[nodiscard]] auto get_config() const -> const HttpConfig& { return config_; }
};


[[nodiscard]] inline auto detect_proxy_from_env() -> std::optional<ProxyConfig> {

    for (auto env_var : {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy", "ALL_PROXY"}) {
        if (auto* val = std::getenv(env_var); val && val[0] != '\0') {
            std::string url(val);
            ProxyConfig proxy;
            if (url.starts_with("https://")) { proxy.type = ProxyType::https; url.erase(0, 8); }
            else if (url.starts_with("http://")) { proxy.type = ProxyType::http; url.erase(0, 7); }
            else if (url.starts_with("socks5://")) { proxy.type = ProxyType::socks5; url.erase(0, 9); }
            if (auto at = url.find('@'); at != std::string::npos) {
                auto auth = url.substr(0, at);
                url.erase(0, at + 1);
                if (auto colon = auth.find(':'); colon != std::string::npos) {
                    proxy.auth_user = auth.substr(0, colon);
                    proxy.auth_pass = auth.substr(colon + 1);
                } else {
                    proxy.auth_user = auth;
                }
            }
            if (auto colon = url.rfind(':'); colon != std::string::npos) {
                proxy.host = url.substr(0, colon);
                proxy.port = static_cast<uint16_t>(std::stoi(url.substr(colon + 1)));
            } else {
                proxy.host = url;
                proxy.port = proxy.type == ProxyType::https ? 443 : 8080;
            }
            return proxy;
        }
    }
    return std::nullopt;
}

} // namespace cc::utils
