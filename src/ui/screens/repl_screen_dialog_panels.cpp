// repl_screen_dialog_panels.cpp - impl unit for cc.ui.screens.repl_screen
// (RFC 0001 Phase C batch 9). the settings / trust lazy dialog components and the tool-permission panel
// cluster (the PermissionPanelKind classifier is TU-local here).
//
// Phase A (#184957): textual FTXUI GMF headers + `import std;`
// (the textual FTXUI includes keep the reduced-BMI writer happy).
module;

#include <cctype>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

module cc.ui.screens.repl_screen;

import std;

import cc.ui.dialogs.settings_dialog;
import cc.config.config;
import cc.ui.dialogs.trust_dialog;
import cc.ui.permissions.permission_bash;
import cc.ui.permissions.permission_file_edit;
import cc.ui.permissions.permission_file_write;
import cc.ui.permissions.single_prompt;

namespace cc::ui::repl_screen {
using namespace ftxui;

namespace dialog_router {

// -------------------------------------------------------------------
// Settings dialog helpers (UI3)
// -------------------------------------------------------------------

namespace settings_ns = cc::ui::dialogs::settings_dialog;

using settings_ns::CommandResultDisplay;
using settings_ns::SettingsDialogOptions;
using settings_ns::SettingsTabId;
using settings_ns::MakeSettingsDialog;

/// Lazily create (or re-create) the settings dialog component.
/// If `state->settings_config` is non-null it is used for reads/writes,
/// otherwise a fresh internal ConfigManager is used (snapshot only).
[[nodiscard]] std::shared_ptr<Component> get_settings_component(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->settings_component) {
        // Use the supplied config manager, otherwise manufacture a default.
        static thread_local cc::core::ConfigManager fallback_config;
        cc::core::ConfigManager* cfg = s->settings_config
                                            ? static_cast<cc::core::ConfigManager*>(
                                                  s->settings_config)
                                            : &fallback_config;
        SettingsDialogOptions opts;
        opts.initial_tab = static_cast<SettingsTabId>(s->settings_initial_tab);
        opts.on_close = [s, cb](std::optional<std::string>, CommandResultDisplay) {
            s->mode = ReplMode::Normal;
            s->settings_component.reset();
            s->settings_initial_tab = 0;  // reset to General for next open
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        s->settings_component = std::make_shared<Component>(
            MakeSettingsDialog(*cfg, std::move(opts)));
    }
    return std::static_pointer_cast<Component>(s->settings_component);
}

/// Reset (destroy) the settings component so the next entry starts fresh
/// (empty dirty flag, pristine snapshot, tab=General).
void reset_settings_component(const std::shared_ptr<ReplScreenState>& s) {
    s->settings_component.reset();
    s->settings_initial_tab = 0;  // General
}

/// Render the settings dialog content as an Element.
[[nodiscard]] Element render_settings(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto comp = get_settings_component(s, cb);
    return comp ? (*comp)->Render() : text("");
}

/// Forward an event to the settings dialog component.
bool forward_settings(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto comp = get_settings_component(s, cb);
    return comp && (*comp)->OnEvent(std::move(ev));
}

// -------------------------------------------------------------------
// Trust dialog helpers (UI8)
// -------------------------------------------------------------------
//
// Trust dialog lives in the STANDALONE slot (full-takeover).  We also
// keep a ReplMode-driven path as a bridge for callers that haven't
// migrated to the queue yet.  The Component is lazy-created via
// MakeWorkspaceTrustDialog() and stored as an opaque handle so
// ReplScreenState doesn't need to import the trust_dialog types.

namespace trust_ns = cc::ui::trust_dialog;
using trust_ns::TrustChoice;
using trust_ns::WorkspaceTrustProps;
using trust_ns::SecuritySources;
using trust_ns::MakeWorkspaceTrustDialog;

/// Lazily create (or re-create) the trust dialog component.
[[nodiscard]] std::shared_ptr<Component> get_trust_dialog(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->wizard_trust) {
        WorkspaceTrustProps props;
        props.workspace_path = s->cwd.empty() ? "." : s->cwd;
        props.on_done = [s, cb](TrustChoice choice) {
            // Bridge choices onto the permission + exit callbacks.  The
            // enum actually ships 5 values (AllowOnce/AlwaysAllow/ViewFile/
            // Cancel/EnableAnyway) — the "Exit" variant is handled by the
            // on_exit callback directly inside the trust dialog component
            // when the user chooses the corresponding option, not via
            // choice enum here.
            using C = TrustChoice;
            if (cb->on_permission_response) {
                switch (choice) {
                  case C::AllowOnce:    cb->on_permission_response(true, false); break;
                  case C::AlwaysAllow:  cb->on_permission_response(true, true);  break;
                  case C::EnableAnyway: cb->on_permission_response(true, true);  break;
                  case C::ViewFile:     cb->on_permission_response(true, false); break;
                  case C::Cancel:
                  default:              cb->on_permission_response(false, std::nullopt); break;
                }
            }
            s->mode = ReplMode::Normal;
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
            s->wizard_trust.reset();
        };
        s->wizard_trust = std::make_shared<Component>(
            MakeWorkspaceTrustDialog(std::move(props)));
    }
    return std::static_pointer_cast<Component>(s->wizard_trust);
}

/// Render the trust dialog content as an Element.
[[nodiscard]] Element render_trust_dialog(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto d = get_trust_dialog(s, cb);
    return d ? (*d)->Render() : text("(trust dialog unavailable)");
}

/// Forward an event to the trust dialog component.
bool forward_trust_dialog(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto d = get_trust_dialog(s, cb);
    return d && (*d)->OnEvent(std::move(ev));
}

// -------------------------------------------------------------------
// Tool-permission rich panel helpers (dlg-permission-legacy)
// -------------------------------------------------------------------
// TS-faithful panels for the dormant ReplMode::ToolPermission branch.
// TS REF: PermissionRequest.tsx:47-82 dispatches on tool identity.
// wizard_trust ownership keeps Component/PromptState alive across frames.
namespace tperm_bash  = cc::ui::permissions::bash_prompt;
namespace tperm_edit  = cc::ui::permissions::file_edit;
namespace tperm_write = cc::ui::permissions::file_write;
namespace tperm_one   = cc::ui::permissions::single_prompt;

namespace {

enum class PermissionPanelKind { Bash, FileEdit, FileWrite, Generic };

// Case-insensitive tool classifier. Uses EXACT canonical names, not
// prefix/suffix matching: loose starts_with("bash")/ends_with("edit") would
// misdispatch unrelated tools ("bashful", "credit", "NotebookEdit",
// "MultiEdit", "BashOutputTool") to the wrong panel.
// TS canonical names: Bash (BashTool/toolName.ts), Edit
// (FileEditTool/constants.ts), Write (FileWriteTool/prompt.ts). NotebookEdit
// and MultiEdit are distinct TS tools with their own UI and must stay
// Generic here. A few historical CPP identifiers are kept as explicit
// aliases (not suffixes).
[[nodiscard]] PermissionPanelKind classify_permission_tool(
    std::string_view name) {
    std::string n;
    n.reserve(name.size());
    for (char c : name)
        n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    auto in = [&](std::initializer_list<const char*> names) {
        for (const char* x : names) {
            if (n == x) return true;
        }
        return false;
    };
    if (in({"bash"})) return PermissionPanelKind::Bash;
    if (in({"edit", "fileedit", "edittool", "fileedittool"}))
        return PermissionPanelKind::FileEdit;
    if (in({"write", "filewrite", "writetool", "filewritetool"}))
        return PermissionPanelKind::FileWrite;
    return PermissionPanelKind::Generic;
}

[[nodiscard]] tperm_one::RiskLevel tperm_risk(const PermissionRequestInfo& i) {
    std::string l = i.risk_labels.empty() ? "medium" : i.risk_labels.front();
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "low") return tperm_one::RiskLevel::Low;
    if (l == "high") return tperm_one::RiskLevel::High;
    if (l == "critical") return tperm_one::RiskLevel::Critical;
    return tperm_one::RiskLevel::Medium;
}

[[nodiscard]] std::string tperm_base(std::string_view p) {
    auto pos = p.find_last_of("/\\");
    return std::string{pos == std::string_view::npos ? p : p.substr(pos + 1)};
}

}  // namespace


