/// @file api_error_message.cppm
/// @brief API-level error card — severity-coded border colors, error code,
/// message, monospace copyable trace-id, optional retry countdown, and
/// Retry / Diagnose / Dismiss action buttons.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.api_error_message;

export namespace cc::ui::messages::api_error_message {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Severity drives border + accent color. Integer HTTP codes also influence
/// the final color (see resolve_border_color()).
enum class ErrorSeverity : std::uint8_t {
    Warning,   // e.g. 401 / 403 — orange family
    Error,     // e.g. 4xx other — red
    Critical,  // e.g. 5xx, transport — deep red
};

/// The canonical API error payload for rendering.
struct APIErrorData {
    std::optional<int> http_status;           // e.g. 401 / 500
    std::string provider;                     // "Anthropic", "OpenAI", etc.
    std::string error_code;                   // e.g. "auth_error", "rate_limit"
    std::string message;                      // Human-readable message
    std::string trace_id;                     // x-request-id / x-amzn-trace-id
    std::string request_id;                   // Secondary id (may be empty)

    ErrorSeverity severity{ErrorSeverity::Error};

    // Retry info
    std::optional<int> current_attempt;       // N of M
    std::optional<int> max_attempts;
    std::optional<double> retry_after_ms;     // Total backoff duration (ms)
    /// TS REF: SystemAPIErrorMessage.tsx — sessionExpired prop.  When true,
    /// the authentication session has expired and the user must clear /
    /// re-authenticate rather than just retry.
    bool session_expired{false};

    // Oversized-body warning
    std::optional<std::size_t> response_bytes;
    bool truncated{false};

    // Expand / collapse long message
    bool collapsed{true};
    std::size_t max_chars_collapsed{1000};    // Max chars in collapsed body
};

/// Component options
struct APIErrorOptions {
    APIErrorData error;
    bool show_trace_ids{true};
    bool show_buttons{true};
    bool dismissible{true};
    bool copyable_trace{true};

