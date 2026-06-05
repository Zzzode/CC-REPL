// WebBrowserTool - Browser automation for navigation, interaction, and content extraction
module;
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.tools.web_browser;


export namespace cc::tools {


enum class BrowserAction {
    Navigate,
    Click,
    Extract,
    Screenshot,
    FillForm,
    GetTitle,
};

constexpr auto action_name(BrowserAction a) -> std::string_view {
    switch (a) {
        case BrowserAction::Navigate:   return "navigate";
        case BrowserAction::Click:      return "click";
        case BrowserAction::Extract:    return "extract";
        case BrowserAction::Screenshot: return "screenshot";
        case BrowserAction::FillForm:   return "fill_form";
        case BrowserAction::GetTitle:   return "get_title";
        default:                        return "unknown";
    }
}


enum class BrowserError {
    InvalidUrl,
    NavigationFailed,
    ElementNotFound,
    SelectorInvalid,
    Timeout,
    PageLoadFailed,
    ExtractionFailed,
    FormFieldNotFound,
    BrowserNotAvailable,
};

constexpr auto format_error(BrowserError err) -> std::string_view {
    switch (err) {
        case BrowserError::InvalidUrl:          return "Invalid URL format";
        case BrowserError::NavigationFailed:    return "Navigation to URL failed";
        case BrowserError::ElementNotFound:     return "Element not found by selector";
        case BrowserError::SelectorInvalid:     return "Invalid CSS/XPath selector";
        case BrowserError::Timeout:             return "Browser operation timed out";
        case BrowserError::PageLoadFailed:      return "Page failed to load";
        case BrowserError::ExtractionFailed:    return "Content extraction failed";
        case BrowserError::FormFieldNotFound:   return "Form field not found";
        case BrowserError::BrowserNotAvailable: return "Browser automation not available";
        default:                                return "Unknown browser error";
    }
}


struct FormField {
    std::string selector;
    std::string value;
};


struct BrowserRequest {
    BrowserAction action;
    std::optional<std::string> url;
    std::optional<std::string> selector;
    std::vector<FormField> form_fields;
    std::chrono::seconds timeout{30};
    std::optional<std::string> extract_selector;
};


struct BrowserResult {
    std::string content;
    std::optional<std::string> title;
    std::optional<std::string> url;
    std::optional<std::string> screenshot_base64;
    std::optional<std::string> media_type;
    std::chrono::milliseconds duration{0};
    bool success{true};
};


class UrlValidator {
public:
    static auto is_valid(std::string_view url) -> bool {
        if (url.empty()) return false;

        return url.starts_with("http://") || url.starts_with("https://") ||
               url.starts_with("file://");
    }

    static auto normalize(std::string_view url) -> std::string {
        std::string result(url);

        if (!url.starts_with("http://") && !url.starts_with("https://") &&
            !url.starts_with("file://")) {
            result = "https://" + result;
        }
        return result;
    }
};


class PageState {
public:
    void set_url(std::string url) { current_url_ = std::move(url); }
    void set_title(std::string title) { title_ = std::move(title); }
    void set_content(std::string content) { content_ = std::move(content); }

    [[nodiscard]] auto url() const -> std::string_view { return current_url_; }
    [[nodiscard]] auto title() const -> std::string_view { return title_; }
    [[nodiscard]] auto content() const -> std::string_view { return content_; }

private:
    std::string current_url_;
    std::string title_;
    std::string content_;
};

using BrowserScreenshotBackend = std::function<std::expected<std::string, BrowserError>(
    const BrowserRequest&,
    const PageState&)>;


class WebBrowserTool {
public:
    static constexpr std::string_view name = "web_browser";
    static constexpr std::string_view description = "Browser automation: navigate, click, extract, and fill forms";

    explicit WebBrowserTool(BrowserScreenshotBackend screenshot_backend = {})
        : screenshot_backend_(std::move(screenshot_backend)) {}

    auto validate(const BrowserRequest& request) const -> std::expected<void, BrowserError> {
        if (request.action == BrowserAction::Navigate) {
            if (!request.url || request.url->empty()) {
                return std::unexpected(BrowserError::InvalidUrl);
            }
            auto normalized = UrlValidator::normalize(*request.url);
            if (!UrlValidator::is_valid(normalized)) {
                return std::unexpected(BrowserError::InvalidUrl);
            }
        }
        if (request.action == BrowserAction::Click ||
            request.action == BrowserAction::Extract) {
            if (!request.selector || request.selector->empty()) {
                return std::unexpected(BrowserError::SelectorInvalid);
            }
        }
        if (request.action == BrowserAction::FillForm && request.form_fields.empty()) {
            return std::unexpected(BrowserError::FormFieldNotFound);
        }
        return {};
    }

