module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module core.memdir;

import cc.utils.env_utils;

export namespace memdir {

// Memory types
constexpr std::string_view MEMORY_TYPES[] = {
    "user",
    "feedback",
    "project",
    "reference"
};

enum class MemoryType {
    User,
    Feedback,
    Project,
    Reference,
    Unknown
};

inline MemoryType parse_memory_type(const std::string& raw) {
    for (size_t i = 0; i < std::size(MEMORY_TYPES); ++i) {
        if (raw == MEMORY_TYPES[i]) {
            return static_cast<MemoryType>(i);
        }
    }
    return MemoryType::Unknown;
}

inline std::string_view memory_type_to_string(MemoryType type) {
    switch (type) {
        case MemoryType::User: return "user";
        case MemoryType::Feedback: return "feedback";
        case MemoryType::Project: return "project";
        case MemoryType::Reference: return "reference";
        default: return "";
    }
}

// Memory age utilities
inline int64_t memory_age_days(int64_t mtime_ms) {
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    auto diff = now_ms - mtime_ms;
    return std::max<int64_t>(0, diff / (24 * 60 * 60 * 1000));
}

inline std::string memory_age(int64_t mtime_ms) {
    auto days = memory_age_days(mtime_ms);
    if (days == 0) {
        return "today";
    } else if (days == 1) {
        return "yesterday";
    }
    return std::to_string(days) + " days ago";
}

inline std::string memory_freshness_text(int64_t mtime_ms) {
    auto days = memory_age_days(mtime_ms);
    if (days <= 1) {
        return "";
    }
    return "This memory is " + std::to_string(days) + " days old. "
           "Memories are point-in-time observations, not live state — "
           "claims about code behavior or file:line citations may be outdated. "
           "Verify against current code before asserting as fact.";
}

inline std::string memory_freshness_note(int64_t mtime_ms) {
    auto text = memory_freshness_text(mtime_ms);
    if (text.empty()) {
        return "";
    }
    return "<system-reminder>" + text + "</system-reminder>\n";
}

// Memory header
struct MemoryHeader {
    std::string filename;
    std::string file_path;
    int64_t mtime_ms;
    std::optional<std::string> description;
    MemoryType type = MemoryType::Unknown;
};

// Constants
constexpr int MAX_MEMORY_FILES = 200;
constexpr int FRONTMATTER_MAX_LINES = 30;
constexpr std::string_view ENTRYPOINT_NAME = "MEMORY.md";
constexpr int MAX_ENTRYPOINT_LINES = 200;
constexpr int MAX_ENTRYPOINT_BYTES = 25000;
constexpr std::string_view AUTO_MEM_DIRNAME = "memory";
constexpr std::string_view AUTO_MEM_ENTRYPOINT_NAME = "MEMORY.md";
constexpr std::string_view AUTO_MEM_DISPLAY_NAME = "auto memory";

// Entrypoint truncation result
struct EntrypointTruncation {
    std::string content;
    int line_count;
    int byte_count;
    bool was_line_truncated;
    bool was_byte_truncated;
};

// Truncate entrypoint content
inline EntrypointTruncation truncate_entrypoint_content(const std::string& raw) {
    std::string trimmed = raw;
    // Trim whitespace from both ends
    size_t start = trimmed.find_first_not_of(" \t\n\r");
    size_t end = trimmed.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) {
        trimmed = "";
    } else {
        trimmed = trimmed.substr(start, end - start + 1);
    }

