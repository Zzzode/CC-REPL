module;
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
export module cc.services.memory.session_memory_utils;

export namespace cc::services::memory {

// Extract key points from a conversation text
auto extract_key_points(std::string_view conversation) -> std::vector<std::string> {
    std::vector<std::string> points;
    // Simple heuristic: split by newlines and filter for meaningful content
    size_t pos = 0;
    while (pos < conversation.size()) {
        auto end = conversation.find('\n', pos);
        if (end == std::string_view::npos) end = conversation.size();

        auto line = conversation.substr(pos, end - pos);
        // Keep lines that look like key points (not too short, not too long)
        if (line.size() > 20 && line.size() < 500) {
            points.emplace_back(line);
        }
        pos = end + 1;
    }
    return points;
}

// Deduplicate memories by removing near-duplicates
auto deduplicate_memories(std::vector<std::string> memories) -> std::vector<std::string> {
    if (memories.size() <= 1) return memories;

    std::vector<std::string> result;
    result.reserve(memories.size());

    // Simple deduplication: remove exact duplicates and substrings
    std::ranges::sort(memories);
    for (size_t i = 0; i < memories.size(); ++i) {
        bool is_duplicate = false;
        for (size_t j = i + 1; j < memories.size(); ++j) {
            if (memories[j].find(memories[i]) != std::string::npos) {
                is_duplicate = true;
                break;
            }
        }
        if (!is_duplicate) {
            result.push_back(std::move(memories[i]));
        }
    }
    return result;
}

// Score how relevant a memory is to the current context (0.0 - 1.0)
auto score_memory_relevance(std::string_view memory, std::string_view context) -> float {
    if (memory.empty() || context.empty()) return 0.0f;

    // Simple word overlap scoring
    int matches = 0;
    int total_words = 0;

    size_t pos = 0;
    while (pos < memory.size()) {
        auto end = memory.find_first_of(" \t\n", pos);
        if (end == std::string_view::npos) end = memory.size();

        auto word = memory.substr(pos, end - pos);
        if (word.size() >= 3) { // Skip very short words
            ++total_words;
            if (context.find(word) != std::string_view::npos) {
                ++matches;
            }
        }
        pos = end + 1;
    }

    if (total_words == 0) return 0.0f;
    return static_cast<float>(matches) / static_cast<float>(total_words);
}

} // namespace cc::services::memory
