// Hooks Configuration Module
// Merges: hooksConfigManager, hooksConfigSnapshot, fileChangedWatcher,
//         registerFrontmatterHooks, registerSkillHooks, sessionHooks, skillImprovement
// Provides hook configuration management, file watching, and skill/session hook registration
module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module cc.utils.hooks_config;

import cc.utils.json;
import cc.utils.async;
import cc.utils.hooks_registry;

export namespace cc::utils::hooks_config {

using namespace cc::utils::hooks_registry;
using cc::utils::async::Task;
using cc::utils::json::JsonVal;
namespace fs = std::filesystem;

// =========================================================================
// HooksConfigSnapshot (from hooksConfigSnapshot.ts)
// Immutable snapshot of hook configuration at a point in time
// =========================================================================

/// Snapshot of hooks grouped by event type
using HooksConfigMap = std::unordered_map<HookEventType, std::vector<HookMatcher>>;

/// Immutable snapshot of the hooks configuration
class HooksConfigSnapshot {
public:
    explicit HooksConfigSnapshot(HooksConfigMap config);

    /// Get all matchers for a specific event
    [[nodiscard]] const std::vector<HookMatcher>& get_matchers(HookEventType event) const;

    /// Check if any hooks are registered for an event
    [[nodiscard]] bool has_hooks_for_event(HookEventType event) const noexcept;

    /// Get all events that have registered hooks
    [[nodiscard]] std::vector<HookEventType> active_events() const;

    /// Get the raw configuration map
    [[nodiscard]] const HooksConfigMap& raw() const noexcept { return config_; }

private:
    HooksConfigMap config_;
    static const std::vector<HookMatcher> kEmptyMatchers;
};

/// Get the current hooks config snapshot (thread-safe)
[[nodiscard]] const HooksConfigSnapshot* get_hooks_config_snapshot();

/// Update the global hooks config snapshot
void set_hooks_config_snapshot(std::unique_ptr<HooksConfigSnapshot> snapshot);

// =========================================================================
// HooksConfigManager (from hooksConfigManager.ts)
// Groups and resolves hooks from all sources
// =========================================================================

/// Grouped hooks for display in configuration UI
struct GroupedHooks {
    std::unordered_map<HookEventType,
        std::unordered_map<std::string, std::vector<IndividualHookConfig>>> by_event_and_matcher;
};

/// Manager for resolving and grouping hook configurations
class HooksConfigManager {
public:
    /// Get the singleton instance
    static HooksConfigManager& instance() {
        static HooksConfigManager mgr;
        return mgr;
    }

    /// Group all hooks by event and matcher (for display)
    [[nodiscard]] GroupedHooks group_hooks_by_event_and_matcher(
        const std::vector<std::string>& tool_names) const;

    /// Get sorted matchers for a specific event
    [[nodiscard]] std::vector<std::string> get_sorted_matchers_for_event(
        const GroupedHooks& grouped,
        HookEventType event) const;

    /// Get hooks for a specific event and matcher
    [[nodiscard]] std::vector<IndividualHookConfig> get_hooks_for_matcher(
        const GroupedHooks& grouped,
        HookEventType event,
        std::optional<std::string_view> matcher) const;

    /// Get matcher metadata for a specific event
    [[nodiscard]] std::optional<MatcherMetadata> get_matcher_metadata(
        HookEventType event,
        const std::vector<std::string>& tool_names) const;

    /// Get all hooks from all sources
    [[nodiscard]] std::vector<IndividualHookConfig> get_all_hooks() const;

    /// Get hooks filtered by event type
    [[nodiscard]] std::vector<IndividualHookConfig> get_hooks_for_event(
        HookEventType event) const;

    /// Invalidate cached configuration (call after settings change)
    void invalidate_cache();

private:
    HooksConfigManager() = default;
    mutable std::shared_mutex mutex_;
    mutable std::optional<std::vector<IndividualHookConfig>> cached_hooks_;
};

// =========================================================================
// FileWatcher (from fileChangedWatcher.ts)
// Watches files for changes and dispatches FileChanged/CwdChanged hooks
// =========================================================================

/// File change event type
enum class FileChangeEvent {
    Changed,
    Added,
    Removed,
};

/// Callback for file change notifications
using FileChangeNotifier = std::function<void(std::string_view text, bool is_error)>;

/// Watches configured files and dispatches hooks on changes
class FileWatcher {
public:
    /// Get the singleton instance
    static FileWatcher& instance() {
        static FileWatcher watcher;
        return watcher;
    }

    /// Initialize the file watcher for the given working directory
    void initialize(const fs::path& cwd);

    /// Set the notification callback for env hook results
    void set_notifier(FileChangeNotifier notifier);

    /// Add additional paths to watch dynamically
    void add_watch_paths(const std::vector<fs::path>& paths);

    /// Handle a working directory change
    Task<void> on_cwd_changed(const fs::path& old_cwd, const fs::path& new_cwd);

    /// Dispose and stop watching
    void dispose();

    /// Check if the watcher is initialized
    [[nodiscard]] bool is_initialized() const noexcept { return initialized_.load(); }

private:
    FileWatcher() = default;

    /// Resolve configured watch paths from hook matchers
    [[nodiscard]] std::vector<fs::path> resolve_watch_paths() const;

