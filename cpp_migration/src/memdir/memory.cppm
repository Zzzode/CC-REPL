/// @file memory.cppm
/// @brief Persistent memory system for cross-session knowledge retention.
/// Manages instructions, facts, preferences, and team-shared memories
/// with filesystem persistence, relevance scoring, and CLAUDE.md integration.
module;

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <filesystem>
#include <algorithm>
#include <ranges>
#include <fstream>
#include <unordered_map>

export module cc.memdir.memory;

import cc.utils.error;

export namespace cc::core::memory {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;
using cc::utils::VoidResult;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// ============================================================
// Memory ID strong type
// ============================================================

struct MemoryIdTag {};
struct MemoryId {
    std::string value;
    auto operator<=>(const MemoryId&) const = default;
    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
};

// ============================================================
// Memory type classification
// ============================================================

enum class MemoryType : std::uint8_t {
    Instruction,   // User directives about behavior
    Fact,          // Factual knowledge about the project/environment
    Preference,    // User preferences (style, workflow)
    Context,       // Contextual information about current work
    TeamShared,    // Shared across team members
};

[[nodiscard]] constexpr std::string_view memory_type_name(MemoryType type) noexcept {
    switch (type) {
        case MemoryType::Instruction: return "instruction";
        case MemoryType::Fact:        return "fact";
        case MemoryType::Preference:  return "preference";
        case MemoryType::Context:     return "context";
        case MemoryType::TeamShared:  return "team_shared";
    }
    return "unknown";
}

// ============================================================
// Memory entry - single unit of persistent memory
// ============================================================

enum class MemorySource : std::uint8_t {
    AutoExtracted, // Automatically extracted from conversation
    UserAdded,     // Explicitly added by user
};

struct MemoryEntry {
    MemoryId id;
    std::string content;
    MemoryType type = MemoryType::Fact;
    std::vector<std::string> tags;
    TimePoint created_at = Clock::now();
    TimePoint last_accessed = Clock::now();
    double relevance_score = 1.0;
    std::uint32_t access_count = 0;
    MemorySource source = MemorySource::UserAdded;
};

struct MemoryStats {
    std::size_t total_count = 0;
    std::size_t instruction_count = 0;
    std::size_t fact_count = 0;
    std::size_t preference_count = 0;
    std::size_t context_count = 0;
    std::size_t team_shared_count = 0;
    std::size_t stale_count = 0; // Memories not accessed in 30+ days
};

// ============================================================
// MemoryStore - in-memory storage with CRUD operations
// ============================================================

class MemoryStore {
public:
    /// Add a new memory entry, returns its assigned ID
    [[nodiscard]] MemoryId add(MemoryEntry entry) {
        if (entry.id.empty()) {
            entry.id = generate_id();
        }
        auto id = entry.id;
        entries_.emplace(id.value, std::move(entry));
        return id;
    }

    /// Remove a memory by ID
    [[nodiscard]] VoidResult remove(const MemoryId& id) {
        if (entries_.erase(id.value) == 0) {
            return std::unexpected(Error(ErrorCode::not_found,
                std::format("Memory '{}' not found", id.value)));
        }
        return {};
    }

    /// Update the content of an existing memory
    [[nodiscard]] VoidResult update(const MemoryId& id, std::string content) {
        auto it = entries_.find(id.value);
        if (it == entries_.end()) {
            return std::unexpected(Error(ErrorCode::not_found,
                std::format("Memory '{}' not found", id.value)));
        }
        it->second.content = std::move(content);
        it->second.last_accessed = Clock::now();
        return {};
    }

    /// Keyword-based search with result limit
    [[nodiscard]] std::vector<MemoryEntry> search(std::string_view query, std::size_t limit = 10) const {
        std::vector<MemoryEntry> results;
        for (const auto& [_, entry] : entries_) {
            // Simple keyword containment check
            if (entry.content.find(query) != std::string::npos) {
                results.push_back(entry);
            }
        }
        // Sort by relevance score descending
        std::ranges::sort(results, [](const auto& a, const auto& b) {
            return a.relevance_score > b.relevance_score;
        });
        if (results.size() > limit) results.resize(limit);
        return results;
    }

