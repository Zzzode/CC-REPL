module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <sstream>
#include <algorithm>

export module cc.skills.bundled.simplify;

import cc.skills.load_skills_dir;

export namespace cc::skills::bundled {

// Simplify a complex output to a shorter, more digestible form
std::string simplify_response(std::string_view complex_output, std::optional<int> max_words) {
    if (complex_output.empty()) {
        return "";
    }

    int word_limit = max_words.value_or(50);

    // Extract key content by removing verbose elements
    std::string simplified;
    std::istringstream stream{std::string(complex_output)};
    std::string word;
    int word_count = 0;

    while (stream >> word && word_count < word_limit) {
        // Skip common filler patterns
        if (word == "Additionally," || word == "Furthermore," ||
            word == "Moreover," || word == "However,") {
            continue;
        }

        if (!simplified.empty()) simplified += " ";
        simplified += word;
        ++word_count;
    }

    // If we truncated, add ellipsis
    if (word_count >= word_limit) {
        simplified += "...";
    }

    return simplified;
}

// Extract key points from a text as a bullet list
std::vector<std::string> extract_key_points(std::string_view text) {
    std::vector<std::string> points;

    if (text.empty()) return points;

    // Split text into sentences (simplified heuristic)
    std::string content(text);
    std::vector<std::string> sentences;

    size_t pos = 0;
    while (pos < content.size()) {
        // Find sentence boundary (period, exclamation, question mark followed by space)
        auto end = content.find_first_of(".!?", pos);
        if (end == std::string::npos) {
            std::string remaining = content.substr(pos);
            if (!remaining.empty() && remaining.size() > 5) {
                sentences.push_back(std::move(remaining));
            }
            break;
        }

        std::string sentence = content.substr(pos, end - pos + 1);
        // Trim leading whitespace
        while (!sentence.empty() && sentence.front() == ' ') {
            sentence.erase(sentence.begin());
        }

        if (sentence.size() > 10) { // Only keep substantial sentences
            sentences.push_back(std::move(sentence));
        }
        pos = end + 1;
        while (pos < content.size() && content[pos] == ' ') ++pos;
    }

    // Score sentences by importance (length, position, keyword presence)
    std::vector<std::pair<int, size_t>> scored;
    for (size_t i = 0; i < sentences.size(); ++i) {
        int score = 0;

        // First and last sentences are usually important
        if (i == 0 || i == sentences.size() - 1) score += 3;

        // Sentences with key action words score higher
        std::string lower = sentences[i];
        std::transform(lower.begin(), lower.end(), lower.begin(),
                      [](unsigned char c) { return std::tolower(c); });

        if (lower.find("must") != std::string::npos) score += 2;
        if (lower.find("important") != std::string::npos) score += 2;
        if (lower.find("key") != std::string::npos) score += 1;
        if (lower.find("result") != std::string::npos) score += 1;
        if (lower.find("conclusion") != std::string::npos) score += 2;

        scored.push_back({score, i});
    }

    // Sort by score descending, take top 5
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    size_t max_points = std::min(scored.size(), size_t{5});
    for (size_t i = 0; i < max_points; ++i) {
        points.push_back(sentences[scored[i].second]);
    }

    return points;
}

// Get the skill manifest for the simplify skill
cc::skills::SkillManifest get_simplify_skill_manifest() {
    return cc::skills::SkillManifest{
        .name = "simplify",
        .description = "Simplify complex outputs into concise summaries",
        .version = "1.0.0",
        .triggers = {"simplify", "summarize", "make shorter", "tl;dr", "key points"},
        .directory = {}
    };
}

} // namespace cc::skills::bundled
