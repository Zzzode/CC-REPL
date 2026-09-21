module;
#include <string>
export module cc.services.compact.prompt;

export namespace cc::services::compact {

// Get the standard compact prompt for full summarization
auto get_compact_prompt() -> std::string {
    return "Please provide a concise summary of the conversation so far, "
           "preserving key decisions, code changes, file paths, and any "
           "unresolved issues. Focus on information needed to continue "
           "the task effectively.";
}

// Get the micro-compact prompt for lightweight trimming
auto get_micro_compact_prompt() -> std::string {
    return "Briefly summarize the older messages, keeping only essential "
           "context needed for the current task.";
}

// Format compact instruction with token budget information
auto format_compact_instruction(int target_tokens, int current_tokens) -> std::string {
    return "Context window is at " + std::to_string(current_tokens) + " tokens. "
           "Please compact to approximately " + std::to_string(target_tokens) + " tokens. "
           "Preserve: file paths, code snippets in progress, key decisions, "
           "and current task state.";
}

} // namespace cc::services::compact
