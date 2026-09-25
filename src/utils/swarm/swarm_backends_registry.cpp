// swarm_backends_registry.cpp — implementation unit for cc.utils.swarm_backends
// (RFC 0001 Phase C batch 10). BackendRegistry's six out-of-line members and
// its six out-of-line static data definitions (formerly static-inline in the
// class). The five trivial locking accessors stay inline in the primary.
module;

module cc.utils.swarm_backends;

import std;

namespace cc::utils::swarm_backends {

// ── Static data (6) ─────────────────────────────────────────────────────────

std::mutex BackendRegistry::mutex_;
std::shared_ptr<PaneBackend> BackendRegistry::cached_backend_;
std::optional<BackendDetectionResult> BackendRegistry::cached_detection_result_;
std::shared_ptr<TeammateExecutor> BackendRegistry::cached_in_process_backend_;
std::shared_ptr<TeammateExecutor> BackendRegistry::cached_pane_executor_;
bool BackendRegistry::in_process_fallback_active_ = false;

// ── Members ─────────────────────────────────────────────────────────────────

BackendDetectionResult BackendRegistry::detect_and_get_backend() {
    std::lock_guard lock(mutex_);
    if (cached_detection_result_) return *cached_detection_result_;

    if (EnvironmentDetection::is_inside_tmux() && EnvironmentDetection::is_tmux_available()) {
        cached_backend_ = std::make_shared<TmuxBackend>();
        cached_detection_result_ = BackendDetectionResult{
            .backend_type = BackendType::Tmux,
            .is_native = true,
        };
        return *cached_detection_result_;
    }
    if (EnvironmentDetection::is_in_iterm2()) {
        if (EnvironmentDetection::is_it2_cli_available()) {
            cached_backend_ = std::make_shared<ITermBackend>();
            cached_detection_result_ = BackendDetectionResult{
                .backend_type = BackendType::ITerm2,
                .is_native = true,
            };
            return *cached_detection_result_;
        }
        cached_detection_result_ = BackendDetectionResult{
            .backend_type = BackendType::ITerm2,
            .is_native = true,
            .needs_it2_setup = true,
        };
        return *cached_detection_result_;
    }
    if (EnvironmentDetection::is_tmux_available()) {
        cached_backend_ = std::make_shared<TmuxBackend>();
        cached_detection_result_ = BackendDetectionResult{
            .backend_type = BackendType::Tmux,
            .is_native = false,
        };
        return *cached_detection_result_;
    }
    cached_detection_result_ = BackendDetectionResult{.backend_type = BackendType::InProcess};
    return *cached_detection_result_;
}

std::shared_ptr<PaneBackend> BackendRegistry::get_backend_by_type(PaneBackendType type) {
    switch (type) {
        case PaneBackendType::Tmux: return std::make_shared<TmuxBackend>();
        case PaneBackendType::ITerm2: return std::make_shared<ITermBackend>();
    }
    return std::make_shared<TmuxBackend>();
}

bool BackendRegistry::is_in_process_enabled() {
    if (auto forced = detail::forced_mode_from_env()) {
        return *forced == TeammateMode::InProcess;
    }
    auto mode = TeammateModeSnapshot::get();
    if (mode == TeammateMode::InProcess) return true;
    if (mode == TeammateMode::Tmux) return false;
    if (in_process_fallback_active_) return true;
    if (EnvironmentDetection::is_inside_tmux() || EnvironmentDetection::is_in_iterm2()) return false;
    return !EnvironmentDetection::is_tmux_available();
}

std::shared_ptr<TeammateExecutor> BackendRegistry::get_in_process_backend() {
    std::lock_guard lock(mutex_);
    if (!cached_in_process_backend_) {
        cached_in_process_backend_ = std::make_shared<InProcessBackend>();
    }
    return cached_in_process_backend_;
}

std::shared_ptr<TeammateExecutor> BackendRegistry::get_teammate_executor(bool prefer_in_process) {
    if (prefer_in_process || is_in_process_enabled()) return get_in_process_backend();
    auto detection = detect_and_get_backend();
    if (detection.backend_type == BackendType::InProcess || detection.needs_it2_setup || !cached_backend_) {
        mark_in_process_fallback();
        return get_in_process_backend();
    }
    std::lock_guard lock(mutex_);
    if (!cached_pane_executor_) {
        cached_pane_executor_ = std::make_shared<PaneBackendExecutor>(cached_backend_);
    }
    return cached_pane_executor_;
}

std::string BackendRegistry::get_tmux_install_instructions() {
    return "Install tmux or set LOOM_TEAMMATE_BACKEND=in-process to use in-process teammates.";
}

} // namespace cc::utils::swarm_backends
