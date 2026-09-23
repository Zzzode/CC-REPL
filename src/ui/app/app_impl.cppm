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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

export module cc.ui.app.app:impl;

import cc.ui.app.app;
import cc.vim.vim_mode;
import cc.hooks.exit_handler;

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

// AppImplDeleter: defined where AppImpl is complete so unique_ptr teardown
// needs no complete type in the constructor/destructor impl units.
void AppImplDeleter::operator()(AppImpl* p) const noexcept {
    delete p;
}

// Construct the backing state. Called from the out-of-line constructor.
void AppAdapter::construct_impl() {
    impl_.reset(new AppImpl());
}

}  // namespace cc::ui
