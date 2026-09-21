// C++23 Module: Panel components for settings, MCP, tasks, diff, help, permissions
module;

#include <chrono>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

export module cc.ui.panels;


namespace ftxui {
    class Element;
    class Node;
}

export namespace cc::ui {

// Panel type discriminator
enum class PanelType { Settings, Mcp, Tasks, Diff, Help, Permissions };

// Convert PanelType to display name
[[nodiscard]] constexpr auto panel_name(PanelType type) -> std::string_view {
    switch (type) {
        case PanelType::Settings:    return "Settings";
        case PanelType::Mcp:         return "MCP Servers";
        case PanelType::Tasks:       return "Tasks";
        case PanelType::Diff:        return "Diff";
        case PanelType::Help:        return "Help";
        case PanelType::Permissions: return "Permissions";
    }
    return "Unknown";
}

// ─── Settings Panel ──────────────────────────────────────────

// A single setting entry
struct SettingEntry {
    std::string key;
    std::string value;
    std::string description;
    std::string category;
    bool is_readonly{false};
    std::vector<std::string> allowed_values; // Empty means free-form
};

// SettingsPanel: displays and edits settings
class SettingsPanel {
public:
    auto load(std::vector<SettingEntry> entries) -> void {
        entries_ = std::move(entries);
        // Group by category
        categories_.clear();
        for (const auto& entry : entries_) {
            categories_[entry.category].push_back(&entry);
        }
    }

    auto set_filter(std::string_view filter) -> void { filter_ = filter; }

    // Get filtered entries matching search query
    [[nodiscard]] auto filtered_entries() const -> std::vector<const SettingEntry*> {
        std::vector<const SettingEntry*> result;
        if (filter_.empty()) {
            result.reserve(entries_.size());
            for (const auto& entry : entries_) {
                result.push_back(&entry);
            }
            return result;
        }
        for (const auto& entry : entries_) {
            if (entry.key.contains(filter_) || entry.description.contains(filter_)) {
                result.push_back(&entry);
            }
        }
        return result;
    }

    // Update a setting value, returns error if readonly
    [[nodiscard]] auto update(std::string_view key, std::string_view value)
        -> std::expected<void, std::string> {
        auto it = std::find_if(entries_.begin(), entries_.end(), [key](const auto& e) { return e.key == key; });
        if (it == entries_.end()) return std::unexpected(std::format("Setting '{}' not found", key));
        if (it->is_readonly) return std::unexpected(std::format("Setting '{}' is read-only", key));
        if (!it->allowed_values.empty()) {
            auto valid = std::find(it->allowed_values.begin(), it->allowed_values.end(), value);
            if (valid == it->allowed_values.end())
                return std::unexpected(std::format("Invalid value '{}' for '{}'", value, key));
        }
        it->value = std::string(value);
        return {};
    }

    [[nodiscard]] auto render() const -> ftxui::Element;

private:
    std::vector<SettingEntry> entries_;
    std::map<std::string, std::vector<const SettingEntry*>> categories_;
    std::string filter_;
};

// ─── MCP Panel ───────────────────────────────────────────────

// MCP server connection status
enum class McpStatus { Connected, Connecting, Disconnected, Error };

// Single MCP server entry
struct McpServerEntry {
    std::string name;
    std::string uri;
    McpStatus status{McpStatus::Disconnected};
    std::vector<std::string> capabilities;
    std::optional<std::string> error_message;
    std::size_t tool_count{0};
    std::chrono::steady_clock::time_point last_heartbeat;
};

// McpPanel: shows MCP server status
class McpPanel {
public:
    auto set_servers(std::vector<McpServerEntry> servers) -> void { servers_ = std::move(servers); }

    [[nodiscard]] auto servers() const -> const std::vector<McpServerEntry>& { return servers_; }

    // Get status summary string
    [[nodiscard]] auto status_summary() const -> std::string {
        auto connected = std::count_if(servers_.begin(), servers_.end(),
            [](const auto& s) { return s.status == McpStatus::Connected; });
        return std::format("{}/{} connected", connected, servers_.size());
    }

    // Status icon for display
    [[nodiscard]] static auto status_icon(McpStatus status) -> std::string_view {
        switch (status) {
            case McpStatus::Connected:    return "●";
            case McpStatus::Connecting:   return "◐";
            case McpStatus::Disconnected: return "○";
            case McpStatus::Error:        return "✗";
        }
        return "?";
    }

    [[nodiscard]] auto render() const -> ftxui::Element;

private:
    std::vector<McpServerEntry> servers_;
};

// ─── Tasks Panel ─────────────────────────────────────────────

// Task status
enum class TaskStatus { Pending, Running, Completed, Failed, Cancelled };

// Background task entry
struct TaskEntry {
    std::string id;
    std::string description;
    TaskStatus status{TaskStatus::Pending};
    double progress{0.0}; // 0.0 - 1.0
    std::optional<std::string> error;
    std::chrono::system_clock::time_point created_at;
    std::optional<std::chrono::system_clock::time_point> completed_at;
};

// TasksPanel: background task list with progress
class TasksPanel {
public:
    auto set_tasks(std::vector<TaskEntry> tasks) -> void { tasks_ = std::move(tasks); }
    auto add_task(TaskEntry task) -> void { tasks_.push_back(std::move(task)); }

    auto update_progress(std::string_view id, double progress) -> void {
        if (auto it = find_task(id); it != tasks_.end()) it->progress = progress;
    }

    auto complete_task(std::string_view id) -> void {
        if (auto it = find_task(id); it != tasks_.end()) {
            it->status = TaskStatus::Completed;
            it->progress = 1.0;
            it->completed_at = std::chrono::system_clock::now();
        }
    }

