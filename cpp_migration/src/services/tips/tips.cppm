/// @file tips.cppm
/// @brief Tips/hints system module.
/// Provides a registry of contextual tips, round-robin scheduling,
/// user preference handling, and built-in tips about features and shortcuts.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <functional>

export module cc.services.tips;

import cc.types.types;

export namespace cc::services::tips {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;

// ============================================================
// Tip category and data structures
// ============================================================

/// Category for organizing tips
enum class TipCategory : uint8_t {
    Shortcut,      // Keyboard shortcuts and quick commands
    Feature,       // Feature highlights and capabilities
    Workflow,      // Workflow optimization and patterns
    Performance,   // Performance tips and resource usage
};

/// Convert category to display string
[[nodiscard]] constexpr std::string_view category_to_string(TipCategory cat) noexcept {
    switch (cat) {
        case TipCategory::Shortcut:    return "shortcut";
        case TipCategory::Feature:     return "feature";
        case TipCategory::Workflow:    return "workflow";
        case TipCategory::Performance: return "performance";
    }
    return "unknown";
}

/// A single tip/hint entry
struct Tip {
    std::string id;                             // Unique identifier
    std::string content;                        // Display text
    TipCategory category;                       // Classification
    uint32_t shown_count = 0;                   // Times displayed
    std::optional<std::chrono::system_clock::time_point> last_shown; // Last display time
};

// ============================================================
// TipRegistry - manages the collection of tips
// ============================================================

/// Registry holding all tips with round-robin retrieval and dismissal support.
class TipRegistry {
public:
    TipRegistry() { load_builtin_tips(); }

    /// Register a new tip
    void register_tip(Tip tip) {
        auto id = tip.id;
        tips_[id] = std::move(tip);
        if (!category_order_.contains(id)) {
            category_order_[id] = next_order_++;
        }
    }

    /// Get the next tip in a category (round-robin, skipping recently shown)
    [[nodiscard]] std::optional<Tip> get_next_tip(TipCategory category) {
        // Collect tips in this category, not dismissed
        std::vector<Tip*> candidates;
        for (auto& [id, tip] : tips_) {
            if (tip.category == category && !dismissed_.contains(id)) {
                candidates.push_back(&tip);
            }
        }
        if (candidates.empty()) return std::nullopt;

        // Sort by shown_count ascending (least shown first), then by last_shown
        std::ranges::sort(candidates, [](const Tip* a, const Tip* b) {
            if (a->shown_count != b->shown_count) return a->shown_count < b->shown_count;
            auto a_time = a->last_shown.value_or(std::chrono::system_clock::time_point{});
            auto b_time = b->last_shown.value_or(std::chrono::system_clock::time_point{});
            return a_time < b_time;
        });

        return *candidates.front();
    }

    /// Mark a tip as shown (increment counter and set timestamp)
    void mark_shown(const std::string& id) {
        auto it = tips_.find(id);
        if (it != tips_.end()) {
            it->second.shown_count++;
            it->second.last_shown = std::chrono::system_clock::now();
        }
    }

    /// Dismiss a tip permanently (never show again)
    void dismiss(const std::string& id) {
        dismissed_.insert(id);
    }

    /// Reset all tips to fresh state
    void reset() {
        dismissed_.clear();
        for (auto& [_, tip] : tips_) {
            tip.shown_count = 0;
            tip.last_shown = std::nullopt;
        }
    }

    /// Get total tip count
    [[nodiscard]] size_t size() const noexcept { return tips_.size(); }

    /// Get tip by ID
    [[nodiscard]] std::optional<Tip> get(const std::string& id) const {
        auto it = tips_.find(id);
        if (it == tips_.end()) return std::nullopt;
        return it->second;
    }