    std::function<void()> on_retry;         // Retry button — re-send last user message
    std::function<void()> on_diagnose;      // Diagnose button
    std::function<void()> on_dismiss;       // Dismiss button
    std::function<void(const std::string&)> on_copy_trace;   // Copy trace id
    /// TS REF: SystemAPIErrorMessage.tsx — onClearSession prop.  Called when
    /// the user clicks "Clear session" on a session-expired error card.
    std::function<void()> on_clear_session;
    /// When the error was first displayed (for live countdown).  Set by the
    /// interactive component at construction time; static renders may leave
    /// it unset (shows raw retry_after_ms value instead).
    std::optional<std::chrono::steady_clock::time_point> error_start_time;
};

// ============================================================
// Color helpers
// ============================================================

/// Resolve effective severity from explicit value + HTTP status hints.
/// 5xx -> Critical, 401/403 -> Warning, other 4xx -> Error.
[[nodiscard]] inline ErrorSeverity resolve_severity(const APIErrorData& e) {
    if (e.http_status) {
        int s = *e.http_status;
        if (s >= 500) return ErrorSeverity::Critical;
        if (s == 401 || s == 403) return ErrorSeverity::Warning;
        if (s >= 400) return ErrorSeverity::Error;
    }
    return e.severity;
}

struct ErrorPalette {
    Color border;
    Color accent;
    Color title;
    Color body_bg;
    const char* badge;
};

[[nodiscard]] inline ErrorPalette palette(const APIErrorData& e) {
    switch (resolve_severity(e)) {
        case ErrorSeverity::Warning:
            return {Color::Orange1, Color::Orange1, Color::Yellow,
                    Color::RGB(40, 25, 0), "⚠"};
        case ErrorSeverity::Critical:
            return {Color::DeepPink1Bis, Color::Red1, Color::Red1,
                    Color::RGB(40, 0, 10), "✖"};
        case ErrorSeverity::Error:
        default:
            return {Color::Red, Color::Red, Color::RedLight,
                    Color::RGB(40, 0, 0), "✗"};
    }
}

// ============================================================
// Rendering
// ============================================================

/// Render a single "pill" button: [<key> Label]
[[nodiscard]] inline Element pill_button(const std::string& key_hint,
                                          const std::string& label,
                                          Color c = Color::White) {
    return hbox({
        text("[") | dim,
        text(key_hint) | color(c) | bold,
        text(" " + label + "]") | dim,
    }) | color(c);
}

/// Render the full error card (static element view)
[[nodiscard]] inline Element RenderAPIError(const APIErrorOptions& opts) {
    const auto& e = opts.error;
    auto p = palette(e);

    Elements rows;

    // --- Row 1: status code + provider + severity badge ---
    {
        Elements parts;
        parts.push_back(text(std::string(" ") + p.badge + " ") | bold | color(p.accent));
        if (e.http_status) {
            parts.push_back(
                text(std::format("HTTP {}", *e.http_status)) | bold | color(p.title));
            parts.push_back(text("  ·  ") | dim);
        } else {
            parts.push_back(text("ERROR") | bold | color(p.title));
            parts.push_back(text("  ·  ") | dim);
        }
        if (!e.provider.empty()) parts.push_back(text(e.provider) | dim);
        if (!e.error_code.empty()) {
            if (!e.provider.empty()) parts.push_back(text(" · ") | dim);
            parts.push_back(text(e.error_code) | color(p.accent) | bold);
        }
        parts.push_back(filler());
        if (e.current_attempt && e.max_attempts) {
            parts.push_back(text(std::format(" attempt {}/{}",
                                             *e.current_attempt, *e.max_attempts))
                            | dim | color(p.title));
        }
        rows.push_back(hbox(parts));
    }

    // --- Row 2: separator ---
    rows.push_back(separator());

    // --- Row 3: error message (collapsed / expanded body) ---
    {
        std::string body = e.message.empty()
            ? std::string("(no error message from provider)")
            : e.message;
        if (e.collapsed && body.size() > e.max_chars_collapsed) {
            body.resize(e.max_chars_collapsed);
            body += std::format("… ({} chars hidden, press Enter to expand)",
                                e.message.size() - e.max_chars_collapsed);
        }
        rows.push_back(paragraph(" " + body) | color(Color::GrayLight));

        if (e.truncated || e.response_bytes) {
            Elements warn;
            warn.push_back(text(" "));
            if (e.response_bytes) {
                warn.push_back(text(std::format("response body: {} bytes",
                                                *e.response_bytes)) | dim);
            }
            if (e.truncated) {
                if (e.response_bytes) warn.push_back(text(" · ") | dim);
                warn.push_back(text("truncated preview") | color(p.accent) | dim);
            }
            rows.push_back(hbox(warn));
        }
    }

    // --- Row 3b: retry countdown text (TS REF: SystemAPIErrorMessage.tsx L106) ---
    // Shows "Retrying in X seconds… (attempt N/M)" when retry_after_ms is set.
    // The live countdown is computed from error_start_time + retry_after_ms.
    if (e.retry_after_ms && e.current_attempt && e.max_attempts) {
        // Compute remaining seconds from the start time stored in error data.
        // When start_time is not set (static render path), show raw retry_after_ms.
        // TS REF: SystemAPIErrorMessage.tsx L43-50  retryInSecondsLive = max(0, round((retryInMs - countdownMs) / 1000))
        double remaining_sec = 0.0;
        if (opts.error_start_time) {
            auto now = std::chrono::steady_clock::now();
            double elapsed_ms = std::chrono::duration<double, std::milli>(
                now - *opts.error_start_time).count();
            remaining_sec = std::max(0.0,
                (*e.retry_after_ms - elapsed_ms) / 1000.0);
        } else {
            remaining_sec = *e.retry_after_ms / 1000.0;
        }
        int secs_int = static_cast<int>(std::round(remaining_sec));
        // TS REF: L103  retryInSecondsLive === 1 ? "second" : "seconds"
        std::string unit = (secs_int == 1) ? "second" : "seconds";
        std::string countdown_text = std::format(
            " Retrying in {} {}… (attempt {}/{})",
            secs_int, unit, *e.current_attempt, *e.max_attempts);
        rows.push_back(text(countdown_text) | dim | color(Color::GrayLight));
    }
    if (opts.show_trace_ids) {
        Elements ids;
        auto add_id = [&](const char* label, const std::string& val) {
            if (val.empty()) return;
            ids.push_back(text(std::format(" {}: ", label)) | dim);
            ids.push_back(text(val) | color(Color::CyanLight) | bold);
        };
        add_id("trace-id",   e.trace_id);
        add_id("request-id", e.request_id);
        if (!ids.empty()) {
            rows.push_back(separator());
            ids.insert(ids.begin(), text(" ") | dim);
            if (opts.copyable_trace) {
                ids.push_back(filler());
                ids.push_back(text("[t] copy ") | dim | color(Color::GrayDark));
            }
            rows.push_back(hbox(ids));
        }
    }

    // --- Row 5: Retry / Diagnose / Dismiss / Clear session action buttons ---
    if (opts.show_buttons) {
        Elements bts;
        bts.push_back(text(" "));
        if (opts.on_retry && !e.session_expired) {
            bts.push_back(pill_button("r", "Retry", Color::Green));
            bts.push_back(text("  "));
        }
        if (opts.on_clear_session && e.session_expired) {
            // TS REF: sessionExpired → show "Clear session" instead of plain Retry
            bts.push_back(pill_button("c", "Clear session", Color::Yellow));
            bts.push_back(text("  "));
        }
        if (opts.on_diagnose) {
            bts.push_back(pill_button("d", "Diagnose", Color::BlueLight));
            bts.push_back(text("  "));
        }
        if (opts.dismissible && opts.on_dismiss) {
            bts.push_back(pill_button("q", "Dismiss", Color::GrayLight));
        }
        bts.push_back(filler());
        bts.push_back(text("enter: expand ") | dim | color(Color::GrayDark));
        rows.push_back(separator());
        rows.push_back(hbox(bts));
    }

    return vbox(rows) | bgcolor(p.body_bg)
                      | borderRounded
                      | color(p.border);
}

// ============================================================
// Interactive Component
// ============================================================

[[nodiscard]] inline Component APIErrorMessage(APIErrorOptions options) {
    struct State {
        APIErrorOptions opts;
    };
    auto s = std::make_shared<State>();
    s->opts = std::move(options);
    // TS REF: SystemAPIErrorMessage.tsx L28  useState(0) for countdownMs.
    // We capture the start time so the renderer can compute live remaining
    // seconds (remaining = retry_after_ms - (now - error_start_time)).
    if (!s->opts.error_start_time) {
        s->opts.error_start_time = std::chrono::steady_clock::now();
    }

    return Renderer([s] { return RenderAPIError(s->opts); })
        | CatchEvent([s](Event event) -> bool {
              auto& o = s->opts;
              auto& e = o.error;

              if (event == Event::Return || event == Event::Character(' ')) {
                  e.collapsed = !e.collapsed;
                  return true;
              }
              if (event == Event::Character('r') || event == Event::Character('R')) {
                  if (o.on_retry && !e.session_expired) {
                      o.on_retry();
                      return true;
                  }
              }
              // TS REF: onClearSession — session-expired errors show "Clear session"
              if (event == Event::Character('c') || event == Event::Character('C')) {
                  if (o.on_clear_session && e.session_expired) {
                      o.on_clear_session();
                      return true;
                  }
              }
              if (event == Event::Character('d') || event == Event::Character('D')) {
                  if (o.on_diagnose) { o.on_diagnose(); return true; }
              }
              if (event == Event::Character('q') || event == Event::Character('Q') ||
                  event == Event::Escape) {
                  if (o.dismissible && o.on_dismiss) {
                      o.on_dismiss();
                      return true;
                  }
              }
              if (event == Event::Character('t') || event == Event::Character('T')) {
                  if (o.copyable_trace && o.on_copy_trace) {
                      std::string joined;
                      if (!e.trace_id.empty())   joined += "trace-id=" + e.trace_id;
                      if (!e.request_id.empty()) {
                          if (!joined.empty()) joined += ", ";
                          joined += "request-id=" + e.request_id;
                      }
                      o.on_copy_trace(joined);
                      return true;
                  }
              }
              return false;
          });
}

} // namespace cc::ui::messages::api_error_message
