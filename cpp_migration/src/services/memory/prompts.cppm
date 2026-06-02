module;
#include <span>
#include <string>
#include <string_view>
export module cc.services.memory.prompts;

export namespace cc::services::memory {

// Get the prompt used to extract memories from conversation
auto get_memory_extraction_prompt() -> std::string {
    return "Extract key facts, preferences, decisions, and important context "
           "from this conversation that would be useful to remember in future "
           "sessions. Focus on:\n"
           "- User preferences and working style\n"
           "- Project-specific knowledge\n"
           "- Key decisions and their rationale\n"
           "- File paths and architecture details\n"
           "Format as concise bullet points.";
}

// Get the prompt to assess memory relevance to a query
auto get_memory_relevance_prompt(std::string_view query, std::span<const std::string> memories)
    -> std::string {
    std::string prompt = "Given the following query:\n\"" + std::string(query) + "\"\n\n"
                         "Rate the relevance of each memory (0-1):\n";
    for (size_t i = 0; i < memories.size(); ++i) {
        prompt += std::to_string(i + 1) + ". " + memories[i] + "\n";
    }
    return prompt;
}

// Format memories for inclusion in conversation context
auto format_memory_for_context(std::span<const std::string> memories) -> std::string {
    if (memories.empty()) {
        return "";
    }
    std::string result = "<memory>\n";
    for (const auto& memory : memories) {
        result += "- " + memory + "\n";
    }
    result += "</memory>";
    return result;
}

} // namespace cc::services::memory
