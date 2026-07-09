module;
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// Shell quoting for safe command construction.
#include <cctype>

export module cc.utils.hyperlink;

export namespace cc::utils {

namespace fs = std::filesystem;

// ============================================================
// Shell argument quoting (POSIX single-quote escape).
// ============================================================
namespace detail {

/// Quote a string for safe use as a single shell argument.
/// Uses POSIX single-quote wrapping with '\'' replacement for
/// embedded single quotes.  Mirrors cc::utils::bash::escape_shell_arg
/// but kept here self-contained so the hyperlink module has no
/// internal dependency on the bash execution module.
[[nodiscard]] inline std::string shell_quote(std::string_view arg) {
    std::string result;
    result += '\'';
    for (char c : arg) {
        if (c == '\'') {
            result += "'\\''";
        } else {
            result += c;
        }
    }
    result += '\'';
    return result;
}

} // namespace detail

// ============================================================
// Platform URL / file openers.
// ============================================================

/// Open a URL (http:// or https://) in the system default browser.
///
/// TS REF: src/utils/browser.ts openBrowser()
///   - macOS:   `open <url>`
///   - Linux:   `xdg-open <url>`  (respects $BROWSER when set)
///   - Windows: `rundll32 url.dll,FileProtocolHandler <url>`
///
/// Returns true if the command exited with status 0.
///
/// @param url  Must be an http(s) URL.  No protocol validation is
///             performed here — callers should route non-http URLs
///             through try_open_hyperlink() which dispatches by scheme.
[[nodiscard]] inline bool open_browser(const std::string& url) {
    // TS REF: browser.ts L46-64 — respects $BROWSER env var on non-Windows.
    const char* browser_env = std::getenv("BROWSER");
    const std::string quoted = detail::shell_quote(url);

#if defined(_WIN32)
    if (browser_env && *browser_env) {
        std::string cmd = std::string(browser_env) + " " + quoted;
        return std::system(cmd.c_str()) == 0;
    }
    std::string cmd = "rundll32 url.dll,FileProtocolHandler " + quoted;
    return std::system(cmd.c_str()) == 0;
#elif defined(__APPLE__)
    std::string cmd = browser_env && *browser_env
        ? std::string(browser_env) + " " + quoted
        : "open " + quoted;
    return std::system(cmd.c_str()) == 0;
#else
    std::string cmd = browser_env && *browser_env
        ? std::string(browser_env) + " " + quoted
        : "xdg-open " + quoted;
    return std::system(cmd.c_str()) == 0;
#endif
}

/// Open a local file path using the system default handler.
///
/// TS REF: src/utils/browser.ts openPath()
///   - macOS:   `open <path>`
///   - Linux:   `xdg-open <path>`
///   - Windows: `explorer <path>`
///
/// Returns true if the command exited with status 0.
[[nodiscard]] inline bool open_file_path(const fs::path& path) {
    const std::string quoted = detail::shell_quote(path.string());

#if defined(_WIN32)
    std::string cmd = "explorer " + quoted;
    return std::system(cmd.c_str()) == 0;
#elif defined(__APPLE__)
    std::string cmd = "open " + quoted;
    return std::system(cmd.c_str()) == 0;
#else
    std::string cmd = "xdg-open " + quoted;
    return std::system(cmd.c_str()) == 0;
#endif
}

// ============================================================
// file:// URL → filesystem path conversion.
// ============================================================

/// Convert a file:// URL to a local filesystem path.
///
/// TS REF: FullscreenLayout.tsx L630-667 — uses Node.js fileURLToPath(url).
///
/// Handles:
///   - `file:///absolute/path`       → `/absolute/path`
///   - `file://localhost/absolute/path` → `/absolute/path`
///   - `file:///path:line`           → `/path`  (line suffix stripped)
///
/// Returns std::nullopt for malformed URLs (instead of throwing, since
/// the TS code catches and silently ignores fileURLToPath errors).
[[nodiscard]] inline std::optional<fs::path> file_url_to_path(const std::string& url) {
    // Must start with "file:"
    if (!url.starts_with("file:")) return std::nullopt;

    std::string_view rest(url);
    rest.remove_prefix(5);  // strip "file:"

    // Strip "//" authority prefix.
    if (rest.starts_with("//")) {
        rest.remove_prefix(2);
        // Strip "localhost" authority.
        if (rest.starts_with("localhost/")) {
            rest.remove_prefix(9);  // "localhost"
        } else if (rest.starts_with("localhost")) {
            // file://localhost → empty path after authority (edge case)
            rest.remove_prefix(9);
        }
        // Other authorities (e.g. file://host/share) produce UNC paths;
        // for simplicity we keep the path as-is after stripping "//".
    }

    // Strip trailing ":line" suffix (e.g. file:///foo.txt:42).
    // Only strip if the suffix after the last ':' is all digits.
    std::string path_str(rest);
    auto last_colon = path_str.rfind(':');
    if (last_colon != std::string::npos && last_colon > 0) {
        bool all_digits = true;
        for (std::size_t i = last_colon + 1; i < path_str.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(path_str[i]))) {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            path_str = path_str.substr(0, last_colon);
        }
    }

