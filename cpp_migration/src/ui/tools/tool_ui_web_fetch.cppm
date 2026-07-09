/// @file tool_ui_web_fetch.cppm
/// @brief WebFetch tool UI — userFacingName, renderToolUseMessage, etc.
///
/// MODULE:   cc.ui.tools.web_fetch
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/WebFetchTool/WebFetchTool.tsx
///   - userFacingName: "Fetch"
///   - renderToolUseMessage: hostname of the URL
///   - isTransparentWrapper: false
module;

#include <string>
#include <string_view>
#include <optional>
#include <cstddef>

export module cc.ui.tools.web_fetch;

import cc.ui.tools.registry;

export namespace cc::ui::tools::web_fetch_ui {

using namespace cc::ui::tools;

namespace detail {

/// Extract "url" field from input JSON.
[[nodiscard]] inline std::string extract_url(std::string_view input_json) {
    auto pos = input_json.find("\"url\"");
    if (pos == std::string_view::npos) return {};
    auto colon = input_json.find(':', pos);
    if (colon == std::string_view::npos) return {};
    auto quote = input_json.find('"', colon + 1);
    if (quote == std::string_view::npos) return {};
    auto end = input_json.find('"', quote + 1);
    if (end == std::string_view::npos) return {};
    return std::string(input_json.substr(quote + 1, end - quote - 1));
}

/// Extract hostname from a URL string.
/// e.g. "https://example.com/path" -> "example.com"
[[nodiscard]] inline std::string extract_hostname(std::string_view url) {
    // Strip scheme
    auto scheme_end = url.find("://");
    if (scheme_end != std::string_view::npos) {
        url = url.substr(scheme_end + 3);
    }
    // Take up to first slash
    auto slash = url.find('/');
    if (slash != std::string_view::npos) {
        url = url.substr(0, slash);
    }
    // Strip port
    auto port = url.find(':');
    if (port != std::string_view::npos) {
        url = url.substr(0, port);
    }
    return std::string{url};
}

/// Truncate URL for display (show host + first part of path).
[[nodiscard]] inline std::string display_url(std::string_view url,
                                              std::size_t max_len = 50) {
    if (url.size() <= max_len) return std::string{url};
    // Show scheme + host + start of path + ellipsis
    std::string hostname = extract_hostname(url);
    if (hostname.size() >= max_len) {
        return hostname.substr(0, max_len - 1) + "\xE2\x80\xA6";
    }
    // scheme + host + ellipsis
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return hostname + "\xE2\x80\xA6";
    }
    return std::string(url.substr(0, scheme_end + 3)) + hostname + "/\xE2\x80\xA6";
}

}  // namespace detail

// ============================================================
// WebFetch tool UI functions
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_web_fetch_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view) {
        return std::string{"Fetch"};
    };

    fns.message = [](std::string_view input_json) {
        std::string url = detail::extract_url(input_json);
        if (!url.empty()) {
            return detail::display_url(url, 50);
        }
        return std::string{"web page…"};
    };

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view partial_result) {
        std::string url = detail::extract_url(input_json);
        std::string hostname = detail::extract_hostname(url);
        if (!hostname.empty()) {
            if (!partial_result.empty()) {
                return "Reading content from " + hostname + "…";
            }
            return "Fetching " + hostname + "…";
        }
        return std::string{"Fetching web page…"};
    };

    fns.queued = [](std::string_view) {
        return std::string{"Waiting to fetch…"};
    };

    fns.is_transparent_wrapper = false;

    // TS REF: WebFetchTool — renderToolResultMessage shows the fetched page
    // content (markdown-converted HTML).  The output IS visible on screen,
    // so we index it for search.  No explicit extractSearchText in TS; this
    // is the faithful default for tools that render their output body.
    fns.extract_search_text = [](
        std::string_view output_text,
        std::string_view /*error_text*/) -> std::optional<std::string>
    {
        return std::string{output_text};
    };

    return fns;
}

/// Register WebFetch tool UI in the global registry.
inline void register_web_fetch_ui() {
    global_tool_ui_registry().register_tool_ui("web_fetch", make_web_fetch_ui());
    global_tool_ui_registry().register_tool_ui("WebFetch", make_web_fetch_ui());
    global_tool_ui_registry().register_tool_ui("WebFetchTool", make_web_fetch_ui());
}

}  // namespace cc::ui::tools::web_fetch_ui
