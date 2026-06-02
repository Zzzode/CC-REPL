module;
#include <span>
#include <string>
#include <string_view>
export module cc.services.auto_dream.consolidation_prompt;

export namespace cc::services::auto_dream {

// Generate the consolidation prompt from memory contents
auto get_consolidation_prompt(std::span<const std::string> memory_contents) -> std::string {
    std::string prompt =
        "You are consolidating memory from multiple sessions. "
        "Analyze the following memory entries and produce a consolidated summary "
        "that preserves key learnings, preferences, and important context:\n\n";

    for (size_t i = 0; i < memory_contents.size(); ++i) {
        prompt += "--- Memory Entry " + std::to_string(i + 1) + " ---\n";
        prompt += memory_contents[i] + "\n\n";
    }

    prompt += "Produce a concise, deduplicated consolidation that retains all "
              "important information while removing redundancy.";
    return prompt;
}

// Generate dream analysis prompt for a session summary
auto get_dream_analysis_prompt(std::string_view session_summary) -> std::string {
    return std::string(
        "Analyze this session summary and extract key patterns, "
        "recurring themes, and important learnings:\n\n") +
        std::string(session_summary) +
        "\n\nProvide structured insights that would help in future sessions.";
}

// Format the consolidation result for storage
auto format_consolidation_result(std::string_view analysis) -> std::string {
    return std::string("## Consolidated Memory\n\n") + std::string(analysis);
}

} // namespace cc::services::auto_dream