    if (path_str.empty()) return std::nullopt;
    return fs::path(path_str);
}

// ============================================================
// Hyperlink click router (file: vs http:).
// ============================================================

/// Attempt to open a hyperlink URL, routing by scheme.
///
/// TS REF: FullscreenLayout.tsx L630-667
///   if (url.startsWith('file:')) {
///     try { void openPath(fileURLToPath(url)) } catch {}
///   } else {
///     void openBrowser(url)
///   }
///
/// - `file://` URLs are converted to paths and opened with the
///   system file handler (open / xdg-open / explorer).
/// - `http://` and `https://` URLs are opened in the browser.
/// - Other schemes are silently ignored (returns false) — matching
///   TS behavior where openBrowser rejects non-http(s) protocols.
///
/// Returns true if the open command succeeded.
[[nodiscard]] inline bool try_open_hyperlink(const std::string& url) {
    if (url.starts_with("file:")) {
        // TS REF: FullscreenLayout.tsx L633-637 — malformed file URLs
        // cause fileURLToPath to throw; caught and ignored silently.
        auto path = file_url_to_path(url);
        if (!path) return false;
        return open_file_path(*path);
    }

    // Only http/https for browser open (security: TS openBrowser validates
    // protocol via validateUrl() — rejects non-http(s)).
    if (url.starts_with("http://") || url.starts_with("https://")) {
        return open_browser(url);
    }

    // Unknown scheme — silently ignore.
    return false;
}

// ============================================================
// OSC 8 hyperlink generation (terminal-native support).
// ============================================================

/// Check if the terminal advertises OSC 8 hyperlink support.
///
/// Detection heuristics (same as make_hyperlink below):
///   - TERM_PROGRAM in {iTerm.app, WezTerm, vscode}
///   - WT_SESSION is set  (Windows Terminal)
///   - VTE_VERSION >= 5000  (GNOME Terminal, Tilix, etc.)
///
/// Note: FTXUI's `hyperlink(url)` decorator also emits OSC 8 sequences
/// unconditionally; this function is retained for callers that need to
/// decide whether to use OSC 8 or plain-text fallback in contexts where
/// raw bytes are written directly (e.g. statusline, tool output formatting).
bool supports_hyperlinks() {
    // iTerm2, WezTerm, Kitty, Windows Terminal support hyperlinks
    const char* term_program = std::getenv("TERM_PROGRAM");
    if (term_program) {
        std::string_view tp(term_program);
        if (tp == "iTerm.app" || tp == "WezTerm" || tp == "vscode") return true;
    }

    const char* wt_session = std::getenv("WT_SESSION");
    if (wt_session) return true;

    // Check VTE version (GNOME Terminal, etc.)
    const char* vte = std::getenv("VTE_VERSION");
    if (vte) {
        try {
            int ver = std::stoi(vte);
            if (ver >= 5000) return true;
        } catch (...) {}
    }

    return false;
}

/// Create an OSC 8 hyperlink (clickable in supported terminals).
///
/// When the terminal supports OSC 8, emits:
///   \e]8;;url\e\\text\e]8;;\e\\
///
/// Otherwise returns plain `text` with no escape sequences.
///
/// NOTE: When FTXUI's `hyperlink(url)` decorator is available, prefer
/// that — it registers the URL with the Screen so pixel-level click
/// detection works even when mouse tracking intercepts terminal-native
/// OSC 8 clicks.  This function is for non-FTXUI contexts (e.g. direct
/// stdout writes from tool output formatters).
std::string make_hyperlink(std::string_view url, std::string_view text) {
    if (!supports_hyperlinks()) {
        return std::string(text);
    }

    std::string result;
    result += "\x1b]8;;";
    result += url;
    result += "\x1b\\";
    result += text;
    result += "\x1b]8;;\x1b\\";
    return result;
}

/// Create a file:// hyperlink with optional line number.
///
/// Display text: filename[:line]
/// URL: file://<absolute-path>[:line]
std::string make_file_link(fs::path file, std::optional<int> line) {
    std::string url = "file://";
    url += fs::absolute(file).string();
    if (line.has_value()) {
        url += ":" + std::to_string(*line);
    }

    std::string display = file.filename().string();
    if (line.has_value()) {
        display += ":" + std::to_string(*line);
    }

    return make_hyperlink(url, display);
}

} // namespace cc::utils
