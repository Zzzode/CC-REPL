/// @file sandbox_config_dialog.cppm
/// @brief Sandbox configuration dialog with filesystem, network, and resource limits.
///
/// 5 sections (vertical layout):
///   Section 1 - Master toggle + explanation
///   Section 2 - Filesystem policy (3-way dropdown: deny outside workspace /
///               read-only outside / full access) + custom allow list,
///               custom deny list
///   Section 3 - Network: block-all toggle + allowed-domain list
///   Section 4 - Resource limits: CPU% / MEM (MB) / Time (s) / Processes
///               (4 sliders)
///   Section 5 - Reset-to-default button
///
/// All paths rendered with middle-ellipsis (via permissions_components).
/// Engine logic (sandbox enforcement) is owned by cc.utils.bash_security
/// and cc.tools.bash_permissions — this file only edits user-visible settings.
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.permissions.sandbox_config;

import cc.ui.permissions.components;

export namespace cc::ui::permissions::sandbox_config {
using namespace ftxui;
namespace pc = cc::ui::permissions::components;

// ============================================================
// Types
// ============================================================

enum class FilesystemPolicy : std::uint8_t {
    DenyOutsideWorkspace = 0,   // Deny access to everything outside workspace
    ReadOnlyOutside,            // Read-only outside workspace, RW inside
    FullAccess,                 // No restriction (full filesystem)
};

/// A single custom path entry with allow/deny type.
struct CustomPathEntry {
    std::string path;
    bool is_allow = true;   // true = allow, false = deny
    bool enabled = true;
};

/// Allowed domain / network target.
struct AllowedDomain {
    std::string pattern;      // exact, glob, or regex
    bool is_regex = false;
    bool enabled = true;
};

/// Resource limit values (0 means "unlimited").
struct ResourceLimits {
    int cpu_percent    = 0;   // 0..100%
    int memory_mb      = 0;   // MB
    int time_seconds   = 0;   // seconds
    int max_processes  = 0;   // max child processes
};

/// Full sandbox configuration state (for the dialog).
struct SandboxConfig {
    bool enabled = true;            // master switch
    FilesystemPolicy fs_policy = FilesystemPolicy::DenyOutsideWorkspace;
    std::vector<CustomPathEntry> custom_allow_paths;
    std::vector<CustomPathEntry> custom_deny_paths;
    bool block_all_network = false;
    std::vector<AllowedDomain> allowed_domains;
    ResourceLimits limits;
};

/// Returned on dialog completion so the caller can persist settings.
struct SandboxConfigResult {
    SandboxConfig config;
    bool was_reset = false;
    bool was_cancelled = false;
};

struct SandboxDialogCallbacks {
    std::function<void(SandboxConfigResult)> on_complete;
};

// ============================================================
// Helpers
// ============================================================

namespace detail {

[[nodiscard]] inline std::string_view FsPolicyLabel(FilesystemPolicy p) {
    switch (p) {
        case FilesystemPolicy::DenyOutsideWorkspace:
            return "Deny access outside workspace";
        case FilesystemPolicy::ReadOnlyOutside:
            return "Read-only outside workspace";
        case FilesystemPolicy::FullAccess:
            return "Full filesystem access";
    }
    return "?";
}

[[nodiscard]] inline Color FsPolicyColor(FilesystemPolicy p) {
    switch (p) {
        case FilesystemPolicy::DenyOutsideWorkspace: return Color::Green;
        case FilesystemPolicy::ReadOnlyOutside:      return Color::Yellow;
        case FilesystemPolicy::FullAccess:           return Color::Red;
    }
    return Color::White;
}

[[nodiscard]] inline SandboxConfig DefaultConfig() {
    SandboxConfig c;
    c.enabled = true;
    c.fs_policy = FilesystemPolicy::DenyOutsideWorkspace;
    c.custom_allow_paths = {
        { "/tmp",           true,  true  },
        { "/var/folders",   true,  true  },
    };
    c.custom_deny_paths = {
        { "~/.ssh",         false, true  },
        { "~/.aws",         false, true  },
    };
    c.block_all_network = false;
    c.allowed_domains = {
        { "api.anthropic.com", false, true },
        { "*.github.com",      false, true },
    };
    c.limits = { 0, 0, 0, 0 }; // unlimited defaults
    return c;
}

/// Render a slider-style integer value as ASCII.
[[nodiscard]] inline Element LimitSlider(std::string_view label,
                                          int value, int max,
                                          std::string_view unit,
                                          bool unlimited_as_zero = true) {
    std::string val_str;
    if (unlimited_as_zero && value == 0) {
        val_str = " unlimited";
    } else {
        val_str = std::format(" {}{}", value, unit);
    }
    double ratio = max > 0 ? std::clamp(static_cast<double>(value) / max, 0.0, 1.0) : 0.0;
    // Gauge-like bar using text characters — use ftxui gauge if available,
    // else fall back to manual fill (hand-rolled).
    auto bar = gauge(ratio) | color(Color::Cyan) | size(WIDTH, GREATER_THAN, 30);

    return hbox({
        text(std::format("{:<12}", label)) | bold,
        bar,
        text(val_str) | color(Color::White),
    });
}

} // namespace detail

// ============================================================
// State
// ============================================================

/// Which section has the cursor (for tab navigation between sections).
enum class FocusSection : std::uint8_t {
    Master,
    FsPolicy,
    AllowPaths,
    DenyPaths,
    NetworkToggle,
    AllowedDomains,
    CpuLimit,
    MemLimit,
    TimeLimit,
    ProcessLimit,
    Reset,
    Done,
};

struct DialogState {
    SandboxConfig config;
    SandboxConfig initial_snapshot;   // for reset + cancel comparison
    SandboxDialogCallbacks cbs;

