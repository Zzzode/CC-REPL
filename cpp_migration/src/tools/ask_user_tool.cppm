// AskUserTool - Asks the user questions and collects responses
module;
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.tools.ask_user;


export namespace cc::tools {

// Error types for ask user operations
enum class AskUserError {
    QuestionEmpty,
    Timeout,
    UserCancelled,
    InvalidOptions,
    CallbackFailed,
    TooManyOptions,
};

constexpr auto format_error(AskUserError err) -> std::string_view {
    switch (err) {
        case AskUserError::QuestionEmpty:   return "Question text is empty";
        case AskUserError::Timeout:         return "User response timed out";
        case AskUserError::UserCancelled:   return "User cancelled the prompt";
        case AskUserError::InvalidOptions:  return "Options list is invalid";
        case AskUserError::CallbackFailed:  return "Response callback invocation failed";
        case AskUserError::TooManyOptions:  return "Too many options provided (max 20)";
        default:                            return "Unknown ask user error";
    }
}

// Question structure representing a user prompt
struct Question {
    std::string text;                               // Primary question text
    std::optional<std::string> header;              // Optional header/title above the question
    std::vector<std::string> options;               // Selectable options (empty = free-form input)
    bool multi_select{false};                       // Allow multiple selections
    std::optional<std::string> default_answer;      // Pre-filled default answer
    std::optional<std::string> placeholder;         // Placeholder text for input
};

// User response structure
struct UserResponse {
    std::string text;                               // Raw text response
    std::vector<std::string> selected_options;      // Selected options (for option-based questions)
    bool is_default{false};                         // Whether default answer was used
    std::chrono::milliseconds response_time{0};     // Time taken to respond
};

// Callback type for collecting user responses
using ResponseCallback = std::function<std::expected<UserResponse, AskUserError>(const Question&)>;

// Configuration for ask user tool
struct AskUserConfig {
    std::chrono::seconds timeout{300};              // 5 minute default timeout
    bool allow_empty_response{false};               // Allow empty responses
    ResponseCallback callback;                      // Callback to collect response
};

// AskUserTool - main class for interactive user prompts
class AskUserTool {
public:
    static constexpr std::string_view name = "ask_user";
    static constexpr std::string_view description = "Ask the user a question and wait for their response";
    static constexpr size_t kMaxOptions = 20;
    static constexpr size_t kMaxQuestionLength = 10000;

    explicit AskUserTool(AskUserConfig config = {})
        : config_(std::move(config)) {}

    // Set the response callback
    void set_callback(ResponseCallback callback) {
        config_.callback = std::move(callback);
    }

    // Validate question before asking
    auto validate(const Question& question) const -> std::expected<void, AskUserError> {
        if (question.text.empty()) {
            return std::unexpected(AskUserError::QuestionEmpty);
        }

        if (question.options.size() > kMaxOptions) {
            return std::unexpected(AskUserError::TooManyOptions);
        }

        // Multi-select requires at least 2 options
        if (question.multi_select && question.options.size() < 2) {
            return std::unexpected(AskUserError::InvalidOptions);
        }

        // Validate default answer is in options list if options are provided
        if (question.default_answer && !question.options.empty()) {
            bool found = std::any_of(question.options.begin(), question.options.end(), [&](const auto& opt) {
                return opt == *question.default_answer;
            });
            if (!found) {
                return std::unexpected(AskUserError::InvalidOptions);
            }
        }
        return {};
    }

    // Main entry: ask a question and collect response
    auto execute(Question question) -> std::expected<UserResponse, AskUserError> {
        // Step 1: Validate the question
        if (auto result = validate(question); !result) {
            return std::unexpected(result.error());
        }

        // Step 2: Check callback is set
        if (!config_.callback) {
            return std::unexpected(AskUserError::CallbackFailed);
        }

        // Step 3: Invoke callback with timeout tracking
        auto start_time = std::chrono::steady_clock::now();

        auto response = config_.callback(question);
        if (!response) {
            // If callback failed and we have a default, use it
            if (question.default_answer) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time);
                return UserResponse{
                    .text = *question.default_answer,
                    .selected_options = {*question.default_answer},
                    .is_default = true,
                    .response_time = elapsed,
                };
            }
            return std::unexpected(response.error());
        }

        // Step 4: Validate response
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        response->response_time = elapsed;

        // Check timeout
        if (elapsed > std::chrono::duration_cast<std::chrono::milliseconds>(config_.timeout)) {
            if (question.default_answer) {
                return UserResponse{
                    .text = *question.default_answer,
                    .is_default = true,
                    .response_time = elapsed,
                };
            }
            return std::unexpected(AskUserError::Timeout);
        }

        // Check empty response
        if (!config_.allow_empty_response && response->text.empty()
            && response->selected_options.empty()) {
            if (question.default_answer) {
                response->text = *question.default_answer;
                response->is_default = true;
            }
        }

        return *response;
    }

    // Convenience: ask a yes/no question
    auto ask_yes_no(std::string_view question_text, bool default_yes = true)
        -> std::expected<bool, AskUserError>
    {
        Question q{
            .text = std::string(question_text),
            .options = {"Yes", "No"},
            .default_answer = default_yes ? "Yes" : "No",
        };

        auto response = execute(std::move(q));
        if (!response) return std::unexpected(response.error());
        return response->text == "Yes" || response->text == "yes" || response->text == "y";
    }

    // Generate JSON schema for LLM tool invocation
    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "question": {{ "type": "string", "description": "The question to ask the user" }},
      "options": {{ "type": "array", "items": {{ "type": "string" }}, "description": "Selectable options" }},
      "multi_select": {{ "type": "boolean", "description": "Allow multiple selections" }},
      "default_answer": {{ "type": "string", "description": "Default answer if user doesn't respond" }}
    }},
    "required": ["question"]
  }}
}})", name, description);
    }

private:
    AskUserConfig config_;
};

// --- Global responder for UI integration ---
//
// When the UI layer sets a global responder (e.g. a dialog-based one),
// ask_user_question tool invocations go through it instead of stdio.
// This enables faithful integration with the DialogQueue system.

using AskUserResponder = std::function<std::optional<std::string>(
    std::string_view question,
    std::optional<std::string> default_answer)>;

/// Set a global ask-user responder.
/// Returns the previous responder (for chaining / testing).
inline AskUserResponder set_global_ask_user_responder(AskUserResponder responder) {
    static AskUserResponder s_responder;
    auto prev = std::move(s_responder);
    s_responder = std::move(responder);
    return prev;
}

/// Get the current global ask-user responder (may be empty).
inline AskUserResponder& get_global_ask_user_responder() {
    static AskUserResponder s_responder;
    return s_responder;
}

} // namespace cc::tools
