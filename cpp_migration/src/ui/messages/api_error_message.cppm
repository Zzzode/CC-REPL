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
    std::optional<double> retry_after_ms;     // Live countdown (display seconds)

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

    std::function<void()> on_retry;       // Retry button
    std::function<void()> on_diagnose;    // Diagnose button
    std::function<void()> on_dismiss;     // Dismiss button
    std::function<void(const std::string&)> on_copy_trace;   // Copy trace id
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
        if (e.retry_after_ms) {
            parts.push_back(text(std::format("  in {:.1f}s", *e.retry_after_ms / 1000.0))
                            | color(Color::Yellow));
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

    // --- Row 4: trace-id / request-id row (monospace, copyable) ---
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

    // --- Row 5: Retry / Diagnose / Dismiss action buttons ---
    if (opts.show_buttons) {
        Elements bts;
        bts.push_back(text(" "));
        if (opts.on_retry) {
            bts.push_back(pill_button("r", "Retry", Color::Green));
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

    return Renderer([s] { return RenderAPIError(s->opts); })
        | CatchEvent([s](Event event) -> bool {
              auto& o = s->opts;

              if (event == Event::Return || event == Event::Character(' ')) {
                  o.error.collapsed = !o.error.collapsed;
                  return true;
              }
              if (event == Event::Character('r') || event == Event::Character('R')) {
                  if (o.on_retry) { o.on_retry(); return true; }
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
                      if (!o.error.trace_id.empty())   joined += "trace-id=" + o.error.trace_id;
                      if (!o.error.request_id.empty()) {
                          if (!joined.empty()) joined += ", ";
                          joined += "request-id=" + o.error.request_id;
                      }
                      o.on_copy_trace(joined);
                      return true;
                  }
              }
              return false;
          });
}

} // namespace cc::ui::messages::api_error_message