    // Focus state
    FocusSection focus = FocusSection::Master;

    // Fs-policy selector
    int fs_policy_idx = 0;

    // Editing path lists
    enum class PathEditing : std::uint8_t { None, Allow, Deny };
    PathEditing path_editing = PathEditing::None;
    std::string new_path_buffer;
    std::size_t allow_cursor = 0;
    std::size_t deny_cursor = 0;

    // Domain list editing
    bool editing_domains = false;
    std::string new_domain_buffer;
    std::size_t domain_cursor = 0;

    // Whether any value changed
    bool dirty = false;
};

// ============================================================
// Rendering of each section
// ============================================================

[[nodiscard]] inline Element RenderSectionHeader(std::string_view num,
                                                  std::string_view title,
                                                  Color col = Color::Cyan,
                                                  bool focused = false) {
    auto h = hbox({
        text(std::format("[{}] ", num)) | color(col) | bold,
        text(std::string{title}) | bold,
    });
    if (focused) h = h | inverted;
    return vbox({ h, pc::ThinDivider() });
}

[[nodiscard]] inline Element RenderMasterSection(DialogState& st) {
    bool focused = st.focus == FocusSection::Master;
    auto toggle = hbox({
        text("Enabled:") | bold,
        text("  "),
        st.config.enabled
            ? hbox({ text("[") | dim,
                     text("ON") | color(Color::Green) | bold,
                     text("]") | dim })
            : hbox({ text("[") | dim,
                     text("OFF") | color(Color::GrayDark) | bold,
                     text("]") | dim }),
    });
    auto expl = paragraph(
        "Sandbox isolates shell commands from the host system. Filesystem, "
        "network, and process creation are restricted by policy below. "
        "Commands run inside the sandbox cannot access paths outside the "
        "workspace unless explicitly allowed."
    ) | dim;
    auto body = vbox({
        toggle,
        text(""),
        expl,
    });
    if (focused) body = body | borderStyled(Color::Yellow);
    return vbox({
        RenderSectionHeader("1", "Master switch", Color::Cyan, focused),
        body,
    });
}

[[nodiscard]] inline Element RenderFsPolicySection(DialogState& st) {
    bool focused = st.focus == FocusSection::FsPolicy;
    static const std::array<FilesystemPolicy, 3> policies = {
        FilesystemPolicy::DenyOutsideWorkspace,
        FilesystemPolicy::ReadOnlyOutside,
        FilesystemPolicy::FullAccess,
    };
    Elements items;
    for (int i = 0; i < 3; ++i) {
        bool sel = i == st.fs_policy_idx;
        auto row = hbox({
            text(std::format(" {} ", sel ? "►" : " ")),
            text(std::string{detail::FsPolicyLabel(policies[i])})
                | color(detail::FsPolicyColor(policies[i])),
        });
        if (sel) row = row | inverted | bold;
        items.push_back(row);
    }
    auto body = vbox(items);
    if (focused) body = body | borderStyled(Color::Yellow);
    return vbox({
        RenderSectionHeader("2", "Filesystem policy", Color::Magenta, focused),
        hbox({
            body,
            text("   ") | dim,
            paragraph("Select the default policy for paths that are not "
                      "matched by custom allow/deny entries below.")
                | dim | size(WIDTH, LESS_THAN, 50) | xflex_grow,
        }) | yflex,
    });
}

template <bool IsAllow>
[[nodiscard]] inline Element RenderPathList(
    DialogState& st,
    std::string_view section_num,
    std::string_view title,
    std::vector<CustomPathEntry>& list,
    std::size_t cursor,
    FocusSection fs,
    FocusSection ds)
{
    bool focused_fs = st.focus == fs;
    bool editing = (IsAllow ? st.path_editing == DialogState::PathEditing::Allow
                            : st.path_editing == DialogState::PathEditing::Deny);
    Color col = IsAllow ? Color::Green : Color::Red;

    Elements rows;
    rows.push_back(hbox({
        text("  ") | size(WIDTH, EQUAL, 3),
        text("Enabled") | bold | size(WIDTH, EQUAL, 9),
        text("Path") | bold | size(WIDTH, EQUAL, 50),
        text("Del") | bold | size(WIDTH, EQUAL, 4),
    }));
    for (std::size_t i = 0; i < list.size(); ++i) {
        bool sel = (focused_fs || editing) && i == cursor;
        auto& e = list[i];
        auto row = hbox({
            text(std::format("{:>2}.", i + 1)) | dim | size(WIDTH, EQUAL, 3),
            (e.enabled ? text("  ✓   ") | color(col) : text("      ") | dim)
                | size(WIDTH, EQUAL, 9),
            pc::PathLabel(e.path, 48) | size(WIDTH, EQUAL, 50),
            (e.enabled ? text(" ✕ ") | color(Color::Red) | dim
                       : text("   ") | dim) | size(WIDTH, EQUAL, 4),
        });
        if (sel) row = row | inverted | focus;
        if (!e.enabled) row = row | dim;
        rows.push_back(row);
    }
    if (list.empty()) {
        rows.push_back(text("  (no custom entries)") | dim);
    }
    if (editing) {
        std::string p = st.new_path_buffer.empty()
            ? std::string{" (type path, Enter to save, Esc cancel) "}
            : st.new_path_buffer;
        rows.push_back(hbox({
            text("  + ") | color(col) | bold,
            text(p) | color(Color::Yellow) | xflex_grow,
        }));
    } else if (focused_fs) {
        rows.push_back(hbox({
            text(" [a] ") | color(Color::Cyan) | dim,
            text("Add") | dim,
            text("  [Enter] toggle  [x] delete") | dim,
            filler(),
        }));
    }

    auto body = vbox(rows);
    if (focused_fs) body = body | borderStyled(Color::Yellow);
    return vbox({
        RenderSectionHeader(section_num, title, col, focused_fs || (st.focus == ds)),
        body,
    });
}

[[nodiscard]] inline Element RenderNetworkSection(DialogState& st) {
    bool focus_toggle = st.focus == FocusSection::NetworkToggle;
    bool focus_domains = st.focus == FocusSection::AllowedDomains;
    Color col = Color::Blue;

    auto toggle_row = hbox({
        text("Block all network:") | bold,
        text("  "),
        st.config.block_all_network
            ? hbox({ text("[") | dim,
                     text("ON") | color(Color::Red) | bold,
                     text("] — all outbound traffic denied") | dim })
            : hbox({ text("[") | dim,
                     text("OFF") | color(Color::GrayDark) | bold,
                     text("]") | dim }),
    });
    if (focus_toggle) toggle_row = toggle_row | borderStyled(Color::Yellow);

    Elements domain_rows;
    domain_rows.push_back(hbox({
        text("  ") | size(WIDTH, EQUAL, 3),
        text("On") | bold | size(WIDTH, EQUAL, 5),
        text("Kind") | bold | size(WIDTH, EQUAL, 7),
        text("Pattern") | bold,
    }));
    for (std::size_t i = 0; i < st.config.allowed_domains.size(); ++i) {
        bool sel = focus_domains && i == st.domain_cursor;
        auto& d = st.config.allowed_domains[i];
        auto row = hbox({
            text(std::format("{:>2}.", i + 1)) | dim | size(WIDTH, EQUAL, 3),
            (d.enabled ? text("  ✓") | color(Color::Green) : text("   ") | dim)
                | size(WIDTH, EQUAL, 5),
            text(d.is_regex ? " regex" : " glob ") | dim
                | size(WIDTH, EQUAL, 7),
            pc::PathLabel(d.pattern, 60),
        });
        if (sel) row = row | inverted | focus;
        if (!d.enabled) row = row | dim;
        domain_rows.push_back(row);
    }
    if (st.editing_domains) {
        std::string p = st.new_domain_buffer.empty()
            ? std::string{" (type domain pattern, Enter save) "}
            : st.new_domain_buffer;
        domain_rows.push_back(hbox({
            text("  + ") | color(Color::Green) | bold,
            text(p) | color(Color::Yellow) | xflex_grow,
        }));
    } else if (focus_domains) {
        domain_rows.push_back(hbox({
            text(" [a] add  [r] toggle regex  [x] delete") | dim,
            filler(),
        }));
    }
    auto domains = vbox(domain_rows);
    if (focus_domains) domains = domains | borderStyled(Color::Yellow);

    return vbox({
        RenderSectionHeader("3", "Network", col, focus_toggle || focus_domains),
        toggle_row,
        text(""),
        text("  Allowed domains (only effective when block-all is OFF):") | bold | dim,
        domains,
    });
}

[[nodiscard]] inline Element RenderResourceSection(DialogState& st) {
    bool f_cpu  = st.focus == FocusSection::CpuLimit;
    bool f_mem  = st.focus == FocusSection::MemLimit;
    bool f_time = st.focus == FocusSection::TimeLimit;
    bool f_proc = st.focus == FocusSection::ProcessLimit;
    bool any_focused = f_cpu || f_mem || f_time || f_proc;

    auto cpu  = detail::LimitSlider("CPU",        st.config.limits.cpu_percent,   100, "%");
    auto mem  = detail::LimitSlider("Memory",     st.config.limits.memory_mb,     8192, "MB");
    auto tm   = detail::LimitSlider("Wall time",  st.config.limits.time_seconds,  3600, "s");
    auto proc = detail::LimitSlider("Processes",  st.config.limits.max_processes, 1024, "");

    auto wrap = [](Element el, bool on) {
        return on ? el | borderStyled(Color::Yellow) : el;
    };

    Elements hint = {};
    if (any_focused) {
        std::string_view which = " (adjust with ←/→, 0 = unlimited)";
        hint.push_back(text(std::string{which}) | dim);
    }

    return vbox({
        RenderSectionHeader("4", "Resource limits", Color::Orange1, any_focused),
        wrap(cpu,  f_cpu),
        wrap(mem,  f_mem),
        wrap(tm,   f_time),
        wrap(proc, f_proc),
        vbox(hint),
    });
}

[[nodiscard]] inline Element RenderResetDoneSection(DialogState& st) {
    bool focus_reset = st.focus == FocusSection::Reset;
    bool focus_done  = st.focus == FocusSection::Done;

    auto reset = hbox({ text(" Reset to defaults ") })
                 | borderStyled(Color::Yellow) | color(Color::Yellow);
    if (focus_reset) reset = reset | inverted | bold;

    auto save = hbox({ text(" Save & close ✅ ") })
                | borderStyled(Color::Green) | color(Color::Green);
    if (focus_done) save = save | inverted | bold;

    auto dirty_tag = st.dirty
        ? text(" * modified") | color(Color::Yellow) | bold
        : text("") | dim;

    return vbox({
        RenderSectionHeader("5", "Actions", Color::GrayLight, focus_reset || focus_done),
        hbox({
            reset, text("   "),
            save, filler(),
            dirty_tag,
        }),
        text(""),
        pc::KeyboardHintRow({
            {"Tab/↓",  "next section"},
            {"↑",      "prev section"},
            {"Space",  "toggle/edit"},
            {"Esc",    "cancel"},
        }),
    });
}

// ============================================================
// Full renderer
// ============================================================

[[nodiscard]] inline Element RenderSandboxDialog(std::shared_ptr<DialogState> st) {
    auto s1 = RenderMasterSection(*st);
    auto s2 = RenderFsPolicySection(*st);
    auto s2b = RenderPathList<true>(
        *st, "2a", "Custom allow paths",
        st->config.custom_allow_paths, st->allow_cursor,
        FocusSection::AllowPaths, FocusSection::AllowPaths);
    auto s2c = RenderPathList<false>(
        *st, "2b", "Custom deny paths",
        st->config.custom_deny_paths, st->deny_cursor,
        FocusSection::DenyPaths, FocusSection::DenyPaths);
    auto s3 = RenderNetworkSection(*st);
    auto s4 = RenderResourceSection(*st);
    auto s5 = RenderResetDoneSection(*st);

    auto body = vbox({
        s1, text(""),
        s2, text(""),
        s2b, text(""),
        s2c, text(""),
        s3, text(""),
        s4, text(""),
        s5,
    }) | yframe | yflex_grow;

    Color border = st->config.enabled ? Color::Green : Color::GrayDark;
    return window(
        text(" 🧪 Sandbox Configuration ") | bold | color(Color::Magenta),
        body | xflex_grow
    ) | color(border) | size(WIDTH, LESS_THAN, 110);
}

// ============================================================
// Event handling — focus navigation + section-specific input
// ============================================================

/// Cycle through the sections in display order.
inline void AdvanceFocus(DialogState& st, int delta) {
    static constexpr std::array<FocusSection, 11> order = {
        FocusSection::Master,
        FocusSection::FsPolicy,
        FocusSection::AllowPaths,
        FocusSection::DenyPaths,
        FocusSection::NetworkToggle,
        FocusSection::AllowedDomains,
        FocusSection::CpuLimit,
        FocusSection::MemLimit,
        FocusSection::TimeLimit,
        FocusSection::ProcessLimit,
        FocusSection::Reset,
    };
    int cur = -1;
    for (int i = 0; i < 11; ++i) if (order[i] == st.focus) { cur = i; break; }
    if (cur == -1) cur = 0;
    cur = (cur + delta + 11) % 11;
    st.focus = order[cur];
}

/// Fire the on_complete callback with the given result.
inline void CompleteDialog(DialogState& st, SandboxConfigResult result) {
    if (st.cbs.on_complete) st.cbs.on_complete(std::move(result));
}

[[nodiscard]] inline bool HandleEvents(DialogState& st, Event event) {
    // ---- Section-level navigation ----
    if (event == Event::Tab)          { AdvanceFocus(st, +1); return true; }
    if (event == Event::BackTab)      { AdvanceFocus(st, -1); return true; }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        AdvanceFocus(st, +1); return true;
    }
    if (event == Event::ArrowUp || event == Event::Character('k')) {
        AdvanceFocus(st, -1); return true;
    }

