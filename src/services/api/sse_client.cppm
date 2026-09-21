// Anthropic SSE Streaming Client (Phase 3-E)
// Lightweight, dependency-minimal SSE client built on libcurl + cc.utils.json.
// - text-only messages (Phase 3)
// - dry-run gate when api_key is empty (no network)
// - abort propagation via should_abort callback
module;
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <curl/curl.h>

export module cc.services.api.sse;

import cc.utils.json;

// Note: this module intentionally avoids importing cc.services.api.* so it can
// be used as an independent SSE primitive.  The full-featured AnthropicClient
// in cc.services.api.client builds on top of this module.

export namespace cc::services::api::sse {

namespace json = cc::utils::json;

// =========================================================================
// Text-only message model
// =========================================================================

enum class Role { System, User, Assistant };

struct TextMessage {
    Role        role    = Role::User;
    std::string content;  // UTF-8 text
};

// =========================================================================
// SSE Event Types
// =========================================================================

enum class SseEventType {
    MessageStart,         // event: message_start
    ContentBlockStart,    // event: content_block_start (type: "text")
    ContentBlockDelta,    // event: content_block_delta -> delta.text
    ContentBlockStop,     // event: content_block_stop
    MessageDelta,         // event: message_delta (stop_reason, usage)
    MessageStop,          // event: message_stop
    Error,                // event: error -> JSON error
    Ping,                 // event: ping
    Unknown,
};

// =========================================================================
// Streaming Callbacks
// =========================================================================

struct StreamingCallbacks {
    /// Called once per complete SSE event (raw event/data pair kept for
    /// debugging).  Never invoked with empty events.
    std::function<void(SseEventType type, std::string_view data)> on_sse_event;
    /// Called on every text delta extracted from
    /// content_block_delta.delta.text.
    std::function<void(std::string_view text_delta)> on_text_delta;
    /// Called exactly once per request — either after receiving message_stop,
    /// or when a terminal error / abort happens.
    std::function<void(int         final_http_status,
                       std::string final_stop_reason,
                       std::string final_accumulated_text,
                       std::string error_reason)> on_final;
    /// Polled periodically (at least once per WRITEFUNCTION invocation).
    /// Return true to abort the transfer.
    std::function<bool()> should_abort;
};

// =========================================================================
// ApiConfig
// =========================================================================

struct ApiConfig {
    std::string base_url    = "https://api.anthropic.com/v1";
    std::string api_key;    // if empty -> dry-run (no network, immediate on_final)
    std::string api_version = "2023-06-01";
    std::string model_id    = "claude-sonnet-4-20250514";
    int         max_tokens  = 8192;
    double      temperature = 0.7;
};

// =========================================================================
// SseClient
// =========================================================================

class SseClient {
public:
    explicit SseClient(ApiConfig cfg) : cfg_(std::move(cfg)) {
        // curl_global_init is not thread-safe; guard with atomic flag.
        [[maybe_unused]] static const int init_rc = [] {
            return curl_global_init(CURL_GLOBAL_ALL);
        }();
    }

    ~SseClient() = default;  // curl_global_cleanup is intentionally skipped:
                             // shared-ownership libcurl in long-lived apps.