    // Split into lines
    std::vector<std::string> lines;
    std::istringstream iss(trimmed);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }

    int line_count = static_cast<int>(lines.size());
    int byte_count = static_cast<int>(trimmed.size());

    bool was_line_truncated = line_count > MAX_ENTRYPOINT_LINES;
    bool was_byte_truncated = byte_count > MAX_ENTRYPOINT_BYTES;

    if (!was_line_truncated && !was_byte_truncated) {
        return {
            trimmed,
            line_count,
            byte_count,
            was_line_truncated,
            was_byte_truncated
        };
    }

    std::string truncated;
    if (was_line_truncated) {
        for (int i = 0; i < MAX_ENTRYPOINT_LINES; ++i) {
            if (i > 0) truncated += "\n";
            truncated += lines[i];
        }
    } else {
        truncated = trimmed;
    }

    // Byte truncation
    if (truncated.size() > MAX_ENTRYPOINT_BYTES) {
        size_t cut_at = truncated.rfind('\n', MAX_ENTRYPOINT_BYTES);
        if (cut_at == std::string::npos) {
            cut_at = MAX_ENTRYPOINT_BYTES;
        }
        truncated = truncated.substr(0, cut_at);
    }

    std::string reason;
    if (was_byte_truncated && !was_line_truncated) {
        reason = std::to_string(byte_count) + " bytes (limit: " +
                 std::to_string(MAX_ENTRYPOINT_BYTES) + ") — index entries are too long";
    } else if (was_line_truncated && !was_byte_truncated) {
        reason = std::to_string(line_count) + " lines (limit: " +
                 std::to_string(MAX_ENTRYPOINT_LINES) + ")";
    } else {
        reason = std::to_string(line_count) + " lines and " +
                 std::to_string(byte_count) + " bytes";
    }

    std::string final_content = truncated + "\n\n> WARNING: " + std::string(ENTRYPOINT_NAME) +
                                " is " + reason + ". Only part of it was loaded. Keep index "
                                "entries to one line under ~150 characters; move detail into topic files.";

    return {
        final_content,
        line_count,
        byte_count,
        was_line_truncated,
        was_byte_truncated
    };
}

// Format memory manifest
inline std::string format_memory_manifest(const std::vector<MemoryHeader>& memories) {
    std::ostringstream oss;
    for (const auto& mem : memories) {
        oss << "- ";
        if (mem.type != MemoryType::Unknown) {
            oss << "[" << memory_type_to_string(mem.type) << "] ";
        }
        oss << mem.filename;
        oss << " (";
        // Format mtime_ms as ISO string (simplified)
        auto time_point = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(mem.mtime_ms)
        );
        auto time_t = std::chrono::system_clock::to_time_t(time_point);
        auto tm = *std::gmtime(&time_t);
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        oss << ")";
        if (mem.description) {
            oss << ": " << *mem.description;
        }
        oss << "\n";
    }
    return oss.str();
}

// Relevant memory
struct RelevantMemory {
    std::string path;
    int64_t mtime_ms;
};

// Guidance strings
constexpr std::string_view DIR_EXISTS_GUIDANCE =
    "This directory already exists — write to it directly with the Write tool (do not run mkdir or check for its existence).";

constexpr std::string_view DIRS_EXIST_GUIDANCE =
    "Both directories already exist — write to them directly with the Write tool (do not run mkdir or check for their existence).";

// Memory frontmatter example
inline std::vector<std::string> memory_frontmatter_example() {
    return {
        "```markdown",
        "---",
        "name: {{memory name}}",
        "description: {{one-line description — used to decide relevance in future conversations, so be specific}}",
        "type: {{" + std::string(MEMORY_TYPES[0]) + ", " + std::string(MEMORY_TYPES[1]) + ", " +
            std::string(MEMORY_TYPES[2]) + ", " + std::string(MEMORY_TYPES[3]) + "}}",
        "---",
        "",
        "{{memory content — for feedback/project types, structure as: rule/fact, then **Why:** and **How to apply:** lines}}",
        "```"
    };
}

// What not to save section
inline std::vector<std::string> what_not_to_save_section() {
    return {
        "## What NOT to save in memory",
        "",
        "- Code patterns, conventions, architecture, file paths, or project structure — these can be derived by reading the current project state.",
        "- Git history, recent changes, or who-changed-what — `git log` / `git blame` are authoritative.",
        "- Debugging solutions or fix recipes — the fix is in the code; the commit message has the context.",
        "- Anything already documented in CLAUDE.md files.",
        "- Ephemeral task details: in-progress work, temporary state, current conversation context.",
        "",
        "These exclusions apply even when the user explicitly asks you to save. If they ask you to save a PR list or activity summary, ask what was *surprising* or *non-obvious* about it — that is the part worth keeping."
    };
}

// Memory drift caveat
constexpr std::string_view MEMORY_DRIFT_CAVEAT =
    "- Memory records can become stale over time. Use memory as context for what was true at a given point in time. Before answering the user or building assumptions based solely on information in memory records, verify that the memory is still correct and up-to-date by reading the current state of the files or resources. If a recalled memory conflicts with current information, trust what you observe now — and update or remove the stale memory rather than acting on it.";