    // ---- Editing a custom path? ----
    if (st.path_editing != DialogState::PathEditing::None) {
        auto& list = st.path_editing == DialogState::PathEditing::Allow
            ? st.config.custom_allow_paths
            : st.config.custom_deny_paths;
        std::size_t& cursor = st.path_editing == DialogState::PathEditing::Allow
            ? st.allow_cursor : st.deny_cursor;

        if (event == Event::Escape) {
            st.path_editing = DialogState::PathEditing::None;
            st.new_path_buffer.clear();
            return true;
        }
        if (event == Event::Return) {
            if (!st.new_path_buffer.empty()) {
                CustomPathEntry e;
                e.path = st.new_path_buffer;
                e.is_allow = (st.path_editing == DialogState::PathEditing::Allow);
                e.enabled = true;
                list.push_back(std::move(e));
                cursor = list.size() - 1;
                st.dirty = true;
            }
            st.path_editing = DialogState::PathEditing::None;
            st.new_path_buffer.clear();
            return true;
        }
        if (event.is_character()) {
            auto c = event.character();
            if (c == "\b"_utf8 || c == "\x7f"_utf8) {
                if (!st.new_path_buffer.empty()) st.new_path_buffer.pop_back();
                return true;
            }
            st.new_path_buffer += c;
            return true;
        }
        return false;
    }