    /// POST /messages with stream=true.  Blocks until stream completes, an
    /// error occurs, or should_abort returns true.
    ///
    /// Returns:
    ///   0               : HTTP 200 + stream completed with message_stop
    ///   positive value  : HTTP status (e.g. 401, 429, 500, ...)
    ///   negative value  : libcurl CURLE_* code (transport failure)
    int PostMessagesStream(std::string_view system_prompt,
                           std::span<const TextMessage> messages,
                           StreamingCallbacks cbs = {}) {
        ResetParser();

        // ---- Dry-run gate: honour empty api_key, never go out to network ----
        if (cfg_.api_key.empty()) {
            if (cbs.on_final) {
                cbs.on_final(0,
                             "dry_run",
                             /* accumulated text */ "",
                             "[dry-run] no api key provided");
            }
            return 0;
        }

        // ---- Build request body ------------------------------------------
        const std::string body = BuildMessagesBody(system_prompt, messages);

        // ---- Build full URL ----------------------------------------------
        const std::string url = cfg_.base_url + "/messages";

        // ---- Scratch state captured in WRITEFUNCTION --------------------
        struct TransferState {
            SseClient*         client;
            StreamingCallbacks* cbs;
            std::string*       accum;
            std::string*       stop_reason;
            int*               http_status;
            std::string*       error_reason;
            bool               final_called = false;
        };

        int                http_status  = 0;
        std::string        accumulated;
        std::string        stop_reason;
        std::string        error_reason;
        TransferState      state{this, &cbs, &accumulated, &stop_reason,
                            &http_status, &error_reason};

        CURL* curl = curl_easy_init();
        if (!curl) {
            if (cbs.on_final) cbs.on_final(0, "", "",
                                           "libcurl: curl_easy_init failed");
            return static_cast<int>(CURLE_FAILED_INIT);
        }

        // Wrap callback state so lambdas don't dangle.
        auto write_cb = +[](char* ptr, size_t size, size_t nmemb, void* userdata)
                            -> size_t {
            const std::size_t n = size * nmemb;
            auto* st = static_cast<TransferState*>(userdata);

            // --- header-less body: feed parser straight away ---
            std::string_view chunk{ptr, n};

            // Detect and emit text deltas, message_stop, error by re-parsing
            // every full SSE event inside FeedParser.
            st->client->FeedParserImpl(chunk, *st->cbs, *st->accum,
                                       *st->stop_reason, *st->error_reason);

            // Abort propagation
            if (st->cbs->should_abort && st->cbs->should_abort()) {
                return CURL_WRITEFUNC_PAUSE;  // will be translated to abort below
            }
            return n;
        };

        auto header_cb = +[](char* ptr, size_t size, size_t nmemb, void* userdata)
                             -> size_t {
            const std::size_t n = size * nmemb;
            auto* st = static_cast<TransferState*>(userdata);
            // Look for "HTTP/1.1 429 Too Many Requests" style line
            std::string_view line{ptr, n};
            if (line.starts_with("HTTP/")) {
                // find 3-digit status
                const auto space1 = line.find(' ');
                if (space1 != std::string_view::npos) {
                    const auto after = line.substr(space1 + 1);
                    if (after.size() >= 3 &&
                        std::isdigit(static_cast<unsigned char>(after[0])) &&
                        std::isdigit(static_cast<unsigned char>(after[1])) &&
                        std::isdigit(static_cast<unsigned char>(after[2]))) {
                        *st->http_status =
                            (after[0] - '0') * 100 +
                            (after[1] - '0') * 10 +
                            (after[2] - '0');
                    }
                }
            }
            return n;
        };

        // ---- Build headers ----------------------------------------------
        struct curl_slist* hdrs = nullptr;
        auto add_hdr = [&](std::string_view h) {
            hdrs = curl_slist_append(hdrs, std::string(h).c_str());
        };
        add_hdr("content-type: application/json");
        add_hdr("accept: text/event-stream");
        add_hdr(std::string("x-api-key: ").append(cfg_.api_key));
        add_hdr(std::string("anthropic-version: ").append(cfg_.api_version));

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(body.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        // Allow long-lived streaming responses
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 180L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);  // we parse body on error

        // Loopback no-proxy for local tests
        if (url.starts_with("http://127.0.0.1") ||
            url.starts_with("http://localhost") ||
            url.starts_with("https://127.0.0.1") ||
            url.starts_with("https://localhost") ||
            url.starts_with("http://[::1]") ||
            url.starts_with("https://[::1]")) {
            curl_easy_setopt(curl, CURLOPT_NOPROXY, "localhost,127.0.0.1,::1");
        }

        const CURLcode rc = curl_easy_perform(curl);

        // ---- Emit on_final exactly once ---------------------------------
        if (!state.final_called && cbs.on_final) {
            if (rc != CURLE_OK) {
                error_reason = std::string("libcurl error: ") + curl_easy_strerror(rc);
                cbs.on_final(static_cast<int>(rc) < 0
                                 ? static_cast<int>(rc)
                                 : -static_cast<int>(rc),
                             std::move(stop_reason),
                             std::move(accumulated),
                             std::move(error_reason));
            } else {
                long http_long = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_long);
                http_status = static_cast<int>(http_long);
                if (http_status == 0) http_status = 200;  // dry-run fallback
                cbs.on_final(http_status,
                             std::move(stop_reason),
                             std::move(accumulated),
                             std::move(error_reason));
            }
            state.final_called = true;
        }

        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            // CURLE codes are small positive values; signify transport error
            // by returning negated value (per interface contract).
            return rc == CURLE_OK ? 0 : -static_cast<int>(rc);
        }
        return http_status;
    }

    /// Synchronous non-streaming facade (uses stream internally, then
    /// accumulates all text deltas).
    struct SyncResult {
        int         http_status = 0;
        std::string text;
        std::string error;
    };

    SyncResult PostMessagesSync(std::string_view system_prompt,
                                std::span<const TextMessage> messages) {
        SyncResult res;
        StreamingCallbacks cbs;
        cbs.on_text_delta = [&res](std::string_view delta) {
            res.text.append(delta.data(), delta.size());
        };
        cbs.on_final = [&res](int http, std::string_view /*stop*/,
                              std::string_view /*accum*/,
                              std::string_view err) {
            res.http_status = http;
            res.error       = std::string(err);
        };
        const int rc = PostMessagesStream(system_prompt, messages, std::move(cbs));
        if (rc != 0 && res.http_status == 0) res.http_status = rc;
        return res;
    }

    /// Feed raw bytes into the SSE parser; exposed so the caller can unit-test
    /// parsing in isolation without network I/O.  Unlike PostMessagesStream
    /// which owns its accumulator, this overload writes into caller-owned
    /// out_accum/out_stop/out_err so unit tests and the QueryEngine mock driver
    /// can inspect parser state without needing a live SSE writer closure.
    void FeedParser(std::string_view      bytes,
                    StreamingCallbacks& cbs,
                    std::string&      out_accumulated_text,
                    std::string&      out_stop_reason,
                    std::string&      out_error_reason) {
        FeedParserImpl(bytes, cbs,
                     out_accumulated_text,
                     out_stop_reason,
                     out_error_reason);
    }

    /// Backwards-compatible overload: FeedParser with no output references — discards
    /// accumulated_text / stop / err.
    void FeedParser(std::string_view bytes, StreamingCallbacks& cbs) {
        std::string dummy_accum, dummy_stop, dummy_err;
        FeedParserImpl(bytes, cbs, dummy_accum, dummy_stop, dummy_err);
    }

    /// Reset the internal SSE buffer state.
    void ResetParser() noexcept { parser_buf_.clear(); }