// When to access section
inline std::vector<std::string> when_to_access_section() {
    return {
        "## When to access memories",
        "- When memories seem relevant, or the user references prior-conversation work.",
        "- You MUST access memory when the user explicitly asks you to check, recall, or remember.",
        "- If the user says to *ignore* or *not use* memory: proceed as if MEMORY.md were empty. Do not apply remembered facts, cite, compare against, or mention memory content.",
        std::string(MEMORY_DRIFT_CAVEAT)
    };
}

// Trusting recall section
inline std::vector<std::string> trusting_recall_section() {
    return {
        "## Before recommending from memory",
        "",
        "A memory that names a specific function, file, or flag is a claim that it existed *when the memory was written*. It may have been renamed, removed, or never merged. Before recommending it:",
        "",
        "- If the memory names a file path: check the file exists.",
        "- If the memory names a function or flag: grep for it.",
        "- If the user is about to act on your recommendation (not just asking about history), verify first.",
        "",
        "\"The memory says X exists\" is not the same as \"X exists now\".",
        "",
        "A memory that summarizes repo state (activity logs, architecture snapshots) is frozen in time. If the user asks about *recent* or *current* state, prefer `git log` or reading the code over recalling the snapshot."
    };
}

// Types section (individual)
inline std::vector<std::string> types_section_individual() {
    return {
        "## Types of memory",
        "",
        "There are several discrete types of memory that you can store in your memory system:",
        "",
        "<types>",
        "<type>",
        "    <name>user</name>",
        "    <description>Contain information about the user's role, goals, responsibilities, and knowledge. Great user memories help you tailor your future behavior to the user's preferences and perspective. Your goal in reading and writing these memories is to build up an understanding of who the user is and how you can be most helpful to them specifically. For example, you should collaborate with a senior software engineer differently than a student who is coding for the very first time. Keep in mind, that the aim here is to be helpful to the user. Avoid writing memories about the user that could be viewed as a negative judgement or that are not relevant to the work you're trying to accomplish together.</description>",
        "    <when_to_save>When you learn any details about the user's role, preferences, responsibilities, or knowledge</when_to_save>",
        "    <how_to_use>When your work should be informed by the user's profile or perspective. For example, if the user is asking you to explain a part of the code, you should answer that question in a way that is tailored to the specific details that they will find most valuable or that helps them build their mental model in relation to domain knowledge they already have.</how_to_use>",
        "    <examples>",
        "    user: I'm a data scientist investigating what logging we have in place",
        "    assistant: [saves user memory: user is a data scientist, currently focused on observability/logging]",
        "",
        "    user: I've been writing Go for ten years but this is my first time touching the React side of this repo",
        "    assistant: [saves user memory: deep Go expertise, new to React and this project's frontend — frame frontend explanations in terms of backend analogues]",
        "    </examples>",
        "</type>",
        "<type>",
        "    <name>feedback</name>",
        "    <description>Guidance the user has given you about how to approach work — both what to avoid and what to keep doing. These are a very important type of memory to read and write as they allow you to remain coherent and responsive to the way you should approach work in the project. Record from failure AND success: if you only save corrections, you will avoid past mistakes but drift away from approaches the user has already validated, and may grow overly cautious.</description>",
        "    <when_to_save>Any time the user corrects your approach (\"no not that\", \"don't\", \"stop doing X\") OR confirms a non-obvious approach worked (\"yes exactly\", \"perfect, keep doing that\", accepting an unusual choice without pushback). Corrections are easy to notice; confirmations are quieter — watch for them. In both cases, save what is applicable to future conversations, especially if surprising or not obvious from the code. Include *why* so you can judge edge cases later.</when_to_save>",
        "    <how_to_use>Let these memories guide your behavior so that the user does not need to offer the same guidance twice.</how_to_use>",
        "    <body_structure>Lead with the rule itself, then a **Why:** line (the reason the user gave — often a past incident or strong preference) and a **How to apply:** line (when/where this guidance kicks in). Knowing *why* lets you judge edge cases instead of blindly following the rule.</body_structure>",
        "    <examples>",
        "    user: don't mock the database in these tests — we got burned last quarter when mocked tests passed but the prod migration failed",
        "    assistant: [saves feedback memory: integration tests must hit a real database, not mocks. Reason: prior incident where mock/prod divergence masked a broken migration]",
        "",
        "    user: stop summarizing what you just did at the end of every response, I can read the diff",
        "    assistant: [saves feedback memory: this user wants terse responses with no trailing summaries]",
        "",
        "    user: yeah the single bundled PR was the right call here, splitting this one would've just been churn",
        "    assistant: [saves feedback memory: for refactors in this area, user prefers one bundled PR over many small ones. Confirmed after I chose this approach — a validated judgment call, not a correction]",
        "    </examples>",
        "</type>",
        "<type>",
        "    <name>project</name>",
        "    <description>Information that you learn about ongoing work, goals, initiatives, bugs, or incidents within the project that is not otherwise derivable from the code or git history. Project memories help you understand the broader context and motivation behind the work the user is doing within this working directory.</description>",
        "    <when_to_save>When you learn who is doing what, why, or by when. These states change relatively quickly so try to keep your understanding of this up to date. Always convert relative dates in user messages to absolute dates when saving (e.g., \"Thursday\" → \"2026-03-05\"), so the memory remains interpretable after time passes.</when_to_save>",
        "    <how_to_use>Use these memories to more fully understand the details and nuance behind the user's request and make better informed suggestions.</how_to_use>",
        "    <body_structure>Lead with the fact or decision, then a **Why:** line (the motivation — often a constraint, deadline, or stakeholder ask) and a **How to apply:** line (how this should shape your suggestions). Project memories decay fast, so the why helps future-you judge whether the memory is still load-bearing.</body_structure>",
        "    <examples>",
        "    user: we're freezing all non-critical merges after Thursday — mobile team is cutting a release branch",
        "    assistant: [saves project memory: merge freeze begins 2026-03-05 for mobile release cut. Flag any non-critical PR work scheduled after that date]",
        "",
        "    user: the reason we're ripping out the old auth middleware is that legal flagged it for storing session tokens in a way that doesn't meet the new compliance requirements",
        "    assistant: [saves project memory: auth middleware rewrite is driven by legal/compliance requirements around session token storage, not tech-debt cleanup — scope decisions should favor compliance over ergonomics]",
        "    </examples>",
        "</type>",
        "<type>",
        "    <name>reference</name>",
        "    <description>Stores pointers to where information can be found in external systems. These memories allow you to remember where to look to find up-to-date information outside of the project directory.</description>",
        "    <when_to_save>When you learn about resources in external systems and their purpose. For example, that bugs are tracked in a specific project in Linear or that feedback can be found in a specific Slack channel.</when_to_save>",
        "    <how_to_use>When the user references an external system or information that may be in an external system.</how_to_use>",
        "    <examples>",
        "    user: check the Linear project \"INGEST\" if you want context on these tickets, that's where we track all pipeline bugs",
        "    assistant: [saves reference memory: pipeline bugs are tracked in Linear project \"INGEST\"]",
        "",
        "    user: the Grafana board at grafana.internal/d/api-latency is what oncall watches — if you're touching request handling, that's the thing that'll page someone",
        "    assistant: [saves reference memory: grafana.internal/d/api-latency is the oncall latency dashboard — check it when editing request-path code]",
        "    </examples>",
        "</type>",
        "</types>",
        ""
    };
}

