// app_impl.cppm — internal module interface PARTITION (:impl) for the PIMPL
// backing state of AppAdapter. The primary app.cppm only forward-declares
// AppImpl and holds a unique_ptr (without importing this partition), keeping
// its interface BMI free of the heavy modules this state depends on.
//
// Everything that needs AppImpl to be a complete type lives here: AppImpl's
// definition, the AppAdapter constructor/destructor (out-of-line here so the
// constructor body can initialize hidden state), plus lightweight accessors
// whose signatures don't leak the hidden member types. Other impl units call
// those accessors and never see AppImpl.
module;

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

export module cc.ui.app.app:impl;

import cc.ui.app.app;
import cc.vim.vim_mode;
import cc.hooks.exit_handler;
import cc.state.store;
import cc.state.app_state;
import cc.utils.settings_manager;

namespace cc::ui {

struct AppImpl {
    // Vim state.
    bool vim_enabled_ = false;
    cc::vim::VimStateMachine vim_sm_;

    // Ctrl-C double-press handler (TS useDoublePress, 800ms window).
    cc::hooks::ExitHandler exit_handler_{cc::hooks::ExitHandlerConfig{
        .require_double_press = true,
        .cleanup_timeout_ms = 5000,
        .save_on_exit = true,
        .double_press_window = std::chrono::milliseconds{800}}};

    // Redux-like AppState store for CommandContext bridging.
    std::shared_ptr<cc::state::AppStore> app_store_;

