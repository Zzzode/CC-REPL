/// @file notification.cppm
/// @brief Notification component - displays transient notification banners/toasts
/// with severity levels, auto-dismiss, and stacking support.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <deque>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.components.notification;

import cc.types.types;

export namespace cc::ui::components::notification {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Notification severity level
enum class NotificationLevel : std::uint8_t {
    Info,       // Informational (blue)
    Success,    // Operation succeeded (green)
    Warning,    // Non-critical issue (yellow)
    Error,      // Critical issue (red)
    Debug,      // Debug info (gray, only in dev mode)
};

/// Position for notification display
enum class NotificationPosition : std::uint8_t {
    TopRight,
    TopCenter,
    BottomRight,
    BottomCenter,
};

/// Action button on a notification
struct NotificationAction {
    std::string label;
    std::string key;    // Keyboard shortcut
    std::function<void()> on_click;
};

/// A single notification entry
struct NotificationEntry {
    std::string id;
    NotificationLevel level;
    std::string title;
    std::optional<std::string> body;
    std::optional<std::string> source;  // Component that emitted it
    std::vector<NotificationAction> actions;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::seconds ttl{5};        // Time to live
    bool dismissible = true;
    bool is_persistent = false;         // Don't auto-dismiss
    double progress = -1.0;             // -1 = no progress bar
};

/// Options for the notification component
struct NotificationOptions {
    std::deque<NotificationEntry> notifications;
    int max_visible = 5;
    NotificationPosition position = NotificationPosition::TopRight;
    bool show_timestamps = false;
    std::function<void(const std::string& id)> on_dismiss;
    std::function<void(const std::string& id, int action_index)> on_action;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Get icon and color for notification level
[[nodiscard]] inline std::pair<std::string, Color> level_display(NotificationLevel level) {
    switch (level) {
        case NotificationLevel::Info:    return {"ℹ", Color::Blue};
        case NotificationLevel::Success: return {"✓", Color::Green};
        case NotificationLevel::Warning: return {"⚠", Color::Yellow};
        case NotificationLevel::Error:   return {"✗", Color::Red};
        case NotificationLevel::Debug:   return {"⊙", Color::GrayDark};
    }
    return {"?", Color::White};
}

/// Get border color for notification level
[[nodiscard]] inline Color border_color(NotificationLevel level) {
    switch (level) {
        case NotificationLevel::Info:    return Color::Blue;
        case NotificationLevel::Success: return Color::Green;
        case NotificationLevel::Warning: return Color::Yellow;
        case NotificationLevel::Error:   return Color::Red;
        case NotificationLevel::Debug:   return Color::GrayDark;
    }
    return Color::White;
}

/// Format time since creation
[[nodiscard]] inline std::string time_ago(
    std::chrono::steady_clock::time_point created,
    std::chrono::steady_clock::time_point now) {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - created);
    if (elapsed.count() < 5) return "just now";
    if (elapsed.count() < 60) return std::format("{}s ago", elapsed.count());
    if (elapsed.count() < 3600) return std::format("{}m ago", elapsed.count() / 60);
    return std::format("{}h ago", elapsed.count() / 3600);
}

// ============================================================
// Element Rendering
// ============================================================

/// Render a single notification banner
[[nodiscard]] inline Element RenderNotification(
    const NotificationEntry& entry,
    bool show_timestamp,
    std::chrono::steady_clock::time_point now) {

    auto [icon, icon_color] = level_display(entry.level);
    auto bdr_color = border_color(entry.level);

    // Title line
    Elements title_parts = {
        text(icon + " ") | color(icon_color) | bold,
        text(entry.title) | bold,
    };

    if (entry.source) {
        title_parts.push_back(filler());
        title_parts.push_back(text("[" + *entry.source + "]") | dim | color(Color::GrayDark));
    }

    if (show_timestamp) {
        title_parts.push_back(filler());
        title_parts.push_back(text(time_ago(entry.created_at, now)) | dim);
    }

    if (entry.dismissible) {
        title_parts.push_back(text(" ×") | dim | color(Color::GrayDark));
    }

    auto title_line = hbox(title_parts);

    Elements elements = {title_line};

    // Body text
    if (entry.body) {
        elements.push_back(
            paragraph("  " + *entry.body) | dim | color(Color::GrayLight));
    }

    // Progress bar
    if (entry.progress >= 0.0) {
        elements.push_back(hbox({
            text("  ") | dim,
            gauge(std::clamp(entry.progress, 0.0, 1.0))
                | color(icon_color) | flex,
            text(std::format(" {:.0f}%", entry.progress * 100)) | dim,
        }));
    }

    // Action buttons
    if (!entry.actions.empty()) {
        Elements action_parts = {text("  ") | dim};
        for (const auto& action : entry.actions) {
            action_parts.push_back(text("[") | dim);
            action_parts.push_back(text(action.key) | color(Color::Cyan) | bold);
            action_parts.push_back(text("] ") | dim);
            action_parts.push_back(text(action.label + "  ") | dim);
        }
        elements.push_back(hbox(action_parts));
    }

    return vbox(elements) | borderLight | color(bdr_color)
           | size(WIDTH, LESS_THAN, 60);
}

/// Render all visible notifications stacked
[[nodiscard]] inline Element RenderNotifications(const NotificationOptions& opts) {
    if (opts.notifications.empty()) {
        return text("") | size(HEIGHT, EQUAL, 0) | size(WIDTH, EQUAL, 0);
    }

    auto now = std::chrono::steady_clock::now();
    Elements elements;

    int shown = 0;
    for (const auto& entry : opts.notifications) {
        if (shown >= opts.max_visible) break;

        // Check if expired (skip expired ones)
        if (!entry.is_persistent) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - entry.created_at);
            if (elapsed > entry.ttl) continue;
        }

