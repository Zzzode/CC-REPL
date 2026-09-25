// swarm_backends_detect.cpp — implementation unit for cc.utils.swarm_backends
// (RFC 0001 Phase C batch 10). EnvironmentDetection and TeammateModeSnapshot:
// every member body plus the 7 + 4 out-of-line static data definitions
// (formerly static-inline in the class). import std supplies std::getenv.
module;

module cc.utils.swarm_backends;

import std;

namespace cc::utils::swarm_backends {

// ── EnvironmentDetection static data (7) ────────────────────────────────────

std::mutex EnvironmentDetection::mutex_;
bool EnvironmentDetection::tmux_cached_ = false;
bool EnvironmentDetection::is_inside_tmux_result_ = false;
bool EnvironmentDetection::iterm2_cached_ = false;
bool EnvironmentDetection::is_in_iterm2_result_ = false;
std::string EnvironmentDetection::original_tmux_env_;
std::string EnvironmentDetection::original_tmux_pane_;

// ── EnvironmentDetection members ────────────────────────────────────────────

bool EnvironmentDetection::is_inside_tmux_sync() {
    return !original_tmux_env_.empty();
}

bool EnvironmentDetection::is_inside_tmux() {
    std::lock_guard lock(mutex_);
    if (!tmux_cached_) {
        is_inside_tmux_result_ = !original_tmux_env_.empty();
        tmux_cached_ = true;
    }
    return is_inside_tmux_result_;
}

std::string_view EnvironmentDetection::get_leader_pane_id() {
    return original_tmux_pane_;
}

bool EnvironmentDetection::is_tmux_available() {
    return detail::command_available(TMUX_COMMAND);
}

bool EnvironmentDetection::is_in_iterm2() {
    std::lock_guard lock(mutex_);
    if (!iterm2_cached_) {
        is_in_iterm2_result_ = detect_iterm2();
        iterm2_cached_ = true;
    }
    return is_in_iterm2_result_;
}

bool EnvironmentDetection::is_it2_cli_available() {
    return detail::command_available(IT2_COMMAND);
}

void EnvironmentDetection::reset_cache() {
    std::lock_guard lock(mutex_);
    tmux_cached_ = false;
    iterm2_cached_ = false;
}

void EnvironmentDetection::capture_env(std::string_view tmux_env, std::string_view tmux_pane) {
    std::lock_guard lock(mutex_);
    original_tmux_env_ = std::string(tmux_env);
    original_tmux_pane_ = std::string(tmux_pane);
    // Reset cached detection so the new environment takes effect.
    tmux_cached_ = false;
    is_inside_tmux_result_ = false;
    iterm2_cached_ = false;
    is_in_iterm2_result_ = false;
}

bool EnvironmentDetection::detect_iterm2() {
    // Check TERM_PROGRAM and ITERM_SESSION_ID environment variables
    const char* term_program = std::getenv("TERM_PROGRAM");
    if (term_program && std::string_view(term_program) == "iTerm.app") {
        return true;
    }
    const char* iterm_session = std::getenv("ITERM_SESSION_ID");
    return iterm_session != nullptr && iterm_session[0] != '\0';
}

// ── TeammateModeSnapshot static data (4) ────────────────────────────────────

std::mutex TeammateModeSnapshot::mutex_;
bool TeammateModeSnapshot::captured_ = false;
TeammateMode TeammateModeSnapshot::captured_mode_ = TeammateMode::Auto;
std::optional<TeammateMode> TeammateModeSnapshot::cli_override_;

// ── TeammateModeSnapshot members ────────────────────────────────────────────

void TeammateModeSnapshot::set_cli_override(TeammateMode mode) {
    std::lock_guard lock(mutex_);
    cli_override_ = mode;
}

std::optional<TeammateMode> TeammateModeSnapshot::get_cli_override() {
    std::lock_guard lock(mutex_);
    return cli_override_;
}

void TeammateModeSnapshot::clear_cli_override(TeammateMode new_mode) {
    std::lock_guard lock(mutex_);
    cli_override_ = std::nullopt;
    captured_mode_ = new_mode;
}

void TeammateModeSnapshot::capture() {
    std::lock_guard lock(mutex_);
    if (cli_override_.has_value()) {
        captured_mode_ = *cli_override_;
    } else {
        // Default to Auto if not configured
        captured_mode_ = TeammateMode::Auto;
    }
    captured_ = true;
}

TeammateMode TeammateModeSnapshot::get() {
    std::lock_guard lock(mutex_);
    if (!captured_) {
        capture_unlocked();
    }
    return captured_mode_;
}

void TeammateModeSnapshot::capture_unlocked() {
    if (cli_override_.has_value()) {
        captured_mode_ = *cli_override_;
    } else {
        captured_mode_ = TeammateMode::Auto;
    }
    captured_ = true;
}

} // namespace cc::utils::swarm_backends
