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

/// Convert a local filesystem path to a file:// URL.
///
/// TS REF: Node `url.pathToFileURL(p).href` used by
/// src/commands/terminalSetup/terminalSetup.tsx formatPathLink() L69.
/// POSIX: "file://" + absolute path, per-byte percent-encoded except
/// '/' and the empirically-verified Node v22 safe set
/// A-Za-z0-9 ! $ & ' ( ) * + , - . : ; = @ _
/// (note: '~' AND '[' ']' ARE encoded — %7E / %5B / %5D; non-ASCII bytes
/// are encoded as their raw UTF-8 bytes, e.g. 0xC3 0xA9 -> %C3%A9;
/// space -> %20, '#' -> %23, '?' -> %3F, '%' -> %25; '&' stays raw).
[[nodiscard]] inline std::string path_to_file_url(const fs::path& p) {
    std::error_code ec;
    fs::path ap = fs::absolute(p, ec);
    if (ec) ap = p;
    const std::string raw = ap.string();
    static constexpr char kHex[] = "0123456789ABCDEF";
    static constexpr std::string_view kSafe =
        "!$&'()*+,-.:;=@_";
    std::string url;
    url.reserve(raw.size() + 7);
    url += "file://";
    for (unsigned char c : raw) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9');
        if (c == '/' || unreserved ||
            kSafe.find(static_cast<char>(c)) != std::string_view::npos) {
            url.push_back(static_cast<char>(c));
        } else {
            url.push_back('%');
            url.push_back(kHex[c >> 4]);
            url.push_back(kHex[c & 0x0F]);
        }
    }
    return url;
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
    // TS REF: src/ink/supports-hyperlinks.ts. The supports-hyperlinks
    // library covers iTerm.app/Apple_Terminal/WezTerm/vscode/WT_SESSION and
    // VTE>=5000; on top of that TS whitelists additional terminals via
    // TERM_PROGRAM AND LC_TERMINAL (the latter survives inside tmux, which
    // overwrites TERM_PROGRAM), plus TERM containing "kitty".
    static constexpr std::string_view kAdditional[] = {
        "ghostty", "Hyper", "kitty", "alacritty", "iTerm.app", "iTerm2",
    };
    auto listed = [](const char* v) {
        if (!v) return false;
        std::string_view s(v);
        for (auto t : kAdditional)
            if (s == t) return true;
        return false;
    };

    // Detected by the underlying supports-hyperlinks-equivalent set.
    const char* term_program = std::getenv("TERM_PROGRAM");
    if (term_program) {
        std::string_view tp(term_program);
        if (tp == "iTerm.app" || tp == "Apple_Terminal" ||
            tp == "WezTerm" || tp == "vscode") return true;
    }
    if (std::getenv("WT_SESSION")) return true;

    const char* vte = std::getenv("VTE_VERSION");
    if (vte) {
        try {
            if (std::stoi(vte) >= 5000) return true;
        } catch (...) {}
    }

    // TS ADDITIONAL_HYPERLINK_TERMINALS via TERM_PROGRAM / LC_TERMINAL.
    if (listed(term_program)) return true;
    if (listed(std::getenv("LC_TERMINAL"))) return true;

    // Kitty identifies itself in TERM (e.g. xterm-kitty).
    if (const char* term = std::getenv("TERM");
        term && std::string_view(term).find("kitty") != std::string_view::npos) {
        return true;
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
    // TS REF: Node pathToFileURL via terminalSetup.tsx formatPathLink() L69 —
    // percent-encodes spaces/special chars so OSC8 URLs survive terminals that
    // split links at whitespace.
    std::string url = path_to_file_url(file);
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