// Types section (combined)
inline std::vector<std::string> types_section_combined() {
    return {
        "## Types of memory",
        "",
        "There are several discrete types of memory that you can store in your memory system. Each type below declares a <scope> of `private`, `team`, or guidance for choosing between the two.",
        "",
        "<types>",
        "<type>",
        "    <name>user</name>",
        "    <scope>always private</scope>",
        "    <description>Contain information about the user's role, goals, responsibilities, and knowledge. Great user memories help you tailor your future behavior to the user's preferences and perspective. Your goal in reading and writing these memories is to build up an understanding of who the user is and how you can be most helpful to them specifically. For example, you should collaborate with a senior software engineer differently than a student who is coding for the very first time. Keep in mind, that the aim here is to be helpful to the user. Avoid writing memories about the user that could be viewed as a negative judgement or that are not relevant to the work you're trying to accomplish together.</description>",
        "    <when_to_save>When you learn any details about the user's role, preferences, responsibilities, or knowledge</when_to_save>",
        "    <how_to_use>When your work should be informed by the user's profile or perspective. For example, if the user is asking you to explain a part of the code, you should answer that question in a way that is tailored to the specific details that they will find most valuable or that helps them build their mental model in relation to domain knowledge they already have.</how_to_use>",
        "    <examples>",
        "    user: I'm a data scientist investigating what logging we have in place",
        "    assistant: [saves private user memory: user is a data scientist, currently focused on observability/logging]",
        "",
        "    user: I've been writing Go for ten years but this is my first time touching the React side of this repo",
        "    assistant: [saves private user memory: deep Go expertise, new to React and this project's frontend — frame frontend explanations in terms of backend analogues]",
        "    </examples>",
        "</type>",
        "<type>",
        "    <name>feedback</name>",
        "    <scope>default to private. Save as team only when the guidance is clearly a project-wide convention that every contributor should follow (e.g., a testing policy, a build invariant), not a personal style preference.</scope>",
        "    <description>Guidance the user has given you about how to approach work — both what to avoid and what to keep doing. These are a very important type of memory to read and write as they allow you to remain coherent and responsive to the way you should approach work in the project. Record from failure AND success: if you only save corrections, you will avoid past mistakes but drift away from approaches the user has already validated, and may grow overly cautious. Before saving a private feedback memory, check that it doesn't contradict a team feedback memory — if it does, either don't save it or note the override explicitly.</description>",
        "    <when_to_save>Any time the user corrects your approach (\"no not that\", \"don't\", \"stop doing X\") OR confirms a non-obvious approach worked (\"yes exactly\", \"perfect, keep doing that\", accepting an unusual choice without pushback). Corrections are easy to notice; confirmations are quieter — watch for them. In both cases, save what is applicable to future conversations, especially if surprising or not obvious from the code. Include *why* so you can judge edge cases later.</when_to_save>",
        "    <how_to_use>Let these memories guide your behavior so that the user and other users in the project do not need to offer the same guidance twice.</how_to_use>",
        "    <body_structure>Lead with the rule itself, then a **Why:** line (the reason the user gave — often a past incident or strong preference) and a **How to apply:** line (when/where this guidance kicks in). Knowing *why* lets you judge edge cases instead of blindly following the rule.</body_structure>",
        "    <examples>",
        "    user: don't mock the database in these tests — we got burned last quarter when mocked tests passed but the prod migration failed",
        "    assistant: [saves team feedback memory: integration tests must hit a real database, not mocks. Reason: prior incident where mock/prod divergence masked a broken migration. Team scope: this is a project testing policy, not a personal preference]",
        "",
        "    user: stop summarizing what you just did at the end of every response, I can read the diff",
        "    assistant: [saves private feedback memory: this user wants terse responses with no trailing summaries. Private because it's a communication preference, not a project convention]",
        "",
        "    user: yeah the single bundled PR was the right call here, splitting this one would've just been churn",
        "    assistant: [saves private feedback memory: for refactors in this area, user prefers one bundled PR over many small ones. Confirmed after I chose this approach — a validated judgment call, not a correction]",
        "    </examples>",
        "</type>",
        "<type>",
        "    <name>project</name>",
        "    <scope>private or team, but strongly bias toward team</scope>",
        "    <description>Information that you learn about ongoing work, goals, initiatives, bugs, or incidents within the project that is not otherwise derivable from the code or git history. Project memories help you understand the broader context and motivation behind the work users are working on within this working directory.</description>",
        "    <when_to_save>When you learn who is doing what, why, or by when. These states change relatively quickly so try to keep your understanding of this up to date. Always convert relative dates in user messages to absolute dates when saving (e.g., \"Thursday\" → \"2026-03-05\"), so the memory remains interpretable after time passes.</when_to_save>",
        "    <how_to_use>Use these memories to more fully understand the details and nuance behind the user's request, anticipate coordination issues across users, make better informed suggestions.</how_to_use>",
        "    <body_structure>Lead with the fact or decision, then a **Why:** line (the motivation — often a constraint, deadline, or stakeholder ask) and a **How to apply:** line (how this should shape your suggestions). Project memories decay fast, so the why helps future-you judge whether the memory is still load-bearing.</body_structure>",
        "    <examples>",
        "    user: we're freezing all non-critical merges after Thursday — mobile team is cutting a release branch",
        "    assistant: [saves team project memory: merge freeze begins 2026-03-05 for mobile release cut. Flag any non-critical PR work scheduled after that date]",
        "",
        "    user: the reason we're ripping out the old auth middleware is that legal flagged it for storing session tokens in a way that doesn't meet the new compliance requirements",
        "    assistant: [saves team project memory: auth middleware rewrite is driven by legal/compliance requirements around session token storage, not tech-debt cleanup — scope decisions should favor compliance over ergonomics]",
        "    </examples>",
        "</type>",
        "<type>",
        "    <name>reference</name>",
        "    <scope>usually team</scope>",
        "    <description>Stores pointers to where information can be found in external systems. These memories allow you to remember where to look to find up-to-date information outside of the project directory.</description>",
        "    <when_to_save>When you learn about resources in external systems and their purpose. For example, that bugs are tracked in a specific project in Linear or that feedback can be found in a specific Slack channel.</when_to_save>",
        "    <how_to_use>When the user references an external system or information that may be in an external system.</how_to_use>",
        "    <examples>",
        "    user: check the Linear project \"INGEST\" if you want context on these tickets, that's where we track all pipeline bugs",
        "    assistant: [saves team reference memory: pipeline bugs are tracked in Linear project \"INGEST\"]",
        "",
        "    user: the Grafana board at grafana.internal/d/api-latency is what oncall watches — if you're touching request handling, that's the thing that'll page someone",
        "    assistant: [saves team reference memory: grafana.internal/d/api-latency is the oncall latency dashboard — check it when editing request-path code]",
        "    </examples>",
        "</type>",
        "</types>",
        ""
    };
}