    /// Find memories relevant to the given context string
    [[nodiscard]] std::vector<MemoryEntry> find_relevant(std::string_view context, std::size_t limit = 5) const {
        std::vector<std::pair<double, const MemoryEntry*>> scored;
        for (const auto& [_, entry] : entries_) {
            double score = compute_relevance(entry, context);
            if (score > 0.0) {
                scored.emplace_back(score, &entry);
            }
        }
        std::ranges::sort(scored, [](const auto& a, const auto& b) {
            return a.first > b.first;
        });
        std::vector<MemoryEntry> results;
        for (std::size_t i = 0; i < std::min(limit, scored.size()); ++i) {
            results.push_back(*scored[i].second);
        }
        return results;
    }

    /// List all stored memories
    [[nodiscard]] std::vector<MemoryEntry> list_all() const {
        std::vector<MemoryEntry> result;
        result.reserve(entries_.size());
        for (const auto& [_, entry] : entries_) {
            result.push_back(entry);
        }
        return result;
    }

    /// Remove memories that haven't been accessed in over 30 days
    void age_memories() {
        auto threshold = Clock::now() - std::chrono::days(30);
        std::erase_if(entries_, [&](const auto& pair) {
            return pair.second.last_accessed < threshold
                && pair.second.type != MemoryType::Instruction;
        });
    }

    /// Compute statistics about the memory store
    [[nodiscard]] MemoryStats stats() const {
        MemoryStats s{};
        auto stale_threshold = Clock::now() - std::chrono::days(30);
        for (const auto& [_, entry] : entries_) {
            ++s.total_count;
            switch (entry.type) {
                case MemoryType::Instruction: ++s.instruction_count; break;
                case MemoryType::Fact:        ++s.fact_count; break;
                case MemoryType::Preference:  ++s.preference_count; break;
                case MemoryType::Context:     ++s.context_count; break;
                case MemoryType::TeamShared:  ++s.team_shared_count; break;
            }
            if (entry.last_accessed < stale_threshold) ++s.stale_count;
        }
        return s;
    }

private:
    std::unordered_map<std::string, MemoryEntry> entries_;
    std::uint64_t next_id_ = 1;

    [[nodiscard]] MemoryId generate_id() {
        return MemoryId{std::format("mem_{}", next_id_++)};
    }

    /// Compute relevance score using keyword overlap heuristic
    [[nodiscard]] static double compute_relevance(const MemoryEntry& entry, std::string_view context) {
        double score = 0.0;
        // Check content overlap
        if (context.find(entry.content.substr(0, 32)) != std::string_view::npos) {
            score += 0.5;
        }
        // Boost by tag matches
        for (const auto& tag : entry.tags) {
            if (context.find(tag) != std::string_view::npos) {
                score += 0.3;
            }
        }
        // Apply base relevance and recency factor
        score *= entry.relevance_score;
        return score;
    }
};

// ============================================================
// MemoryPaths - filesystem path resolution
// ============================================================

class MemoryPaths {
public:
    /// Global memory directory: ~/.claude/memory/
    [[nodiscard]] static std::filesystem::path global_memory_dir() {
        auto home = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "~");
        return home / ".claude" / "memory";
    }

    /// Project-local memory directory: .claude/memory/
    [[nodiscard]] static std::filesystem::path project_memory_dir() {
        return std::filesystem::current_path() / ".claude" / "memory";
    }

    /// Team-shared memory directory
    [[nodiscard]] static std::filesystem::path team_memory_dir() {
        return project_memory_dir() / "team";
    }
};

// ============================================================
// MemoryManager - high-level orchestration
// ============================================================

class MemoryManager {
public:
    /// Load memories from filesystem (global + project)
    [[nodiscard]] VoidResult load() {
        load_from_dir(MemoryPaths::global_memory_dir());
        load_from_dir(MemoryPaths::project_memory_dir());
        return {};
    }

    /// Persist current memories to filesystem
    [[nodiscard]] VoidResult save() {
        auto dir = MemoryPaths::project_memory_dir();
        std::filesystem::create_directories(dir);
        // Serialize each entry as a JSON line
        std::ofstream out(dir / "memories.ndjson", std::ios::trunc);
        if (!out) {
            return std::unexpected(Error(ErrorCode::io_error, "Failed to open memory file for writing"));
        }
        for (const auto& entry : store_.list_all()) {
            out << serialize_entry(entry) << '\n';
        }
        return {};
    }

    /// Auto-extract potential memories from conversation messages
    [[nodiscard]] std::vector<MemoryEntry> extract_from_conversation(
            const std::vector<std::string>& messages) {
        std::vector<MemoryEntry> extracted;
        for (const auto& msg : messages) {
            // Heuristic: lines starting with "Remember:" or containing preference indicators
            if (msg.starts_with("Remember:") || msg.find("always ") != std::string::npos) {
                MemoryEntry entry;
                entry.content = msg;
                entry.type = MemoryType::Instruction;
                entry.source = MemorySource::AutoExtracted;
                entry.id = MemoryId{std::format("auto_{}", auto_id_counter_++)};
                extracted.push_back(std::move(entry));
            }
        }
        return extracted;
    }