    /// Start watching the given paths
    void start_watching(const std::vector<fs::path>& paths);

    /// Handle a file change event
    Task<void> handle_file_change(const fs::path& path, FileChangeEvent event);

    std::atomic<bool> initialized_{false};
    fs::path current_cwd_;
    std::vector<fs::path> dynamic_watch_paths_;
    FileChangeNotifier notifier_;
    mutable std::mutex mutex_;
};

// =========================================================================
// Session Hooks (from sessionHooks.ts)
// In-memory per-session hooks that exist for the lifetime of a session
// =========================================================================

/// Function hook predicate (returns true if condition is met)
using FunctionHookPredicate = std::function<bool(const std::vector<JsonVal>& messages)>;

/// A function-based session hook
struct FunctionSessionHook {
    HookEventType event;
    std::string matcher;
    FunctionHookPredicate predicate;
    std::string failure_message;
    std::optional<std::chrono::milliseconds> timeout;
};

/// Manage session-scoped hooks
class SessionHookManager {
public:
    /// Get the singleton instance
    static SessionHookManager& instance() {
        static SessionHookManager mgr;
        return mgr;
    }

    /// Add a function hook to a specific session
    void add_function_hook(
        std::string_view session_id,
        HookEventType event,
        std::string_view matcher,
        FunctionHookPredicate predicate,
        std::string failure_message,
        std::optional<std::chrono::milliseconds> timeout = std::nullopt);

    /// Get all session hooks for a session, grouped by event
    [[nodiscard]] std::unordered_map<HookEventType, std::vector<HookMatcher>>
        get_session_hooks(std::string_view session_id) const;

    /// Clear all hooks for a specific session
    void clear_session_hooks(std::string_view session_id);

    /// Clear all session hooks (global reset)
    void clear_all();

private:
    SessionHookManager() = default;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::vector<FunctionSessionHook>> hooks_by_session_;
};

// =========================================================================
// SkillHookRegistrar (from registerSkillHooks.ts, registerFrontmatterHooks.ts)
// Registers hooks from skill definitions and frontmatter
// =========================================================================

/// Skill hook definition extracted from a skill's frontmatter
struct SkillHookDefinition {
    HookEventType event;
    std::string matcher;
    HookCommand command;
    std::string skill_name;
    fs::path skill_path;
};

/// Frontmatter hook definition from a CLAUDE.md or rule file
struct FrontmatterHookDefinition {
    HookEventType event;
    std::optional<std::string> matcher;
    HookCommand command;
    fs::path source_file;
};

/// Registrar for hooks defined in skills and frontmatter
class SkillHookRegistrar {
public:
    /// Get the singleton instance
    static SkillHookRegistrar& instance() {
        static SkillHookRegistrar reg;
        return reg;
    }

    /// Register all hooks from skill frontmatter definitions
    void register_skill_hooks(const std::vector<SkillHookDefinition>& definitions);

    /// Register hooks from frontmatter in instruction files
    void register_frontmatter_hooks(const std::vector<FrontmatterHookDefinition>& definitions);

    /// Unregister all hooks from a specific skill
    void unregister_skill(std::string_view skill_name);

    /// Unregister hooks from a specific source file
    void unregister_frontmatter_source(const fs::path& source_file);

    /// Get all registered skill hooks as IndividualHookConfigs
    [[nodiscard]] std::vector<IndividualHookConfig> get_registered_hooks() const;

    /// Clear all registered hooks
    void clear_all();

private:
    SkillHookRegistrar() = default;
    mutable std::shared_mutex mutex_;
    std::vector<SkillHookDefinition> skill_hooks_;
    std::vector<FrontmatterHookDefinition> frontmatter_hooks_;
};

// =========================================================================
// Skill Improvement (from skillImprovement.ts)
// Tracks hook outcomes for skill quality feedback
// =========================================================================

/// Hook result outcome classification
enum class HookResultOutcome {
    Success,
    Blocking,
    NonBlockingError,
    Cancelled,
};

/// Record of a skill hook execution outcome
struct SkillHookOutcomeRecord {
    std::string skill_name;
    HookEventType event;
    HookResultOutcome outcome;
    std::chrono::steady_clock::time_point timestamp;
    std::optional<std::string> error_message;
};

/// Tracks skill hook outcomes for improvement feedback
class SkillImprovementTracker {
public:
    /// Get the singleton instance
    static SkillImprovementTracker& instance() {
        static SkillImprovementTracker tracker;
        return tracker;
    }

    /// Record a skill hook execution outcome
    void record_outcome(SkillHookOutcomeRecord record);

    /// Get recent outcomes for a skill
    [[nodiscard]] std::vector<SkillHookOutcomeRecord> get_outcomes(
        std::string_view skill_name,
        std::optional<std::size_t> limit = std::nullopt) const;

    /// Get the success rate for a skill's hooks
    [[nodiscard]] double get_success_rate(std::string_view skill_name) const;

    /// Clear all tracked outcomes
    void clear();

private:
    SkillImprovementTracker() = default;
    mutable std::shared_mutex mutex_;
    std::vector<SkillHookOutcomeRecord> outcomes_;
};

} // namespace cc::utils::hooks_config