// Build memory lines (individual mode)
inline std::vector<std::string> build_memory_lines(
    const std::string& display_name,
    const std::string& memory_dir,
    const std::optional<std::vector<std::string>>& extra_guidelines = std::nullopt,
    bool skip_index = false
) {
    auto how_to_save = [&]() {
        std::vector<std::string> lines;
        if (skip_index) {
            lines = {
                "## How to save memories",
                "",
                "Write each memory to its own file (e.g., `user_role.md`, `feedback_testing.md`) using this frontmatter format:",
                ""
            };
        } else {
            lines = {
                "## How to save memories",
                "",
                "Saving a memory is a two-step process:",
                "",
                "**Step 1** — write the memory to its own file (e.g., `user_role.md`, `feedback_testing.md`) using this frontmatter format:",
                ""
            };
        }

        auto frontmatter_example = memory_frontmatter_example();
        lines.insert(lines.end(), frontmatter_example.begin(), frontmatter_example.end());

        if (skip_index) {
            lines.push_back("");
            lines.push_back("- Keep the name, description, and type fields in memory files up-to-date with the content");
            lines.push_back("- Organize memory semantically by topic, not chronologically");
            lines.push_back("- Update or remove memories that turn out to be wrong or outdated");
            lines.push_back("- Do not write duplicate memories. First check if there is an existing memory you can update before writing a new one.");
        } else {
            lines.push_back("");
            lines.push_back("**Step 2** — add a pointer to that file in `" + std::string(ENTRYPOINT_NAME) + "`. `" + std::string(ENTRYPOINT_NAME) + "` is an index, not a memory — each entry should be one line, under ~150 characters: `- [Title](file.md) — one-line hook`. It has no frontmatter. Never write memory content directly into `" + std::string(ENTRYPOINT_NAME) + "`.");
            lines.push_back("");
            lines.push_back("- `" + std::string(ENTRYPOINT_NAME) + "` is always loaded into your conversation context — lines after " + std::to_string(MAX_ENTRYPOINT_LINES) + " will be truncated, so keep the index concise");
            lines.push_back("- Keep the name, description, and type fields in memory files up-to-date with the content");
            lines.push_back("- Organize memory semantically by topic, not chronologically");
            lines.push_back("- Update or remove memories that turn out to be wrong or outdated");
            lines.push_back("- Do not write duplicate memories. First check if there is an existing memory you can update before writing a new one.");
        }
        return lines;
    }();

    std::vector<std::string> lines = {
        "# " + display_name,
        "",
        "You have a persistent, file-based memory system at `" + memory_dir + "`. " + std::string(DIR_EXISTS_GUIDANCE),
        "",
        "You should build up this memory system over time so that future conversations can have a complete picture of who the user is, how they'd like to collaborate with you, what behaviors to avoid or repeat, and the context behind the work the user gives you.",
        "",
        "If the user explicitly asks you to remember something, save it immediately as whichever type fits best. If they ask you to forget something, find and remove the relevant entry.",
        ""
    };

    auto types_section = types_section_individual();
    lines.insert(lines.end(), types_section.begin(), types_section.end());

    auto what_not_to_save = what_not_to_save_section();
    lines.insert(lines.end(), what_not_to_save.begin(), what_not_to_save.end());
    lines.push_back("");

    lines.insert(lines.end(), how_to_save.begin(), how_to_save.end());
    lines.push_back("");

    auto when_to_access = when_to_access_section();
    lines.insert(lines.end(), when_to_access.begin(), when_to_access.end());
    lines.push_back("");

    auto trusting_recall = trusting_recall_section();
    lines.insert(lines.end(), trusting_recall.begin(), trusting_recall.end());
    lines.push_back("");

    lines.push_back("## Memory and other forms of persistence");
    lines.push_back("Memory is one of several persistence mechanisms available to you as you assist the user in a given conversation. The distinction is often that memory can be recalled in future conversations and should not be used for persisting information that is only useful within the scope of the current conversation.");
    lines.push_back("- When to use or update a plan instead of memory: If you are about to start a non-trivial implementation task and would like to reach alignment with the user on your approach you should use a Plan rather than saving this information to memory. Similarly, if you already have a plan within the conversation and you have changed your approach persist that change by updating the plan rather than saving a memory.");
    lines.push_back("- When to use or update tasks instead of memory: When you need to break your work in current conversation into discrete steps or keep track of your progress use tasks instead of saving to memory. Tasks are great for persisting information about the work that needs to be done in the current conversation, but memory should be reserved for information that will be useful in future conversations.");
    lines.push_back("");

    if (extra_guidelines) {
        lines.insert(lines.end(), extra_guidelines->begin(), extra_guidelines->end());
        lines.push_back("");
    }

    return lines;
}