// Lazily build the panel, keyed on request identity so focus/props are
// never reused across requests.  State is reset BEFORE signalling the
// blocked permission worker (app_agent_menu get_permission_callback).
// One-shot guard: the bash/edit/write panels invoke on_abort AND
// on_decide(Abort) on one Esc — the TS contract is one terminal reply.
[[nodiscard]] std::shared_ptr<Component> get_tool_permission_component(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    const auto& i = *s->permission_request;
    const std::string leaf = i.bash_command.value_or(i.file_path.value_or(""));
    const std::string key = i.tool_name + "\x1f" + i.description + "\x1f" + leaf;
    if (s->tool_permission_component && key == s->tool_permission_key)
        return std::static_pointer_cast<Component>(s->tool_permission_component);
    s->tool_permission_component.reset();

    auto fired = std::make_shared<bool>(false);
    auto respond = [s, cb, fired](bool ok, std::optional<bool> always) {
        if (*fired) return;
        *fired = true;
        if (cb->on_permission_response) cb->on_permission_response(ok, always);
        s->mode = ReplMode::Normal;
        s->permission_request.reset();
        s->tool_permission_component.reset();
        s->tool_permission_key.clear();
        if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
    };
    auto deny = [respond] { respond(false, std::nullopt); };

    const std::string path = i.file_path.value_or("");
    const std::string rel  = i.file_relative_path.value_or(path);
    const std::string base = i.file_filename.value_or(tperm_base(path));

    switch (classify_permission_tool(i.tool_name)) {
      case PermissionPanelKind::Bash: {
        tperm_bash::BashPromptProps p;
        p.command = i.bash_command.value_or("");
        if (!i.bash_command && !i.description.empty()) p.command = i.description;
        if (i.bash_working_dir) p.working_dir = i.bash_working_dir;
        if (!i.description.empty()) p.description = i.description;
        p.is_destructive = i.bash_is_destructive;
        p.destructive_reason = i.bash_destructive_reason;
        p.show_always_allow = i.can_always_allow;
        if (!p.command.empty()) {  // token + ":*", mirrors RenderBashPermissionPromptForTest (permission_bash.cppm)
            auto sp = p.command.find_first_of(" \t");
            p.editable_prefix = (sp == std::string::npos ? p.command : p.command.substr(0, sp)) + ":*";
        }
        p.on_decide = [respond](tperm_bash::Decision d, std::string_view, std::string_view) {
            using D = tperm_bash::Decision;
            if (d == D::AllowOnce) respond(true, false);
            else if (d == D::AllowWithPrefix) respond(true, true);
            else respond(false, std::nullopt);
        };
        p.on_abort = deny;
        s->tool_permission_component = std::make_shared<Component>(
            tperm_bash::MakeBashPermissionPrompt(std::move(p)));
        break;
      }
      case PermissionPanelKind::FileEdit: {
        tperm_edit::FileEditPermissionProps p;
        p.file_path = path;
        p.old_string = i.file_old_content.value_or("");
        p.new_string = i.file_new_content.value_or("");
        p.replace_all = i.file_replace_all;
        p.file_content = i.file_old_content.value_or("");  // never null; sparse diff must not throw
        p.relative_path = rel;
        p.filename = base;
        if (i.file_language) p.language = *i.file_language;
        p.on_decide = [respond](tperm_edit::Decision d, tperm_edit::SessionScope, std::string_view) {
            using D = tperm_edit::Decision;
            if (d == D::AllowOnce) respond(true, false);
            else if (d == D::AllowSession) respond(true, true);
            else respond(false, std::nullopt);
        };
        p.on_abort = deny;
        s->tool_permission_component = std::make_shared<Component>(
            tperm_edit::MakeFileEditPermissionPrompt(std::move(p)));
        break;
      }
      case PermissionPanelKind::FileWrite: {
        tperm_write::FileWritePermissionProps p;
        p.file_path = path;
        p.content = i.file_new_content.value_or("");
        p.old_content = i.file_old_content.value_or("");
        p.file_exists = i.file_exists;
        p.relative_path = rel;
        p.filename = base;
        if (i.file_language) p.language = *i.file_language;
        p.on_decide = [respond](tperm_write::Decision d, tperm_write::SessionScope, std::string_view) {
            using D = tperm_write::Decision;
            if (d == D::AllowOnce) respond(true, false);
            else if (d == D::AllowSession) respond(true, true);
            else respond(false, std::nullopt);
        };
        p.on_abort = deny;
        s->tool_permission_component = std::make_shared<Component>(
            tperm_write::MakeFileWritePermissionPrompt(std::move(p)));
        break;
      }
      case PermissionPanelKind::Generic: {
        tperm_one::SinglePromptProps p;
        p.tool_name = i.tool_name;
        p.action_kind = tperm_one::ActionKind::Other;
        p.risk_level = tperm_risk(i);
        p.description = i.description;
        if (i.file_path) p.affected_paths.push_back(*i.file_path);
        p.detail = tperm_one::DetailGeneric{i.description};
        p.on_decide = [respond](tperm_one::Decision d, bool /*sandbox_requested*/) {
            using D = tperm_one::Decision;
            if (d == D::AllowOnce) respond(true, false);
            else if (d == D::AlwaysAllow) respond(true, true);
            else respond(false, std::nullopt);
        };
        p.on_abort = deny;
        s->tool_permission_component = std::make_shared<Component>(
            tperm_one::MakeSinglePromptDialog(std::move(p)));
        break;
      }
    }
    s->tool_permission_key = key;
    return std::static_pointer_cast<Component>(s->tool_permission_component);
}

[[nodiscard]] Element render_tool_permission(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto d = get_tool_permission_component(s, cb);
    return d ? (*d)->Render() : text("");
}

bool forward_tool_permission(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb, Event ev) {
    auto d = get_tool_permission_component(s, cb);
    return d && (*d)->OnEvent(std::move(ev));
}

}  // namespace dialog_router

}  // namespace cc::ui::repl_screen