    // Settings manager (disk load + file-watch).
    std::unique_ptr<cc::utils::settings_manager::SettingsManager> settings_manager_;
    cc::utils::settings_manager::UnsubscribeFn settings_unsubscribe_;
};

// ── Vim accessors (keep VimMode/VimStateMachine out of the interface) ───────
bool AppAdapter::vim_enabled() const noexcept {
    return impl_ && impl_->vim_enabled_;
}

void AppAdapter::set_vim_enabled(bool on) {
    if (impl_) impl_->vim_enabled_ = on;
}

std::optional<std::string> AppAdapter::vim_statusline_label() const {
    if (!impl_ || !impl_->vim_enabled_) return std::nullopt;
    std::string label;
    switch (impl_->vim_sm_.get_mode()) {
        case cc::vim::VimMode::Normal:     label = "NORMAL"; break;
        case cc::vim::VimMode::Insert:     label = "INSERT"; break;
        case cc::vim::VimMode::Visual:     label = "VISUAL"; break;
        case cc::vim::VimMode::VisualLine: label = "VISUAL LINE"; break;
        case cc::vim::VimMode::Command:    label = "COMMAND"; break;
        case cc::vim::VimMode::Replace:    label = "REPLACE"; break;
        default:                           label = "INSERT"; break;
    }
    return label;
}

// ── Exit handler accessors (keep ExitReason/ExitHandler out of interface) ──
void AppAdapter::set_exit_message_impl(std::string_view msg) {
    if (impl_) impl_->exit_handler_.set_exit_message(std::string(msg));
}

void AppAdapter::reset_exit_handler() {
    if (impl_) impl_->exit_handler_.reset();
}

bool AppAdapter::handle_ctrl_c() {
    return impl_ && impl_->exit_handler_.handle_signal(
                        cc::hooks::ExitReason::ctrl_c);
}

// ── AppStore accessors (keep AppStore/AppState out of the interface) ────────
bool AppAdapter::has_app_store() const noexcept {
    return impl_ && static_cast<bool>(impl_->app_store_);
}

void* AppAdapter::app_store_raw() noexcept {
    return impl_ ? static_cast<void*>(impl_->app_store_.get()) : nullptr;
}

BridgeState AppAdapter::bridge_state() const {
    BridgeState b{false, false, false, false, false};
    if (impl_ && impl_->app_store_) {
        const auto st = impl_->app_store_->get_state();
        b.enabled        = st.repl_bridge_enabled;
        b.explicit_remote = st.repl_bridge_explicit;
        b.connected      = st.repl_bridge_connected;
        b.session_active = st.repl_bridge_session_active;
        b.reconnecting   = st.repl_bridge_reconnecting;
    }
    return b;
}

// ── Settings accessors (keep SettingsManager/SettingsJson out of interface)
void AppAdapter::init_settings_manager() {
    if (!impl_) return;
    impl_->settings_manager_ =
        std::make_unique<cc::utils::settings_manager::SettingsManager>();
    impl_->settings_manager_->initialize();
}

void AppAdapter::subscribe_settings_changed(std::function<void()> cb) {
    if (!impl_ || !impl_->settings_manager_) return;
    impl_->settings_unsubscribe_ =
        impl_->settings_manager_->on_change([cb = std::move(cb)](
            cc::utils::settings_manager::SettingSource) mutable { cb(); });
}

std::optional<std::string> AppAdapter::setting_string(std::string_view key) const {
    if (!impl_ || !impl_->settings_manager_) return std::nullopt;
    auto settings = impl_->settings_manager_->get_initial_settings();
    auto it = settings.find(std::string(key));
    if (it != settings.end() &&
        std::holds_alternative<std::string>(it->second)) {
        return std::get<std::string>(it->second);
    }
    return std::nullopt;
}

std::optional<std::string> AppAdapter::statusline_setting(std::string_view key) const {
    if (!impl_ || !impl_->settings_manager_) return std::nullopt;
    auto settings = impl_->settings_manager_->get_initial_settings();
    auto sl = settings.find("statusLine");
    if (sl == settings.end() ||
        !std::holds_alternative<std::map<std::string, std::string>>(sl->second)) {
        return std::nullopt;
    }
    const auto& m = std::get<std::map<std::string, std::string>>(sl->second);
    auto it = m.find(std::string(key));
    return it != m.end() ? std::optional<std::string>{it->second} : std::nullopt;
}

std::string AppAdapter::output_style_setting() const {
    return setting_string("outputStyle").value_or("full");
}

void AppAdapter::ProjectSettingsToScreenState() {
    if (!impl_ || !impl_->settings_manager_) return;

    // --- default model ---
    screen_state_->settings_model = setting_string("model").value_or(std::string{});

    // --- default agent display name ---
    screen_state_->settings_agent_name =
        setting_string("agent").value_or(std::string{});

    // --- status line config (settings.statusLine) ---
    std::optional<std::string> status_line_type = statusline_setting("type");
    std::string status_line_command  = statusline_setting("command").value_or(std::string{});
    std::optional<bool> status_line_enabled;
    int status_line_padding = 0;

    if (auto enabled = statusline_setting("enabled")) {
        status_line_enabled = parse_bool_text(*enabled);
    }
    if (auto pad = statusline_setting("padding")) {
        if (auto parsed = parse_int_text(*pad)) status_line_padding = *parsed;
    }

    if (auto command = first_non_empty_env({
            "LOOM_STATUS_LINE_COMMAND",
            "LOOM_STATUS_LINE_COMMAND"})) {
        status_line_command = *command;
        status_line_type = "command";
    }
    if (auto enabled = first_non_empty_env({
            "LOOM_STATUS_LINE_ENABLED",
            "LOOM_STATUS_LINE_ENABLED"})) {
        status_line_enabled = parse_bool_text(*enabled);
    }
    if (auto padding = first_non_empty_env({
            "LOOM_STATUS_LINE_PADDING",
            "LOOM_STATUS_LINE_PADDING"})) {
        if (auto parsed = parse_int_text(*padding)) {
            status_line_padding = *parsed;
        }
    }

    const bool type_allows_command = !status_line_type || *status_line_type == "command";
    const bool enabled = status_line_enabled.value_or(
        !status_line_command.empty() && type_allows_command);
    screen_state_->status_line_command = std::move(status_line_command);
    screen_state_->status_line_padding = status_line_padding;
    screen_state_->status_line_enabled =
        enabled && type_allows_command && !screen_state_->status_line_command.empty();
    if (!screen_state_->status_line_enabled) {
        screen_state_->status_line_text.clear();
    }
}

// AppImplDeleter: defined where AppImpl is complete so unique_ptr teardown
// needs no complete type in the constructor/destructor impl units.
void AppImplDeleter::operator()(AppImpl* p) const noexcept {
    delete p;
}

// Construct the backing state. Called from the out-of-line constructor.
void AppAdapter::construct_impl() {
    impl_.reset(new AppImpl());
    impl_->app_store_ = cc::state::create_app_store();
}

}  // namespace cc::ui