// Join lines
inline std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream oss;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) oss << "\n";
        oss << lines[i];
    }
    return oss.str();
}

// Path traversal error
class PathTraversalError : public std::runtime_error {
public:
    explicit PathTraversalError(const std::string& message)
        : std::runtime_error(message) {}
};

// Team memory paths
inline bool is_team_memory_enabled() {
    if (cc::utils::is_env_truthy(std::getenv("CLAUDE_CODE_DISABLE_AUTO_MEMORY"))) {
        return false;
    }
    if (cc::utils::is_env_defined_falsy(std::getenv("CLAUDE_CODE_ENABLE_TEAM_MEMORY"))) {
        return false;
    }
    if (cc::utils::is_env_truthy(std::getenv("CLAUDE_CODE_ENABLE_TEAM_MEMORY"))) {
        return true;
    }
    return std::getenv("CC_TEAM_MEMORY_SYNC_URL") != nullptr ||
           std::getenv("TEAM_MEMORY_SYNC_URL") != nullptr;
}

inline std::string get_team_mem_path(const std::string& auto_mem_path) {
    return auto_mem_path + "team/";
}

inline std::string get_team_mem_entrypoint(const std::string& auto_mem_path) {
    return auto_mem_path + "team/MEMORY.md";
}

// Build combined memory prompt (team + auto)
inline std::string build_combined_memory_prompt(
    const std::string& auto_dir,
    const std::string& team_dir,
    const std::optional<std::vector<std::string>>& extra_guidelines = std::nullopt,
    bool skip_index = false
) {
    auto how_to_save = [&]() {
        std::vector<std::string> lines;
        if (skip_index) {
            lines = {
                "## How to save memories",
                "",
                "Write each memory to its own file in the chosen directory (private or team, per the type's scope guidance) using this frontmatter format:",
                ""
            };
        } else {
            lines = {
                "## How to save memories",
                "",
                "Saving a memory is a two-step process:",
                "",
                "**Step 1** — write the memory to its own file in the chosen directory (private or team, per the type's scope guidance) using this frontmatter format:",
                ""
            };
        }

        auto frontmatter_example = memory_frontmatter_example();
        lines.insert(lines.end(), frontmatter_example.begin(), frontmatter_example.end());

        if (skip_index) {
            lines.push_back("");
            lines.push_back("- Keep the name, description, and type fields in memory files up-to-date with the content");
            lines.push_back("- Organize memory semantically by topic, not chronologically");
            lines.push_back("- Update or remove memories that turn out to be wrong or outdated");
            lines.push_back("- Do not write duplicate memories. First check if there is an existing memory you can update before writing a new one.");
        } else {
            lines.push_back("");
            lines.push_back("**Step 2** — add a pointer to that file in the same directory's `" + std::string(ENTRYPOINT_NAME) + "`. Each directory (private and team) has its own `" + std::string(ENTRYPOINT_NAME) + "` index — each entry should be one line, under ~150 characters: `- [Title](file.md) — one-line hook`. They have no frontmatter. Never write memory content directly into a `" + std::string(ENTRYPOINT_NAME) + "`.");
            lines.push_back("");
            lines.push_back("- Both `" + std::string(ENTRYPOINT_NAME) + "` indexes are loaded into your conversation context — lines after " + std::to_string(MAX_ENTRYPOINT_LINES) + " will be truncated, so keep them concise");
            lines.push_back("- Keep the name, description, and type fields in memory files up-to-date with the content");
            lines.push_back("- Organize memory semantically by topic, not chronologically");
            lines.push_back("- Update or remove memories that turn out to be wrong or outdated");
            lines.push_back("- Do not write duplicate memories. First check if there is an existing memory you can update before writing a new one.");
        }
        return lines;
    }();

    std::vector<std::string> lines = {
        "# Memory",
        "",
        "You have a persistent, file-based memory system with two directories: a private directory at `" + auto_dir + "` and a shared team directory at `" + team_dir + "`. " + std::string(DIRS_EXIST_GUIDANCE),
        "",
        "You should build up this memory system over time so that future conversations can have a complete picture of who the user is, how they'd like to collaborate with you, what behaviors to avoid or repeat, and the context behind the work the user gives you.",
        "",
        "If the user explicitly asks you to remember something, save it immediately as whichever type fits best. If they ask you to forget something, find and remove the relevant entry.",
        "",
        "## Memory scope",
        "",
        "There are two scope levels:",
        "",
        "- private: memories that are private between you and the current user. They persist across conversations with only this specific user and are stored at the root `" + auto_dir + "`.",
        "- team: memories that are shared with and contributed by all of the users who work within this project directory. Team memories are synced at the beginning of every session and they are stored at `" + team_dir + "`.",
        ""
    };

    auto types_section = types_section_combined();
    lines.insert(lines.end(), types_section.begin(), types_section.end());

    auto what_not_to_save = what_not_to_save_section();
    lines.insert(lines.end(), what_not_to_save.begin(), what_not_to_save.end());
    lines.push_back("- You MUST avoid saving sensitive data within shared team memories. For example, never save API keys or user credentials.");
    lines.push_back("");

    lines.insert(lines.end(), how_to_save.begin(), how_to_save.end());
    lines.push_back("");

    // When to access section with team memory
    lines.push_back("## When to access memories");
    lines.push_back("- When memories (personal or team) seem relevant, or the user references prior work with them or others in their organization.");
    lines.push_back("- You MUST access memory when the user explicitly asks you to check, recall, or remember.");
    lines.push_back("- If the user says to *ignore* or *not use* memory: proceed as if MEMORY.md were empty. Do not apply remembered facts, cite, compare against, or mention memory content.");
    lines.push_back(std::string(MEMORY_DRIFT_CAVEAT));
    lines.push_back("");

    auto trusting_recall = trusting_recall_section();
    lines.insert(lines.end(), trusting_recall.begin(), trusting_recall.end());
    lines.push_back("");

    lines.push_back("## Memory and other forms of persistence");
    lines.push_back("Memory is one of several persistence mechanisms available to you as you assist the user in a given conversation. The distinction is often that memory can be recalled in future conversations and should not be used for persisting information that is only useful within the scope of the current conversation.");
    lines.push_back("- When to use or update a plan instead of memory: If you are about to start a non-trivial implementation task and would like to reach alignment with the user on your approach you should use a Plan rather than saving this information to memory. Similarly, if you already have a plan within the conversation and you have changed your approach persist that change by updating the plan rather than saving a memory.");
    lines.push_back("- When to use or update tasks instead of memory: When you need to break your work in current conversation into discrete steps or keep track of your progress use tasks instead of saving to memory. Tasks are great for persisting information about the work that needs to be done in the current conversation, but memory should be reserved for information that will be useful in future conversations.");

    if (extra_guidelines) {
        lines.insert(lines.end(), extra_guidelines->begin(), extra_guidelines->end());
    }
    lines.push_back("");

    return join_lines(lines);
}

} // namespace memdir