        elements.push_back(
            RenderNotification(entry, opts.show_timestamps, now));
        elements.push_back(text("") | size(HEIGHT, EQUAL, 0));
        ++shown;
    }

    // Overflow indicator
    int total_active = 0;
    for (const auto& entry : opts.notifications) {
        if (entry.is_persistent) { ++total_active; continue; }
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - entry.created_at);
        if (elapsed <= entry.ttl) ++total_active;
    }
    if (total_active > opts.max_visible) {
        elements.push_back(
            text(std::format(" +{} more notifications",
                total_active - opts.max_visible))
            | dim | color(Color::GrayDark));
    }

    return vbox(elements);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create a notification stack component
[[nodiscard]] inline Component NotificationStack(NotificationOptions options) {
    auto state = std::make_shared<NotificationOptions>(std::move(options));

    return Renderer([state] {
        return RenderNotifications(*state);
    }) | CatchEvent([state](Event event) -> bool {
        if (state->notifications.empty()) return false;

        // Dismiss top notification with Escape or 'x'
        if (event == Event::Character('x') || event == Event::Escape) {
            if (!state->notifications.empty()) {
                auto& top = state->notifications.front();
                if (top.dismissible) {
                    if (state->on_dismiss) state->on_dismiss(top.id);
                    state->notifications.pop_front();
                    return true;
                }
            }
        }

        // Check action keys on top notification
        if (!state->notifications.empty() && event.is_character()) {
            auto& top = state->notifications.front();
            for (int i = 0; i < static_cast<int>(top.actions.size()); ++i) {
                if (event == Event::Character(top.actions[i].key[0])) {
                    if (top.actions[i].on_click) {
                        top.actions[i].on_click();
                    }
                    if (state->on_action) {
                        state->on_action(top.id, i);
                    }
                    return true;
                }
            }
        }

        return false;
    });
}

/// Helper: create and push a notification
inline void push_notification(
    NotificationOptions& opts,
    NotificationLevel level,
    const std::string& title,
    std::optional<std::string> body = std::nullopt) {

    NotificationEntry entry;
    entry.id = std::format("notif_{}", opts.notifications.size());
    entry.level = level;
    entry.title = title;
    entry.body = body;
    entry.created_at = std::chrono::steady_clock::now();
    opts.notifications.push_front(entry);

    // Limit total stored
    while (opts.notifications.size() > 50) {
        opts.notifications.pop_back();
    }
}

} // namespace cc::ui::components::notification