    /// Build a prompt injection string with relevant memories
    [[nodiscard]] std::string inject_into_prompt(std::string_view context) {
        auto relevant = store_.find_relevant(context, 5);
        if (relevant.empty()) return {};

        std::string injection = "<memory_context>\n";
        for (const auto& entry : relevant) {
            injection += std::format("- [{}] {}\n",
                memory_type_name(entry.type), entry.content);
        }
        injection += "</memory_context>\n";
        return injection;
    }

    /// Sync team-shared memories from the team directory
    [[nodiscard]] VoidResult sync_team_memories() {
        load_from_dir(MemoryPaths::team_memory_dir());
        return {};
    }

    /// Remove expired/stale memories
    void cleanup_expired() { store_.age_memories(); }

    /// Access underlying store
    [[nodiscard]] MemoryStore& store() noexcept { return store_; }
    [[nodiscard]] const MemoryStore& store() const noexcept { return store_; }

private:
    MemoryStore store_;
    std::uint64_t auto_id_counter_ = 1;

    void load_from_dir(const std::filesystem::path& dir) {
        auto file = dir / "memories.ndjson";
        if (!std::filesystem::exists(file)) return;
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            if (auto entry = deserialize_entry(line); entry.has_value()) {
                store_.add(std::move(*entry));
            }
        }
    }

    /// Serialize a MemoryEntry to JSON string (simplified)
    [[nodiscard]] static std::string serialize_entry(const MemoryEntry& e) {
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
            e.created_at.time_since_epoch()).count();
        return std::format(R"({{"id":"{}","content":"{}","type":"{}","ts":{}}})",
            e.id.value, e.content, memory_type_name(e.type), ts);
    }

    /// Deserialize a JSON line into a MemoryEntry (simplified parser)
    [[nodiscard]] static std::optional<MemoryEntry> deserialize_entry(std::string_view line) {
        // Minimal parse: extract id and content fields
        auto find_field = [&](std::string_view key) -> std::string_view {
            auto pos = line.find(key);
            if (pos == std::string_view::npos) return {};
            pos = line.find('"', pos + key.size() + 2); // skip ":"
            if (pos == std::string_view::npos) return {};
            auto end = line.find('"', pos + 1);
            if (end == std::string_view::npos) return {};
            return line.substr(pos + 1, end - pos - 1);
        };
        auto id = find_field("id");
        auto content = find_field("content");
        if (id.empty() || content.empty()) return std::nullopt;

        MemoryEntry entry;
        entry.id = MemoryId{std::string(id)};
        entry.content = std::string(content);
        return entry;
    }
};

// ============================================================
// CLAUDE.md reader utilities
// ============================================================

struct ClaudeInstructions {
    std::string raw_content;
    std::vector<std::string> rules;
    std::filesystem::path source_path;
};

/// Search up the directory tree from start_dir for CLAUDE.md
[[nodiscard]] inline std::optional<std::filesystem::path> find_claude_md(
        const std::filesystem::path& start_dir) {
    auto current = std::filesystem::absolute(start_dir);
    while (true) {
        auto candidate = current / "CLAUDE.md";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
        auto parent = current.parent_path();
        if (parent == current) break; // Reached filesystem root
        current = parent;
    }
    return std::nullopt;
}

/// Parse CLAUDE.md content into structured instructions
[[nodiscard]] inline ClaudeInstructions parse_claude_md(std::string_view content,
        std::filesystem::path source = {}) {
    ClaudeInstructions result;
    result.raw_content = std::string(content);
    result.source_path = std::move(source);

    // Extract lines that look like rules (starting with - or *)
    std::string_view remaining = content;
    while (!remaining.empty()) {
        auto nl = remaining.find('\n');
        auto line = (nl == std::string_view::npos) ? remaining : remaining.substr(0, nl);
        // Trim leading whitespace
        auto trimmed = line;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
            trimmed.remove_prefix(1);
        }
        if (trimmed.starts_with("- ") || trimmed.starts_with("* ")) {
            result.rules.emplace_back(trimmed.substr(2));
        }
        if (nl == std::string_view::npos) break;
        remaining.remove_prefix(nl + 1);
    }
    return result;
}

} // namespace cc::core::memory