private:
    // -----------------------------------------------------------------
    // Body builder using cc.utils.json (RAII mutable doc) for zero-copy headers
    // -----------------------------------------------------------------
    [[nodiscard]] std::string BuildMessagesBody(
        std::string_view system_prompt,
        std::span<const TextMessage> messages) const {

        json::JsonMutDoc doc;
        json::JsonMutVal root = doc.object();
        doc.set_root(root);

        auto add_sv = [&](std::string_view key, std::string_view value) {
            root.add(key, doc.string(value));
        };
        add_sv("model", cfg_.model_id);
        root.add("max_tokens", doc.number(static_cast<int64_t>(cfg_.max_tokens)));
        root.add("temperature", doc.number(cfg_.temperature));
        root.add("stream", doc.boolean(true));

        if (!system_prompt.empty()) {
            add_sv("system", system_prompt);
        }

        json::JsonMutVal msgs = doc.array();
        for (const auto& m : messages) {
            json::JsonMutVal obj = doc.object();
            const char* role_str =
                m.role == Role::System ? "system" :
                m.role == Role::Assistant ? "assistant" : "user";
            obj.add("role", doc.string(role_str));

            // content = [{type:"text", text:m.content}]
            json::JsonMutVal content_arr = doc.array();
            json::JsonMutVal text_blk = doc.object();
            text_blk.add("type", doc.string("text"));
            text_blk.add("text", doc.string(m.content));
            content_arr.append(text_blk);
            obj.add("content", content_arr);

            msgs.append(obj);
        }
        root.add("messages", msgs);

        return doc.to_string();
    }

    // -----------------------------------------------------------------
    // SSE Parser: feeds partial buffer, emits complete events (newline
    // terminated `\n\n`) and pulls JSON fields for text deltas / stop.
    // -----------------------------------------------------------------
    void FeedParserImpl(std::string_view chunk,
                        StreamingCallbacks& cbs,
                        std::string& accumulated_text,
                        std::string& out_stop_reason,
                        std::string& out_error_reason) {
        parser_buf_.append(chunk.data(), chunk.size());

        while (true) {
            // Find double newline = end of SSE event. Accept \n\n and \r\n\r\n.
            std::size_t sep = std::string::npos;
            for (std::size_t i = 0; i + 1 < parser_buf_.size(); ++i) {
                const char c = parser_buf_[i];
                const char d = parser_buf_[i + 1];
                if (c == '\n' && d == '\n') { sep = i; break; }
                if (c == '\r' && d == '\n' && i + 3 < parser_buf_.size() &&
                    parser_buf_[i + 2] == '\r' && parser_buf_[i + 3] == '\n') {
                    sep = i; break;
                }
            }
            if (sep == std::string::npos) break;

            const bool crlf_style = parser_buf_[sep] == '\r';
            const std::size_t sep_len = crlf_style ? 4 : 2;

            const std::string event_text = parser_buf_.substr(0, sep);
            parser_buf_.erase(0, sep + sep_len);

            SseEventType type_hint = SseEventType::Unknown;
            std::string  data_str;
            bool         has_data = false;

            // Scan event line-by-line
            std::size_t line_start = 0;
            while (line_start <= event_text.size()) {
                std::size_t line_end = event_text.find('\n', line_start);
                if (line_end == std::string::npos) line_end = event_text.size();
                std::string_view line =
                    std::string_view(event_text).substr(line_start, line_end - line_start);
                // strip trailing \r
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                line_start = line_end + 1;

                if (line.empty()) continue;
                if (line.front() == ':') continue;  // comment

                if (line.starts_with("event:")) {
                    auto val = line.substr(6);
                    if (!val.empty() && val.front() == ' ') val.remove_prefix(1);
                    type_hint = ClassifyEvent(val);
                } else if (line.starts_with("data:")) {
                    auto val = line.substr(5);
                    if (!val.empty() && val.front() == ' ') val.remove_prefix(1);
                    if (has_data) data_str.push_back('\n');
                    data_str.append(val.data(), val.size());
                    has_data = true;
                }
                // id/retry ignored here
            }

            if (!has_data && type_hint == SseEventType::Unknown) continue;

            // Fire raw SSE event callback
            if (cbs.on_sse_event) {
                cbs.on_sse_event(type_hint, data_str);
            }

            // ---- Extract typed payloads ----------------------------------
            switch (type_hint) {
                case SseEventType::ContentBlockDelta: {
                    auto parsed = json::parse(data_str);
                    if (!parsed) break;
                    json::JsonVal root = parsed->root();
                    if (auto delta = root.get("delta")) {
                        if (auto text = delta.get("text")) {
                            std::string_view sv = text.as_str();
                            accumulated_text.append(sv.data(), sv.size());
                            if (cbs.on_text_delta) cbs.on_text_delta(sv);
                        }
                    }
                    break;
                }
                case SseEventType::MessageDelta: {
                    auto parsed = json::parse(data_str);
                    if (!parsed) break;
                    json::JsonVal root = parsed->root();
                    if (auto delta = root.get("delta")) {
                        if (auto sr = delta.get("stop_reason")) {
                            std::string_view sv = sr.as_str();
                            out_stop_reason.assign(sv.data(), sv.size());
                        }
                    }
                    break;
                }
                case SseEventType::Error: {
                    auto parsed = json::parse(data_str);
                    if (!parsed) {
                        out_error_reason = data_str;
                        break;
                    }
                    json::JsonVal root = parsed->root();
                    if (auto err = root.get("error")) {
                        if (auto msg = err.get("message")) {
                            std::string_view sv = msg.as_str();
                            out_error_reason.assign(sv.data(), sv.size());
                        } else if (auto type_v = err.get("type")) {
                            // Fallback: serialize the whole error object; if that
                            // fails, fall back to the bare type string.
                            std::string serialized = json::to_string(err);
                            if (!serialized.empty()) {
                                out_error_reason = std::move(serialized);
                            } else {
                                std::string_view sv = type_v.as_str();
                                out_error_reason.assign(sv.data(), sv.size());
                            }
                        }
                    }
                    break;
                }
                case SseEventType::MessageStop:
                    // Terminal event.  We intentionally do NOT fire on_final
                    // here — on_final is fired once by PostMessagesStream when
                    // curl_easy_perform returns, so HTTP status is included.
                    break;
                default:
                    break;
            }
        }
    }

    [[nodiscard]] static SseEventType ClassifyEvent(std::string_view name) noexcept {
        if (name == "message_start")       return SseEventType::MessageStart;
        if (name == "content_block_start") return SseEventType::ContentBlockStart;
        if (name == "content_block_delta") return SseEventType::ContentBlockDelta;
        if (name == "content_block_stop")  return SseEventType::ContentBlockStop;
        if (name == "message_delta")       return SseEventType::MessageDelta;
        if (name == "message_stop")        return SseEventType::MessageStop;
        if (name == "error")               return SseEventType::Error;
        if (name == "ping")                return SseEventType::Ping;
        return SseEventType::Unknown;
    }

    ApiConfig   cfg_;
    std::string parser_buf_;  // incomplete SSE event buffer (cross-chunk)
};