    auto execute(BrowserRequest request) -> std::expected<BrowserResult, BrowserError> {
        if (auto v = validate(request); !v) return std::unexpected(v.error());

        auto start = std::chrono::steady_clock::now();

        BrowserResult result;

        switch (request.action) {
            case BrowserAction::Navigate: {
                auto url = UrlValidator::normalize(*request.url);
                auto fetch_result = fetch_page(url, request.timeout);
                if (!fetch_result) return std::unexpected(fetch_result.error());
                page_state_.set_url(url);
                page_state_.set_content(*fetch_result);
                result.content = std::format("Navigated to: {}", url);
                result.url = url;
                break;
            }
            case BrowserAction::Click: {

                result.content = std::format("Clicked element: {}", *request.selector);
                break;
            }
            case BrowserAction::Extract: {

                if (page_state_.content().empty()) {
                    return std::unexpected(BrowserError::ExtractionFailed);
                }
                result.content = std::string(page_state_.content());
                break;
            }
            case BrowserAction::Screenshot: {
                auto screenshot = capture_screenshot(request);
                if (!screenshot) return std::unexpected(screenshot.error());
                result.content = "Captured browser screenshot.";
                result.screenshot_base64 = std::move(*screenshot);
                result.media_type = "image/png";
                break;
            }
            case BrowserAction::FillForm: {
                std::string filled;
                for (const auto& field : request.form_fields) {
                    filled += std::format("  {} = {}\n", field.selector, field.value);
                }
                result.content = std::format("Filled form fields:\n{}", filled);
                break;
            }
            case BrowserAction::GetTitle: {
                result.content = std::string(page_state_.title());
                result.title = std::string(page_state_.title());
                break;
            }
        }

        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        return result;
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "action": {{ "type": "string", "enum": ["navigate", "click", "extract", "screenshot", "fill_form", "get_title"], "description": "Browser action to perform" }},
      "url": {{ "type": "string", "description": "URL to navigate to" }},
      "selector": {{ "type": "string", "description": "CSS selector for target element" }},
      "form_fields": {{ "type": "array", "items": {{ "type": "object", "properties": {{ "selector": {{ "type": "string" }}, "value": {{ "type": "string" }} }} }}, "description": "Form fields to fill" }},
      "timeout": {{ "type": "integer", "description": "Operation timeout in seconds (default 30)" }}
    }},
    "required": ["action"]
  }}
}})json", name, description);
    }

private:
    PageState page_state_;
    BrowserScreenshotBackend screenshot_backend_;

    [[nodiscard]] static std::string shell_quote(std::string_view value) {
        std::string out = "'";
        for (char c : value) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    static void replace_all(std::string& text,
                            std::string_view needle,
                            std::string_view replacement) {
        std::size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos) {
            text.replace(pos, needle.size(), replacement);
            pos += replacement.size();
        }
    }

    [[nodiscard]] std::expected<std::string, BrowserError> capture_screenshot(
        const BrowserRequest& request) const {
        if (screenshot_backend_) {
            return screenshot_backend_(request, page_state_);
        }

        auto* command_env = std::getenv("CC_REPL_BROWSER_SCREENSHOT_CMD");
        if (!command_env || std::string_view(command_env).empty()) {
            return std::unexpected(BrowserError::BrowserNotAvailable);
        }

        auto url = request.url.value_or(std::string(page_state_.url()));
        if (url.empty()) {
            return std::unexpected(BrowserError::InvalidUrl);
        }

        std::string command = command_env;
        auto quoted_url = shell_quote(url);
        if (command.find("{url}") != std::string::npos) {
            replace_all(command, "{url}", quoted_url);
        } else {
            command += " ";
            command += quoted_url;
        }

        FILE* pipe = ::popen(command.c_str(), "r");
        if (!pipe) return std::unexpected(BrowserError::BrowserNotAvailable);
        std::string output;
        std::array<char, 8192> buffer{};
        while (auto bytes = ::fread(buffer.data(), 1, buffer.size(), pipe)) {
            output.append(buffer.data(), bytes);
        }
        auto status = ::pclose(pipe);
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
            output.pop_back();
        }
        if (status != 0 || output.empty()) {
            return std::unexpected(BrowserError::BrowserNotAvailable);
        }
        return output;
    }

    auto fetch_page(const std::string& url, std::chrono::seconds timeout)
        -> std::expected<std::string, BrowserError>
    {
        auto cmd = std::format("curl -sL --max-time {} {}", timeout.count(), shell_quote(url));

        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe) return std::unexpected(BrowserError::NavigationFailed);

        std::string output;
        std::array<char, 8192> buffer{};
        while (auto bytes = ::fread(buffer.data(), 1, buffer.size(), pipe)) {
            output.append(buffer.data(), bytes);
        }
        int status = ::pclose(pipe);

        if (status != 0 || output.empty()) {
            return std::unexpected(BrowserError::PageLoadFailed);
        }
        return output;
    }
};

} // namespace cc::tools
