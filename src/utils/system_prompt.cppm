module;

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.system_prompt;

export namespace cc::utils {

// ─── System Prompt Type ──────────────────────────────────────────────────────

/// A branded system prompt type — a readonly vector of prompt sections.
/// Equivalent to TS branded `SystemPrompt = readonly string[] & { __brand }`.
class SystemPrompt {
public:
    SystemPrompt() = default;

    /// Construct from a vector of sections
    static SystemPrompt from_sections(std::vector<std::string> sections);

    /// Get the prompt sections
    const std::vector<std::string>& sections() const { return sections_; }

    /// Check if empty
    bool empty() const { return sections_.empty(); }

    /// Number of sections
    size_t size() const { return sections_.size(); }

    /// Concatenate all sections with newline separator
    std::string joined(std::string_view separator = "\n") const;

    /// Access individual section
    const std::string& operator[](size_t index) const { return sections_[index]; }

    /// Iterators
    auto begin() const { return sections_.begin(); }
    auto end() const { return sections_.end(); }

private:
    std::vector<std::string> sections_;
};

/// Create a SystemPrompt from a vector of section strings (factory function)
SystemPrompt as_system_prompt(std::vector<std::string> value);

// ─── Prompt Section Inclusion ────────────────────────────────────────────────

/// Predicate for conditional section inclusion
using SectionPredicate = std::function<bool()>;

/// A prompt section that may be conditionally included
struct ConditionalSection {
    std::string content;
    SectionPredicate condition;  // If null/empty, always included
};

/// Build a prompt from conditional sections, including only those
/// whose predicates return true (or have no predicate)
std::vector<std::string> resolve_conditional_sections(
    const std::vector<ConditionalSection>& sections);

// ─── Agent Definition (minimal interface for prompt building) ────────────────

/// Minimal agent definition interface for system prompt building
struct AgentPromptInfo {
    std::string name;
    bool is_built_in = false;
    std::optional<std::string> custom_system_prompt;
};

/// Tool use context options relevant to prompt assembly
struct PromptOptions {
    bool coordinator_mode = false;
    bool proactive_mode = false;
};

// ─── Build Effective System Prompt ───────────────────────────────────────────

/// Configuration for building the effective system prompt
struct BuildSystemPromptConfig {
    std::optional<AgentPromptInfo> agent_definition;
    PromptOptions options;
    std::optional<std::string> custom_system_prompt;
    std::vector<std::string> default_system_prompt;
    std::optional<std::string> append_system_prompt;
    std::optional<std::string> override_system_prompt;
};

/// Configuration without the optional override prompt
struct BuildSystemPromptConfigBase {
    std::optional<AgentPromptInfo> agent_definition;
    PromptOptions options;
    std::optional<std::string> custom_system_prompt;
    std::vector<std::string> default_system_prompt;
    std::optional<std::string> append_system_prompt;
};

/**
 * Builds the effective system prompt array based on priority:
 * 0. Override system prompt (if set, replaces all other prompts)
 * 1. Coordinator system prompt (if coordinator mode is active)
 * 2. Agent system prompt (if agent definition is set)
 *    - In proactive mode: agent prompt is APPENDED to default
 *    - Otherwise: agent prompt REPLACES default
 * 3. Custom system prompt (if specified via --system-prompt)
 * 4. Default system prompt (the standard prompt)
 *
 * append_system_prompt is always added at the end (except when override is set).
 */
SystemPrompt build_effective_system_prompt(const BuildSystemPromptConfig& config);

/// Overload without override_system_prompt field
SystemPrompt build_effective_system_prompt(const BuildSystemPromptConfigBase& config);

} // namespace cc::utils