// =========================================================================
// Task #37 dispatch compatibility layer (4-field SseCallbacks + Message(role/text_content))
// =========================================================================

/// 7 SSE event types required by the dispatch layer (lives in its own namespace
/// to avoid ambiguity with cc::services::api::sse::SseEventType which includes
/// ContentBlockStart / Unknown extensions).
enum class SseEvent {
    MessageStart,
    ContentBlockDelta,
    ContentBlockStop,
    MessageDelta,
    MessageStop,
    Error,
    Ping,
};

/// Plain-text message required by dispatch (role as string, no image/tool support).
struct Message {
    std::string role = "user";   // "user" or "assistant" (dispatch semantics)
    std::string text_content;
};

/// 4-field callback set required by dispatch:
///   on_event(string_view event_name)     - raw SSE event: ... string
///   on_data(string_view  data)           - raw SSE data: ... accumulated string
///   on_error(int http_status, string reason) - terminal error (HTTP status or libcurl)
///   on_complete()                        - message_stop or dry-run normal completion
struct SseCallbacks {
    std::function<void(std::string_view event)> on_event;
    std::function<void(std::string_view data)>  on_data;
    std::function<void(int http_status, std::string error_reason)> on_error;
    std::function<void()>                         on_complete;
};

/// Adapts dispatch-style `PostMessagesStream(system_prompt, vector<Message>, SseCallbacks)`
/// to the internal StreamingCallbacks; also constructs ApiConfig.
/// Return semantics match SseClient::PostMessagesStream:
///   0=success, >0=HTTP status, <0=negated libcurl CURLE code.
inline int PostMessagesStream(
    std::string_view              base_url,
    std::string_view              api_key,
    std::string_view              system_prompt,
    const std::vector<Message>&   messages,
    SseCallbacks                  cbs,
    std::string_view              api_version = "2023-06-01",
    std::string_view              model_id    = "claude-sonnet-4-20250514",
    int                           max_tokens  = 8192,
    double                        temperature = 0.7) {

    ApiConfig cfg;
    cfg.base_url    = std::string(base_url);
    cfg.api_key     = std::string(api_key);
    cfg.api_version = std::string(api_version);
    cfg.model_id    = std::string(model_id);
    cfg.max_tokens  = max_tokens;
    cfg.temperature = temperature;

    SseClient client(std::move(cfg));

    std::vector<TextMessage> converted;
    converted.reserve(messages.size());
    for (const auto& m : messages) {
        TextMessage tm;
        if (m.role == "assistant")      tm.role = Role::Assistant;
        else if (m.role == "system")    tm.role = Role::System;
        else                            tm.role = Role::User;
        tm.content = m.text_content;
        converted.push_back(std::move(tm));
    }

    StreamingCallbacks internal;
    internal.on_sse_event = [cbs](SseEventType t, std::string_view data) {
        const char* name = "unknown";
        switch (t) {
            case SseEventType::MessageStart:      name = "message_start";      break;
            case SseEventType::ContentBlockStart: name = "content_block_start";break;
            case SseEventType::ContentBlockDelta: name = "content_block_delta";break;
            case SseEventType::ContentBlockStop:  name = "content_block_stop"; break;
            case SseEventType::MessageDelta:      name = "message_delta";      break;
            case SseEventType::MessageStop:       name = "message_stop";       break;
            case SseEventType::Error:             name = "error";              break;
            case SseEventType::Ping:              name = "ping";               break;
            case SseEventType::Unknown:           name = "";                   break;
        }
        if (name[0] && cbs.on_event) cbs.on_event(name);
        if (!data.empty() && cbs.on_data)  cbs.on_data(data);
    };
    internal.on_final =
        [cbs = std::move(cbs)](int http, std::string_view stop,
                               std::string_view /*accum*/, std::string_view err) {
            if (!err.empty() || (http != 0 && !(http >= 200 && http < 300))) {
                if (cbs.on_error) {
                    cbs.on_error(http,
                                 err.empty()
                                     ? std::string("HTTP ").append(std::to_string(http))
                                     : std::string(err));
                }
            } else if (cbs.on_complete) {
                cbs.on_complete();
            }
            (void)stop;
        };

    return client.PostMessagesStream(system_prompt, converted, std::move(internal));
}

}  // namespace cc::services::api::sse
