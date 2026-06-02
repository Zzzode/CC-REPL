/// @file doctor_screen.cppm
/// @brief Doctor diagnostics screen with check results and fix suggestions.
/// Migrated from src/screens/Doctor.tsx
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <expected>
#include <variant>
#include <format>
#include <cstdint>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.doctor_screen;

export namespace cc::ui::doctor_screen {
using namespace ftxui;

// ============================================================
// Types (migrated from Doctor.tsx diagnostic types)
// ============================================================

/// Severity level for a diagnostic check
enum class DiagnosticSeverity : std::uint8_t {
    Ok,         // Check passed
    Info,       // Informational
    Warning,    // Non-critical issue
    Error,      // Critical issue
};

/// A single diagnostic check result
struct DiagnosticCheck {
    std::string name;
    std::string description;
    DiagnosticSeverity severity;
    std::string message;
    std::optional<std::string> fix_suggestion;
    std::optional<std::string> fix_command;    // Auto-fix command if available
};

/// Section of related diagnostics
struct DiagnosticSection {
    std::string title;
    std::string icon;
    std::vector<DiagnosticCheck> checks;
};

/// Version/dist-tag information
struct VersionInfo {
    std::string current_version;
    std::optional<std::string> latest_version;
    std::optional<std::string> stable_version;
    bool update_available = false;
};

/// Agent information for doctor display
struct AgentInfo {
    struct AgentEntry {
        std::string agent_type;
        std::string source; // "built-in", "plugin", "user", "project", etc.
    };
    std::vector<AgentEntry> active_agents;
    std::string user_agents_dir;
    std::string project_agents_dir;
    bool user_dir_exists = false;
    bool project_dir_exists = false;
    std::vector<std::pair<std::string, std::string>> failed_files; // path, error
};

/// Lock/version info
struct VersionLockInfo {
    bool enabled = false;
    int lock_count = 0;
    std::string locks_dir;
    int stale_locks_cleaned = 0;
};

/// Context warnings
struct ContextWarning {
    std::string category;
    std::string message;
    DiagnosticSeverity severity;
};

/// Complete doctor diagnostic info
struct DoctorDiagnosticInfo {
    VersionInfo version;
    AgentInfo agents;
    VersionLockInfo locks;
    std::vector<ContextWarning> context_warnings;
    std::vector<DiagnosticSection> sections;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Get severity icon and color
[[nodiscard]] inline std::pair<std::string, Color> severity_display(DiagnosticSeverity sev) {
    switch (sev) {
        case DiagnosticSeverity::Ok:      return {"✓", Color::Green};
        case DiagnosticSeverity::Info:    return {"ℹ", Color::Blue};
        case DiagnosticSeverity::Warning: return {"⚠", Color::Yellow};
        case DiagnosticSeverity::Error:   return {"✗", Color::Red};
    }
    return {"?", Color::White};
}

/// Render a single diagnostic check
[[nodiscard]] inline Element RenderDiagnosticCheck(const DiagnosticCheck& check) {
    auto [icon, ic] = severity_display(check.severity);

    Elements parts;
    parts.push_back(hbox({
        text(" " + icon + " ") | color(ic),
        text(check.name) | bold | color(Color::White),
        text(": ") | dim,
        text(check.message) | color(
            check.severity == DiagnosticSeverity::Ok ? Color::Green :
            check.severity == DiagnosticSeverity::Error ? Color::Red :
            check.severity == DiagnosticSeverity::Warning ? Color::Yellow :
            Color::GrayLight),
    }));

    // Fix suggestion
    if (check.fix_suggestion) {
        parts.push_back(hbox({
            text("   └ ") | dim,
            text(*check.fix_suggestion) | dim | color(Color::Cyan),
        }));
    }

    // Fix command
    if (check.fix_command) {
        parts.push_back(hbox({
            text("     $ ") | dim | color(Color::Green),
            text(*check.fix_command) | dim | color(Color::Green),
        }));
    }

    return vbox(parts);
}

/// Render a diagnostic section
[[nodiscard]] inline Element RenderDiagnosticSection(const DiagnosticSection& section) {
    Elements items;

    items.push_back(hbox({
        text(" " + section.icon + " ") | dim,
        text(section.title) | bold | color(Color::Blue),
    }));

    for (const auto& check : section.checks) {
        items.push_back(RenderDiagnosticCheck(check));
    }

    return vbox(items);
}

/// Render version info header
[[nodiscard]] inline Element RenderVersionInfo(const VersionInfo& vi) {
    Elements parts = {
        text(" Version: ") | bold,
        text(vi.current_version) | color(Color::Cyan),
    };

    if (vi.update_available && vi.latest_version) {
        parts.push_back(text(" → ") | dim);
        parts.push_back(text(*vi.latest_version) | color(Color::Green));
        parts.push_back(text(" (update available)") | color(Color::Yellow));
    }

    if (vi.stable_version) {
        parts.push_back(text("  stable: ") | dim);
        parts.push_back(text(*vi.stable_version) | dim);
    }

    return hbox(parts);
}

/// Render context warnings
[[nodiscard]] inline Element RenderContextWarnings(
    const std::vector<ContextWarning>& warnings) {

    if (warnings.empty()) return text("");

    Elements items;
    items.push_back(hbox({
        text(" ⚠ Context Warnings ") | bold | color(Color::Yellow),
    }));

    for (const auto& w : warnings) {
        auto [icon, ic] = severity_display(w.severity);
        items.push_back(hbox({
            text("  " + icon + " ") | color(ic),
            text("[" + w.category + "] ") | dim,
            text(w.message) | color(Color::White),
        }));
    }

    return vbox(items);
}

// ============================================================
// Full Doctor Screen
// ============================================================

/// Options for the doctor screen
struct DoctorScreenOptions {
    DoctorDiagnosticInfo diagnostics;
    bool loading = false;
    std::function<void()> on_done;
};

/// Render the full doctor screen
[[nodiscard]] inline Element RenderDoctorScreen(const DoctorScreenOptions& opts) {
    if (opts.loading) {
        return vbox({
            text("") | dim,
            text(" 🔍 Running diagnostics...") | color(Color::Cyan),
            text("") | dim,
        }) | borderRounded;
    }

    Elements sections;

    // Title
    sections.push_back(hbox({
        text(" 🩺 Doctor ") | bold | color(Color::Green),
        filler(),
    }));
    sections.push_back(separator());

    // Version info
    sections.push_back(RenderVersionInfo(opts.diagnostics.version));
    sections.push_back(separator());

    // Context warnings
    auto warnings_el = RenderContextWarnings(opts.diagnostics.context_warnings);
    if (!opts.diagnostics.context_warnings.empty()) {
        sections.push_back(warnings_el);
        sections.push_back(separator());
    }

    // Diagnostic sections
    for (const auto& sec : opts.diagnostics.sections) {
        sections.push_back(RenderDiagnosticSection(sec));
        sections.push_back(text("") | dim);
    }

    // Summary
    int errors = 0, warnings = 0, ok = 0;
    for (const auto& sec : opts.diagnostics.sections) {
        for (const auto& c : sec.checks) {
            switch (c.severity) {
                case DiagnosticSeverity::Ok: ++ok; break;
                case DiagnosticSeverity::Warning: ++warnings; break;
                case DiagnosticSeverity::Error: ++errors; break;
                default: break;
            }
        }
    }

    sections.push_back(separator());
    sections.push_back(hbox({
        text(" Summary: ") | bold,
        text(std::format("{}✓ ", ok)) | color(Color::Green),
        text(std::format("{}⚠ ", warnings)) | color(Color::Yellow),
        text(std::format("{}✗", errors)) | color(Color::Red),
        filler(),
        text(" Press Enter to continue ") | dim,
    }));

    return vbox(sections) | borderRounded;
}

/// Create the doctor screen component
[[nodiscard]] inline Component DoctorScreen(DoctorScreenOptions options) {
    auto state = std::make_shared<DoctorScreenOptions>(std::move(options));

    return Renderer([state] {
        return RenderDoctorScreen(*state);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::Return || event == Event::Escape) {
            if (state->on_done) state->on_done();
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::doctor_screen
