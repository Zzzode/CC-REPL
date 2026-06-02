/// @file context_analysis.cppm
/// @brief Context window analysis, token usage tracking, context suggestions, query context building
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <unordered_map>
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <functional>

export module cc.utils.context_analysis;

export namespace cc::utils::context_analysis {

// ---------------------------------------------------------------------------
// Context window constants
// ---------------------------------------------------------------------------

/// Default model context window size (200k tokens)
inline constexpr std::size_t MODEL_CONTEXT_WINDOW_DEFAULT = 200'000;

/// Maximum output tokens for compact operations
inline constexpr std::size_t COMPACT_MAX_OUTPUT_TOKENS = 20'000;

/// Capped default for slot-reservation optimization
inline constexpr std::size_t CAPPED_DEFAULT_MAX_TOKENS = 8'000;

/// Escalated max tokens after hitting the cap
inline constexpr std::size_t ESCALATED_MAX_TOKENS = 64'000;

// ---------------------------------------------------------------------------
// Token statistics from context analysis
// ---------------------------------------------------------------------------

/// Per-tool token breakdown
struct ToolTokenEntry {
    std::string tool_name;
    std::size_t call_tokens{0};
    std::size_t result_tokens{0};
};

/// Duplicate file read info
struct DuplicateFileRead {
    std::string file_path;
    std::size_t count{0};
    std::size_t tokens{0};
};

/// Token statistics for context analysis
struct TokenStats {
    std::unordered_map<std::string, std::size_t> tool_requests;
    std::unordered_map<std::string, std::size_t> tool_results;
    std::size_t human_messages{0};
    std::size_t assistant_messages{0};
    std::size_t local_command_outputs{0};
    std::size_t other{0};
    std::unordered_map<std::string, std::size_t> attachments;
    std::vector<DuplicateFileRead> duplicate_file_reads;
    std::size_t total{0};
};

// ---------------------------------------------------------------------------
// Context data for suggestions engine
// ---------------------------------------------------------------------------

/// Message breakdown entry for tool calls
struct ToolCallBreakdown {
    std::string tool_name;
    std::size_t call_tokens{0};
    std::size_t result_tokens{0};
    std::size_t call_count{0};
};

/// Full message breakdown for context analysis
struct MessageBreakdown {
    std::vector<ToolCallBreakdown> tool_calls_by_type;
    std::size_t human_tokens{0};
    std::size_t assistant_tokens{0};
    std::size_t system_tokens{0};
    std::size_t attachment_tokens{0};
};

/// Context data used for generating suggestions
struct ContextData {
    std::size_t raw_max_tokens{0};
    std::size_t used_tokens{0};
    double percentage{0.0};
    bool is_auto_compact_enabled{true};
    std::optional<MessageBreakdown> message_breakdown;
    std::size_t memory_tokens{0};
};

// ---------------------------------------------------------------------------
// Context suggestions
// ---------------------------------------------------------------------------

/// Severity level for context suggestions
enum class SuggestionSeverity : std::uint8_t {
    Info,
    Warning,
};

/// A suggestion for optimizing context usage
struct ContextSuggestion {
    SuggestionSeverity severity{SuggestionSeverity::Info};
    std::string title;
    std::string detail;
    std::optional<std::size_t> savings_tokens;
};

/// Generate context optimization suggestions based on current state
[[nodiscard]] auto generate_context_suggestions(const ContextData& data)
    -> std::vector<ContextSuggestion>;

// ---------------------------------------------------------------------------
// Context window analysis
// ---------------------------------------------------------------------------

/// Context usage percentages
struct ContextPercentages {
    std::optional<double> used;
    std::optional<double> remaining;
};

/// Token usage data from API response
struct TokenUsage {
    std::size_t input_tokens{0};
    std::size_t cache_creation_input_tokens{0};
    std::size_t cache_read_input_tokens{0};
};

/// Calculate context window usage percentage from token usage data
[[nodiscard]] auto calculate_context_percentages(
    std::optional<TokenUsage> current_usage,
    std::size_t context_window_size) -> ContextPercentages;

/// Check if 1M context is available for a given model
[[nodiscard]] auto has_1m_context(std::string_view model) -> bool;

/// Check if a model natively supports 1M context
[[nodiscard]] auto model_supports_1m(std::string_view model) -> bool;

/// Get the context window size for a model
[[nodiscard]] auto get_context_window_for_model(
    std::string_view model,
    const std::vector<std::string>& betas = {}) -> std::size_t;

// ---------------------------------------------------------------------------
// Model output token limits
// ---------------------------------------------------------------------------

/// Max output token limits for a model
struct ModelMaxOutputTokens {
    std::size_t default_tokens{0};
    std::size_t upper_limit{0};
};

/// Returns the model's default and upper limit for max output tokens
[[nodiscard]] auto get_model_max_output_tokens(std::string_view model)
    -> ModelMaxOutputTokens;

/// Returns the max thinking budget tokens for a given model
[[nodiscard]] auto get_max_thinking_tokens_for_model(std::string_view model)
    -> std::size_t;

// ---------------------------------------------------------------------------
// Context analysis functions
// ---------------------------------------------------------------------------

/// Analyze token distribution across messages
[[nodiscard]] auto analyze_context(
    const std::vector<std::string>& serialized_messages)
    -> std::expected<TokenStats, std::string>;

// ---------------------------------------------------------------------------
// Query context building
// ---------------------------------------------------------------------------

/// System prompt parts for API cache-key prefix
struct SystemPromptParts {
    std::vector<std::string> default_system_prompt;
    std::unordered_map<std::string, std::string> user_context;
    std::unordered_map<std::string, std::string> system_context;
};

/// Fetch the system prompt parts that form the API cache-key prefix
[[nodiscard]] auto fetch_system_prompt_parts(
    std::string_view main_loop_model,
    const std::vector<std::string>& additional_working_directories,
    std::optional<std::string_view> custom_system_prompt = std::nullopt)
    -> std::expected<SystemPromptParts, std::string>;

} // namespace cc::utils::context_analysis
