module;
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <algorithm>

export module cc.skills.bundled.stuck;

import cc.skills.load_skills_dir;

export namespace cc::skills::bundled {

// Detect if recent outputs indicate a stuck/looping pattern
bool detect_stuck_pattern(std::span<std::string> recent_outputs) {
    if (recent_outputs.size() < 3) {
        return false; // Need at least 3 outputs to detect a pattern
    }

    // Pattern 1: Identical consecutive outputs (exact repetition)
    int consecutive_same = 0;
    for (size_t i = 1; i < recent_outputs.size(); ++i) {
        if (recent_outputs[i] == recent_outputs[i - 1]) {
            ++consecutive_same;
        }
    }
    if (consecutive_same >= 2) {
        return true; // Three or more identical outputs
    }

    // Pattern 2: Same error message repeating
    int error_count = 0;
    for (const auto& output : recent_outputs) {
        if (output.find("Error") != std::string::npos ||
            output.find("error") != std::string::npos ||
            output.find("failed") != std::string::npos) {
            ++error_count;
        }
    }
    if (error_count >= 3) {
        return true; // Multiple consecutive errors suggest being stuck
    }

    // Pattern 3: Oscillating between two states
    if (recent_outputs.size() >= 4) {
        bool oscillating = true;
        for (size_t i = 2; i < recent_outputs.size() && oscillating; ++i) {
            if (recent_outputs[i] != recent_outputs[i - 2]) {
                oscillating = false;
            }
        }
        if (oscillating) return true;
    }

    return false;
}

// Suggest an action to get unstuck based on context
std::string suggest_unstuck_action(std::string_view context) {
    std::string ctx(context);
    std::transform(ctx.begin(), ctx.end(), ctx.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Analyze context to suggest appropriate action
    if (ctx.find("permission") != std::string::npos || ctx.find("denied") != std::string::npos) {
        return "Try running with elevated permissions, or check file/directory ownership.";
    }

    if (ctx.find("not found") != std::string::npos || ctx.find("no such") != std::string::npos) {
        return "Verify the path/resource exists. Try listing the directory or searching for the correct name.";
    }

    if (ctx.find("timeout") != std::string::npos || ctx.find("timed out") != std::string::npos) {
        return "The operation is timing out. Try: 1) Check network connectivity, 2) Increase timeout, 3) Try a different endpoint.";
    }

    if (ctx.find("syntax") != std::string::npos || ctx.find("parse") != std::string::npos) {
        return "There's a syntax/parsing error. Try: 1) Validate the input format, 2) Check for missing delimiters, 3) Simplify the input.";
    }

    if (ctx.find("import") != std::string::npos || ctx.find("module") != std::string::npos) {
        return "Module/import issue. Try: 1) Verify the dependency is installed, 2) Check import paths, 3) Clear module cache.";
    }

    // Generic suggestion
    return "Try a different approach: 1) Break the problem into smaller steps, "
           "2) Verify assumptions with explicit checks, "
           "3) Search for similar patterns in the codebase.";
}

// Get alternative approaches when the current one isn't working
std::vector<std::string> get_alternative_approaches(std::string_view current_approach) {
    std::vector<std::string> alternatives;

    std::string approach(current_approach);
    std::transform(approach.begin(), approach.end(), approach.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Suggest complementary approaches
    if (approach.find("edit") != std::string::npos || approach.find("modify") != std::string::npos) {
        alternatives.push_back("Rewrite the file from scratch instead of editing in place");
        alternatives.push_back("Create a new file with the desired content, then replace");
        alternatives.push_back("Use a different editing strategy (line-based vs block-based)");
    }

    if (approach.find("search") != std::string::npos || approach.find("find") != std::string::npos) {
        alternatives.push_back("Try broader search terms or patterns");
        alternatives.push_back("Search in different directories or file types");
        alternatives.push_back("Use semantic search instead of text matching");
    }

    if (approach.find("build") != std::string::npos || approach.find("compile") != std::string::npos) {
        alternatives.push_back("Clean build artifacts and rebuild from scratch");
        alternatives.push_back("Check for missing or conflicting dependencies");
        alternatives.push_back("Try building individual components in isolation");
    }

    // Always include generic alternatives
    if (alternatives.empty()) {
        alternatives.push_back("Step back and re-read the requirements");
        alternatives.push_back("Try the simplest possible implementation first");
        alternatives.push_back("Look for existing code that does something similar");
        alternatives.push_back("Break the task into smaller, independently testable steps");
    }

    return alternatives;
}

// Get the skill manifest for the stuck skill
cc::skills::SkillManifest get_stuck_skill_manifest() {
    return cc::skills::SkillManifest{
        .name = "stuck",
        .description = "Detect stuck patterns and suggest alternative approaches",
        .version = "1.0.0",
        .triggers = {"stuck", "not working", "keep failing", "try harder", "different approach"},
        .directory = {}
    };
}

} // namespace cc::skills::bundled