    // ---- Editing allowed domains? ----
    if (st.editing_domains) {
        if (event == Event::Escape) {
            st.editing_domains = false;
            st.new_domain_buffer.clear();
            return true;
        }
        if (event == Event::Return) {
            if (!st.new_domain_buffer.empty()) {
                AllowedDomain d;
                d.pattern = st.new_domain_buffer;
                d.is_regex = false;
                d.enabled = true;
                st.config.allowed_domains.push_back(std::move(d));
                st.domain_cursor = st.config.allowed_domains.size() - 1;
                st.dirty = true;
            }
            st.editing_domains = false;
            st.new_domain_buffer.clear();
            return true;
        }
        if (event.is_character()) {
            auto c = event.character();
            if (c == "\b"_utf8 || c == "\x7f"_utf8) {
                if (!st.new_domain_buffer.empty()) st.new_domain_buffer.pop_back();
                return true;
            }
            st.new_domain_buffer += c;
            return true;
        }
        return false;
    }

    // ---- Section-specific actions ----
    switch (st.focus) {
        // --- Master toggle ---
        case FocusSection::Master: {
            if (event == Event::Character(' ') || event == Event::Return) {
                st.config.enabled = !st.config.enabled;
                st.dirty = true;
                return true;
            }
            break;
        }
        // --- Filesystem policy selector ---
        case FocusSection::FsPolicy: {
            if (event == Event::ArrowLeft  || event == Event::Character('h')) {
                st.fs_policy_idx = (st.fs_policy_idx + 2) % 3;
                static const std::array<FilesystemPolicy, 3> pols = {
                    FilesystemPolicy::DenyOutsideWorkspace,
                    FilesystemPolicy::ReadOnlyOutside,
                    FilesystemPolicy::FullAccess,
                };
                st.config.fs_policy = pols[st.fs_policy_idx];
                st.dirty = true;
                return true;
            }
            if (event == Event::ArrowRight || event == Event::Character('l')) {
                st.fs_policy_idx = (st.fs_policy_idx + 1) % 3;
                static const std::array<FilesystemPolicy, 3> pols = {
                    FilesystemPolicy::DenyOutsideWorkspace,
                    FilesystemPolicy::ReadOnlyOutside,
                    FilesystemPolicy::FullAccess,
                };
                st.config.fs_policy = pols[st.fs_policy_idx];
                st.dirty = true;
                return true;
            }
            break;
        }
        // --- Path list navigation ---
        case FocusSection::AllowPaths:
        case FocusSection::DenyPaths: {
            auto& list  = (st.focus == FocusSection::AllowPaths)
                ? st.config.custom_allow_paths : st.config.custom_deny_paths;
            std::size_t& cursor = (st.focus == FocusSection::AllowPaths)
                ? st.allow_cursor : st.deny_cursor;
            const std::size_t N = list.size();

            if (event == Event::Character('a')) {
                st.path_editing = (st.focus == FocusSection::AllowPaths)
                    ? DialogState::PathEditing::Allow
                    : DialogState::PathEditing::Deny;
                st.new_path_buffer.clear();
                return true;
            }
            if ((event == Event::Character(' ') || event == Event::Return)
                && !list.empty() && cursor < N)
            {
                list[cursor].enabled = !list[cursor].enabled;
                st.dirty = true;
                return true;
            }
            if ((event == Event::Delete || event == Event::Character('x'))
                && !list.empty() && cursor < N)
            {
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(cursor));
                if (cursor >= list.size() && !list.empty()) cursor = list.size() - 1;
                st.dirty = true;
                return true;
            }
            break;
        }
        // --- Network toggle ---
        case FocusSection::NetworkToggle: {
            if (event == Event::Character(' ') || event == Event::Return) {
                st.config.block_all_network = !st.config.block_all_network;
                st.dirty = true;
                return true;
            }
            break;
        }
        // --- Allowed domains ---
        case FocusSection::AllowedDomains: {
            auto& list = st.config.allowed_domains;
            const std::size_t N = list.size();
            if (event == Event::Character('a')) {
                st.editing_domains = true;
                st.new_domain_buffer.clear();
                return true;
            }
            if (event == Event::Character('r') && !list.empty()
                && st.domain_cursor < N) {
                list[st.domain_cursor].is_regex = !list[st.domain_cursor].is_regex;
                st.dirty = true;
                return true;
            }
            if ((event == Event::Character(' ') || event == Event::Return)
                && !list.empty() && st.domain_cursor < N) {
                list[st.domain_cursor].enabled = !list[st.domain_cursor].enabled;
                st.dirty = true;
                return true;
            }
            if ((event == Event::Delete || event == Event::Character('x'))
                && !list.empty() && st.domain_cursor < N) {
                list.erase(list.begin() +
                    static_cast<std::ptrdiff_t>(st.domain_cursor));
                if (st.domain_cursor >= list.size() && !list.empty())
                    st.domain_cursor = list.size() - 1;
                st.dirty = true;
                return true;
            }
            break;
        }
        // --- Resource limit sliders ---
        case FocusSection::CpuLimit: {
            if (event == Event::ArrowRight || event == Event::Character('l')) {
                st.config.limits.cpu_percent =
                    std::min(100, st.config.limits.cpu_percent + 10);
                st.dirty = true; return true;
            }
            if (event == Event::ArrowLeft  || event == Event::Character('h')) {
                st.config.limits.cpu_percent =
                    std::max(0,   st.config.limits.cpu_percent - 10);
                st.dirty = true; return true;
            }
            break;
        }
        case FocusSection::MemLimit: {
            if (event == Event::ArrowRight || event == Event::Character('l')) {
                st.config.limits.memory_mb =
                    std::min(8192, st.config.limits.memory_mb + 256);
                st.dirty = true; return true;
            }
            if (event == Event::ArrowLeft  || event == Event::Character('h')) {
                st.config.limits.memory_mb =
                    std::max(0,    st.config.limits.memory_mb - 256);
                st.dirty = true; return true;
            }
            break;
        }
        case FocusSection::TimeLimit: {
            if (event == Event::ArrowRight || event == Event::Character('l')) {
                st.config.limits.time_seconds =
                    std::min(3600, st.config.limits.time_seconds + 60);
                st.dirty = true; return true;
            }
            if (event == Event::ArrowLeft  || event == Event::Character('h')) {
                st.config.limits.time_seconds =
                    std::max(0,    st.config.limits.time_seconds - 60);
                st.dirty = true; return true;
            }
            break;
        }
        case FocusSection::ProcessLimit: {
            if (event == Event::ArrowRight || event == Event::Character('l')) {
                st.config.limits.max_processes =
                    std::min(1024, st.config.limits.max_processes + 64);
                st.dirty = true; return true;
            }
            if (event == Event::ArrowLeft  || event == Event::Character('h')) {
                st.config.limits.max_processes =
                    std::max(0,    st.config.limits.max_processes - 64);
                st.dirty = true; return true;
            }
            break;
        }
        // --- Reset to defaults ---
        case FocusSection::Reset: {
            if (event == Event::Character(' ') || event == Event::Return) {
                SandboxConfig def = detail::DefaultConfig();
                def.enabled = st.config.enabled; // keep master switch
                st.config = std::move(def);
                st.dirty = true;
                return true;
            }
            break;
        }
        case FocusSection::Done: break;
    }

    // ---- Save & close (focused on Reset/Done, or universal Enter on Done) ----
    if (st.focus == FocusSection::Done &&
        (event == Event::Character(' ') || event == Event::Return)) {
        SandboxConfigResult r;
        r.config = st.config;
        r.was_reset = false;
        r.was_cancelled = false;
        CompleteDialog(st, std::move(r));
        return true;
    }

    // ---- Escape / Q = cancel ----
    if (event == Event::Escape || event == Event::Character('q')) {
        SandboxConfigResult r;
        r.config = st.initial_snapshot;
        r.was_cancelled = true;
        CompleteDialog(st, std::move(r));
        return true;
    }

    return false;
}

// ============================================================
// Public factory
// ============================================================

/// Create the interactive sandbox configuration dialog.
[[nodiscard]] inline Component MakeSandboxConfigDialog(
    SandboxConfig initial,
    SandboxDialogCallbacks cbs)
{
    auto st = std::make_shared<DialogState>();
    st->config = std::move(initial);
    st->initial_snapshot = st->config;
    st->cbs = std::move(cbs);

    // Sync fs_policy_idx from config
    switch (st->config.fs_policy) {
        case FilesystemPolicy::DenyOutsideWorkspace: st->fs_policy_idx = 0; break;
        case FilesystemPolicy::ReadOnlyOutside:      st->fs_policy_idx = 1; break;
        case FilesystemPolicy::FullAccess:           st->fs_policy_idx = 2; break;
    }

    return Renderer([st] { return RenderSandboxDialog(st); })
         | CatchEvent([st](Event event) { return HandleEvents(*st, event); });
}

/// Convenience overload — open dialog with default configuration.
[[nodiscard]] inline Component MakeSandboxConfigDialog(
    SandboxDialogCallbacks cbs)
{
    return MakeSandboxConfigDialog(detail::DefaultConfig(), std::move(cbs));
}

} // namespace cc::ui::permissions::sandbox_config