    /// Check if a tip is dismissed
    [[nodiscard]] bool is_dismissed(const std::string& id) const {
        return dismissed_.contains(id);
    }

private:
    /// Load built-in tips covering features, shortcuts, and workflows
    void load_builtin_tips() {
        // --- Shortcut tips ---
        register_tip({"shortcut-escape", "Press Escape to interrupt the current response",
                      TipCategory::Shortcut});
        register_tip({"shortcut-clear", "Use /clear to start a fresh conversation",
                      TipCategory::Shortcut});
        register_tip({"shortcut-compact", "Use /compact to compress context when hitting limits",
                      TipCategory::Shortcut});
        register_tip({"shortcut-help", "Type /help for a list of all slash commands",
                      TipCategory::Shortcut});
        register_tip({"shortcut-vim", "Enable /vim for vi-mode keybindings in the input",
                      TipCategory::Shortcut});

        // --- Feature tips ---
        register_tip({"feature-mcp", "Connect external tools via MCP with /mcp add",
                      TipCategory::Feature});
        register_tip({"feature-agent", "Spawn sub-agents for parallel work with the Agent tool",
                      TipCategory::Feature});
        register_tip({"feature-review", "Use /review to get a code review of staged changes",
                      TipCategory::Feature});
        register_tip({"feature-commit", "Use /commit to auto-generate commit messages",
                      TipCategory::Feature});
        register_tip({"feature-doctor", "Run /doctor to diagnose configuration issues",
                      TipCategory::Feature});
        register_tip({"feature-cost", "Track token usage and cost with /cost",
                      TipCategory::Feature});
        register_tip({"feature-context", "Add files to context with /context add <path>",
                      TipCategory::Feature});

        // --- Workflow tips ---
        register_tip({"workflow-plan", "Use /plan for complex tasks to get a step-by-step approach",
                      TipCategory::Workflow});
        register_tip({"workflow-resume", "Resume previous sessions with /resume",
                      TipCategory::Workflow});
        register_tip({"workflow-branch", "Use /branch to work on isolated task branches",
                      TipCategory::Workflow});
        register_tip({"workflow-export", "Export conversations with /export for sharing",
                      TipCategory::Workflow});
        register_tip({"workflow-session", "Use /session to manage and switch between sessions",
                      TipCategory::Workflow});

        // --- Performance tips ---
        register_tip({"perf-compact", "Compacting context reduces tokens and cost significantly",
                      TipCategory::Performance});
        register_tip({"perf-model", "Switch to a faster model with /model for simple tasks",
                      TipCategory::Performance});
        register_tip({"perf-context", "Keep context focused: remove unneeded files from context",
                      TipCategory::Performance});
        register_tip({"perf-cache", "System prompt caching saves tokens on repeated tool use",
                      TipCategory::Performance});
        register_tip({"perf-parallel", "Use team mode for parallel independent tasks",
                      TipCategory::Performance});
    }

    std::unordered_map<std::string, Tip> tips_;
    std::unordered_set<std::string> dismissed_;
    std::unordered_map<std::string, uint32_t> category_order_;
    uint32_t next_order_ = 0;
};

// ============================================================
// TipScheduler - controls when and how tips are shown
// ============================================================

/// Event types that can trigger contextual tips
enum class TipEvent : uint8_t {
    SessionStart,        // New session started
    LongPause,           // User idle for a while
    ErrorOccurred,       // An error happened
    ContextOverflow,     // Context window near limit
    FirstToolUse,        // First time using a tool type
    MilestoneReached,    // Token/message milestone
};

/// Controls the timing and frequency of tip display,
/// respecting user preferences and avoiding interruption.
class TipScheduler {
public:
    explicit TipScheduler(TipRegistry& registry) : registry_(registry) {}

    /// Configure periodic tip display interval
    void schedule_periodic(std::chrono::seconds interval) {
        periodic_interval_ = interval;
        last_shown_ = std::chrono::steady_clock::now();
    }

    /// Register a contextual tip trigger for an event type
    void show_on_event(TipEvent event, TipCategory category) {
        event_triggers_[event] = category;
    }

    /// Check whether a tip should be shown now (respects cooldown and preferences)
    [[nodiscard]] bool should_show() const {
        if (!enabled_) return false;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_shown_);
        return elapsed >= periodic_interval_;
    }

    /// Get a tip for the given event (if a trigger is registered)
    [[nodiscard]] std::optional<Tip> get_tip_for_event(TipEvent event) {
        auto it = event_triggers_.find(event);
        if (it == event_triggers_.end()) return std::nullopt;
        auto tip = registry_.get_next_tip(it->second);
        if (tip) {
            registry_.mark_shown(tip->id);
            last_shown_ = std::chrono::steady_clock::now();
        }
        return tip;
    }

    /// Get the next periodic tip
    [[nodiscard]] std::optional<Tip> get_periodic_tip() {
        if (!should_show()) return std::nullopt;
        // Cycle through categories
        auto category = static_cast<TipCategory>(next_category_index_ % 4);
        next_category_index_++;
        auto tip = registry_.get_next_tip(category);
        if (tip) {
            registry_.mark_shown(tip->id);
            last_shown_ = std::chrono::steady_clock::now();
        }
        return tip;
    }

    /// Enable or disable tips globally
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }

    /// Set maximum tips per session
    void set_max_per_session(uint32_t max) noexcept { max_per_session_ = max; }

private:
    TipRegistry& registry_;
    bool enabled_ = true;
    std::chrono::seconds periodic_interval_{300};  // Default: every 5 minutes
    std::chrono::steady_clock::time_point last_shown_ = std::chrono::steady_clock::now();
    std::unordered_map<TipEvent, TipCategory> event_triggers_;
    uint32_t next_category_index_ = 0;
    uint32_t max_per_session_ = 10;
    uint32_t shown_this_session_ = 0;
};

} // namespace cc::services::tips