    auto fail_task(std::string_view id, std::string_view error) -> void {
        if (auto it = find_task(id); it != tasks_.end()) {
            it->status = TaskStatus::Failed;
            it->error = std::string(error);
        }
    }

    // Count active (running) tasks
    [[nodiscard]] auto active_count() const -> std::size_t {
        return static_cast<std::size_t>(std::count_if(
            tasks_.begin(), tasks_.end(),
            [](const auto& t) { return t.status == TaskStatus::Running; }));
    }

    [[nodiscard]] auto render() const -> ftxui::Element;

private:
    std::vector<TaskEntry> tasks_;

    auto find_task(std::string_view id) -> std::vector<TaskEntry>::iterator {
        return std::find_if(tasks_.begin(), tasks_.end(), [id](const auto& t) { return t.id == id; });
    }
};

// ─── Diff Panel ──────────────────────────────────────────────

// Diff line type for syntax highlighting
enum class PanelDiffLineType { Context, Added, Removed, Header };

// Single line in a diff hunk
struct PanelDiffLine {
    PanelDiffLineType type;
    std::string content;
    std::optional<std::size_t> old_line_num;
    std::optional<std::size_t> new_line_num;
};

// A diff hunk (section of changes)
struct DiffHunk {
    std::size_t old_start{0};
    std::size_t old_count{0};
    std::size_t new_start{0};
    std::size_t new_count{0};
    std::vector<PanelDiffLine> lines;
};

// File diff entry
struct FileDiff {
    std::string file_path;
    std::vector<DiffHunk> hunks;
    std::size_t additions{0};
    std::size_t deletions{0};
    bool is_binary{false};
    bool is_new_file{false};
    bool is_deleted{false};
};

// DiffPanel: file diff viewer with syntax highlighting
class DiffPanel {
public:
    auto set_diffs(std::vector<FileDiff> diffs) -> void { diffs_ = std::move(diffs); }

    auto set_active_file(std::size_t index) -> void {
        if (index < diffs_.size()) active_file_ = index;
    }

    // Total stats across all files
    [[nodiscard]] auto total_stats() const -> std::pair<std::size_t, std::size_t> {
        std::size_t adds = 0, dels = 0;
        for (const auto& d : diffs_) { adds += d.additions; dels += d.deletions; }
        return {adds, dels};
    }

    [[nodiscard]] auto file_count() const -> std::size_t { return diffs_.size(); }

    [[nodiscard]] auto render() const -> ftxui::Element;

private:
    std::vector<FileDiff> diffs_;
    std::size_t active_file_{0};
    std::size_t scroll_offset_{0};
};

// ─── Help Panel ──────────────────────────────────────────────

// Keybinding entry
struct Keybinding {
    std::string keys;       // e.g., "Ctrl+C"
    std::string action;     // e.g., "Cancel current generation"
    std::string category;   // e.g., "General", "Navigation"
};

// HelpPanel: keybindings and command reference
class HelpPanel {
public:
    auto set_keybindings(std::vector<Keybinding> bindings) -> void { bindings_ = std::move(bindings); }
    auto set_commands(std::vector<std::pair<std::string, std::string>> cmds) -> void {
        commands_ = std::move(cmds);
    }

    // Get keybindings grouped by category
    [[nodiscard]] auto grouped_bindings() const
        -> std::map<std::string, std::vector<const Keybinding*>> {
        std::map<std::string, std::vector<const Keybinding*>> grouped;
        for (const auto& b : bindings_) grouped[b.category].push_back(&b);
        return grouped;
    }

    [[nodiscard]] auto render() const -> ftxui::Element;

private:
    std::vector<Keybinding> bindings_;
    std::vector<std::pair<std::string, std::string>> commands_;
};

// ─── Permissions Panel ───────────────────────────────────────

// Permission level
enum class PermissionLevel { Allow, Ask, Deny };

// Permission rule entry
struct PermissionRule {
    std::string tool_name;
    PermissionLevel level{PermissionLevel::Ask};
    std::optional<std::string> path_pattern; // Glob pattern restriction
    std::string reason;
};

// PermissionsPanel: current permission state
class PermissionsPanel {
public:
    auto set_rules(std::vector<PermissionRule> rules) -> void { rules_ = std::move(rules); }

    // Check permission for a tool+path combination
    [[nodiscard]] auto check(std::string_view tool, std::string_view path) const -> PermissionLevel {
        for (const auto& rule : rules_) {
            if (rule.tool_name == tool) {
                if (!rule.path_pattern || path.starts_with(*rule.path_pattern)) {
                    return rule.level;
                }
            }
        }
        return PermissionLevel::Ask; // Default
    }

    // Level display icon
    [[nodiscard]] static auto level_icon(PermissionLevel level) -> std::string_view {
        switch (level) {
            case PermissionLevel::Allow: return "✓";
            case PermissionLevel::Ask:   return "?";
            case PermissionLevel::Deny:  return "✗";
        }
        return " ";
    }

    [[nodiscard]] auto render() const -> ftxui::Element;

private:
    std::vector<PermissionRule> rules_;
};

// ─── Unified Panel Render ────────────────────────────────────

// State variant for panel rendering
using PanelState = std::variant<
    SettingsPanel,
    McpPanel,
    TasksPanel,
    DiffPanel,
    HelpPanel,
    PermissionsPanel
>;

// Render any panel by type via visitor
[[nodiscard]] auto render_panel(const PanelState& state) -> ftxui::Element;

// Render panel with title bar and close button
[[nodiscard]] auto render_panel_frame(PanelType type, const PanelState& state) -> ftxui::Element;

} // namespace cc::ui
