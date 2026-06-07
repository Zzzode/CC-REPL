/// @file ui.cppm
/// @brief Bridge UI utilities for status display and user interaction
module;

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>
#include <format>
#include <unordered_map>
#include <iostream>

export module cc.bridge.ui;

import cc.types.types;
import cc.bridge.config;

export namespace cc::bridge {

/// Bridge status states
enum class StatusState {
    Idle,
    Attached,
    Titled,
    Reconnecting,
    Failed
};

/// Bridge UI logger
class BridgeLogger {
    std::function<void(const std::string&)> writer_;
    bool verbose_;
    StatusState current_state_ = StatusState::Idle;
    std::string current_state_text_ = "Ready";
    std::string repo_name_;
    std::string branch_name_;
    std::string debug_log_path_;
    std::string connect_url_;
    std::string cached_ingress_url_;
    std::string cached_environment_id_;
    std::optional<std::string> active_session_url_;
    bool qr_visible_ = false;
    std::vector<std::string> qr_lines_;
    int session_active_count_ = 0;
    int session_max_count_ = 1;
    std::string spawn_mode_;
    int connecting_tick_ = 0;
    bool connecting_active_ = false;
    
    // Session display info
    struct SessionDisplayInfo {
        std::optional<std::string> title;
        std::string url;
        std::string activity;
    };
    std::unordered_map<std::string, SessionDisplayInfo> session_display_info_;

public:
    explicit BridgeLogger(bool verbose = false, 
                         std::function<void(const std::string&)> writer = nullptr)
        : verbose_(verbose), writer_(std::move(writer)) {}

    /// Print banner with connection info
    void print_banner(const BridgeConfig& config, const std::string& environment_id) {
        cached_ingress_url_ = std::format("http://{}:{}", config.host, config.port);
        cached_environment_id_ = environment_id;
        connect_url_ = build_bridge_connect_url(environment_id, cached_ingress_url_);
        
        if (verbose_) {
            write_line("Remote Control v1.0.0");
            write_line(std::format("Transport: {}", static_cast<int>(config.transport)));
            write_line(std::format("Environment ID: {}", environment_id));
        }
        write_line("");
        
        // Start connecting spinner
        start_connecting();
    }

    /// Log session start
    void log_session_start(const std::string& session_id, const std::string& prompt) {
        if (verbose_) {
            auto short_prompt = truncate_prompt(prompt, 80);
            print_log(std::format("[{}] Session started: \"{}\" ({})", 
                                 timestamp(), short_prompt, session_id));
        }
    }

    /// Log session completion
    void log_session_complete(const std::string& session_id, int64_t duration_ms) {
        print_log(std::format("[{}] Session {} ({})", 
                             timestamp(), "completed", format_duration(duration_ms), session_id));
    }

    /// Log session failure
    void log_session_failed(const std::string& session_id, const std::string& error) {
        print_log(std::format("[{}] Session {}: {} ({})", 
                             timestamp(), "failed", error, session_id));
    }

    /// Log status message
    void log_status(const std::string& message) {
        print_log(std::format("[{}] {}", timestamp(), message));
    }

    /// Log verbose message
    void log_verbose(const std::string& message) {
        if (verbose_) {
            print_log(std::format("[{}] {}", timestamp(), message));
        }
    }

    void debug(std::string_view message) {
        log_verbose(std::string(message));
    }

    void warn(std::string_view message) {
        print_log(std::format("[{}] Warning: {}", timestamp(), message));
    }

    /// Log error message
    void log_error(const std::string& message) {
        print_log(std::format("[{}] Error: {}", timestamp(), message));
    }

    /// Log reconnection
    void log_reconnected(int64_t disconnected_ms) {
        print_log(std::format("[{}] {} after {}", 
                             timestamp(), "Reconnected", format_duration(disconnected_ms)));
    }

    /// Set repository info
    void set_repo_info(const std::string& repo, const std::string& branch) {
        repo_name_ = repo;
        branch_name_ = branch;
    }

    /// Set debug log path
    void set_debug_log_path(const std::string& path) {
        debug_log_path_ = path;
    }

    /// Update to idle status
    void update_idle_status() {
        stop_connecting();
        current_state_ = StatusState::Idle;
        current_state_text_ = "Ready";
        active_session_url_ = std::nullopt;
        regenerate_qr(connect_url_);
        render_status_line();
    }

    /// Set attached status
    void set_attached(const std::string& session_id) {
        stop_connecting();
        current_state_ = StatusState::Attached;
        current_state_text_ = "Connected";
        if (session_max_count_ <= 1) {
            active_session_url_ = build_bridge_session_url(session_id, 
                                                          cached_environment_id_, 
                                                          cached_ingress_url_);
            regenerate_qr(*active_session_url_);
        }
        render_status_line();
    }

    /// Update reconnecting status
    void update_reconnecting_status(const std::string& delay_str, 
                                   const std::string& elapsed_str) {
        stop_connecting();
        clear_status_lines();
        current_state_ = StatusState::Reconnecting;
        
        std::string suffix;
        if (!repo_name_.empty()) {
            suffix += " \u00b7 " + repo_name_;
        }
        if (!branch_name_.empty()) {
            suffix += " \u00b7 " + branch_name_;
        }
        
        write_line(std::format("{} Reconnecting \u00b7 retrying in {} \u00b7 disconnected {}", 
                             get_spinner_frame(), delay_str, elapsed_str));
    }

    /// Update failed status
    void update_failed_status(const std::string& error) {
        stop_connecting();
        clear_status_lines();
        current_state_ = StatusState::Failed;
        
        std::string suffix;
        if (!repo_name_.empty()) {
            suffix += " \u00b7 " + repo_name_;
        }
        if (!branch_name_.empty()) {
            suffix += " \u00b7 " + branch_name_;
        }
        
        write_line(std::format("✗ Remote Control Failed{}", suffix));
        write_line(std::string(FAILED_FOOTER_TEXT));
        if (!error.empty()) {
            write_line(error);
        }
    }

    /// Update session status with activity
    void update_session_status(const std::string& session_id, 
                              int64_t elapsed_ms,
                              const std::string& activity_type,
                              const std::vector<std::string>& trail) {
        auto& info = session_display_info_[session_id];
        info.activity = std::format("{} · {}", activity_type, format_duration(elapsed_ms));
        for (const auto& entry : trail) {
            if (!entry.empty()) info.activity += " › " + truncate_prompt(entry, 24);
        }
        render_status_line();
    }

    /// Clear status display
    void clear_status() {
        stop_connecting();
        clear_status_lines();
    }

    /// Toggle QR code visibility
    void toggle_qr() {
        qr_visible_ = !qr_visible_;
        render_status_line();
    }

    /// Update session count
    void update_session_count(int active, int max, const std::string& mode) {
        session_active_count_ = active;
        session_max_count_ = max;
        spawn_mode_ = mode;
        // Don't re-render here - status ticker will handle it
    }

    /// Add a session to display
    void add_session(const std::string& session_id, const std::string& url) {
        session_display_info_[session_id] = SessionDisplayInfo{std::nullopt, url, {}};
    }

    /// Update session activity
    void update_session_activity(const std::string& session_id, 
                                const std::string& activity) {
        auto it = session_display_info_.find(session_id);
        if (it != session_display_info_.end()) {
            it->second.activity = activity;
            render_status_line();
        }
    }

    /// Set session title
    void set_session_title(const std::string& session_id, const std::string& title) {
        auto it = session_display_info_.find(session_id);
        if (it != session_display_info_.end()) {
            it->second.title = title;
            if (current_state_ == StatusState::Reconnecting || 
                current_state_ == StatusState::Failed) {
                return;
            }
            if (session_max_count_ == 1) {
                current_state_ = StatusState::Titled;
                current_state_text_ = truncate_prompt(title, 40);
            }
            render_status_line();
        }
    }

    /// Remove a session from display
    void remove_session(const std::string& session_id) {
        session_display_info_.erase(session_id);
    }

    /// Refresh display
    void refresh_display() {
        if (current_state_ == StatusState::Reconnecting || 
            current_state_ == StatusState::Failed) {
            return;
        }
        render_status_line();
    }

private:
    /// Write a line to output
    void write_line(const std::string& line) {
        if (writer_) {
            writer_(line + "\n");
        } else {
            // Default to stdout
            std::cout << line << std::endl;
        }
    }

    /// Print a permanent log line
    void print_log(const std::string& line) {
        clear_status_lines();
        write_line(line);
    }

    /// Start connecting spinner
    void start_connecting() {
        connecting_tick_ = 0;
        connecting_active_ = true;
        render_connecting_line();
    }

    /// Stop connecting spinner
    void stop_connecting() {
        connecting_active_ = false;
    }

    /// Render connecting line
    void render_connecting_line() {
        clear_status_lines();
        std::string suffix;
        if (!repo_name_.empty()) {
            suffix += " \u00b7 " + repo_name_;
        }
        if (!branch_name_.empty()) {
            suffix += " \u00b7 " + branch_name_;
        }
        write_line(std::format("{} Connecting{}", 
                             get_spinner_frame(), suffix));
    }

    /// Render status line
    void render_status_line() {
        if (current_state_ == StatusState::Reconnecting || 
            current_state_ == StatusState::Failed) {
            return;
        }
        
        clear_status_lines();
        
        bool is_idle = current_state_ == StatusState::Idle;
        
        // QR code above status line
        if (qr_visible_) {
            for (const auto& line : qr_lines_) {
                write_line(line);
            }
        }
        
        // Determine indicator and colors
        std::string indicator = "✓";
        std::string state_text = current_state_text_;
        
        // Build suffix with repo and branch
        std::string suffix;
        if (!repo_name_.empty()) {
            suffix += " \u00b7 " + repo_name_;
        }
        if (!branch_name_.empty() && spawn_mode_ != "worktree") {
            suffix += " \u00b7 " + branch_name_;
        }
        
        if (verbose_ && !debug_log_path_.empty()) {
            write_line(std::format("[ANT-ONLY] Logs: {}", debug_log_path_));
        }
        
        write_line(std::format("{} {}{}", indicator, state_text, suffix));
        
        // Session count and per-session list (multi-session mode only)
        if (session_max_count_ > 1) {
            std::string mode_hint = spawn_mode_ == "worktree" 
                ? "New sessions will be created in an isolated worktree"
                : "New sessions will be created in the current directory";
            write_line(std::format("    Capacity: {}/{} \u00b7 {}", 
                                  session_active_count_, session_max_count_, mode_hint));
            
            for (const auto& [sid, info] : session_display_info_) {
                std::string title_text = info.title 
                    ? truncate_prompt(*info.title, 35)
                    : "Attached";
                write_line(std::format("    {}", title_text));
            }
        }
        
        // Mode line for single slot or single-session mode
        if (session_max_count_ == 1) {
            std::string mode_text;
            if (spawn_mode_ == "single-session") {
                mode_text = "Single session \u00b7 exits when complete";
            } else if (spawn_mode_ == "worktree") {
                mode_text = std::format("Capacity: {}/1 \u00b7 New sessions will be created in an isolated worktree", 
                                       session_active_count_);
            } else {
                mode_text = std::format("Capacity: {}/1 \u00b7 New sessions will be created in the current directory", 
                                       session_active_count_);
            }
            write_line(std::format("    {}", mode_text));
        }
        
        // Blank line separator before footer
        auto url = active_session_url_ ? *active_session_url_ : connect_url_;
        if (!url.empty()) {
            write_line("");
            std::string footer_text = is_idle 
                ? build_idle_footer_text(url)
                : build_active_footer_text(url);
            std::string qr_hint = qr_visible_ ? "space to hide QR code" : "space to show QR code";
            write_line(footer_text);
            write_line(qr_hint);
        }
    }

    /// Clear status lines
    void clear_status_lines() {
        if (writer_) {
            writer_("\r\033[2K");
        }
    }

    /// Get spinner frame
    std::string get_spinner_frame() const {
        static const std::vector<std::string> frames = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
        return frames[connecting_tick_ % frames.size()];
    }

    /// Regenerate QR code
    void regenerate_qr(const std::string& url) {
        qr_lines_.clear();
        if (url.empty()) return;
        qr_lines_.push_back("┌──────────────────────────────┐");
        qr_lines_.push_back("│ Open remote session URL:      │");
        qr_lines_.push_back(std::format("│ {} │", truncate_prompt(url, 28)));
        qr_lines_.push_back("└──────────────────────────────┘");
    }

    // Helper functions from bridgeStatusUtil
    static std::string timestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        return std::format("{:02d}:{:02d}:{:02d}", 
                          tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    static std::string format_duration(int64_t ms) {
        if (ms < 60000) {
            return std::format("{}s", (ms + 500) / 1000);
        }
        int minutes = ms / 60000;
        int seconds = (ms % 60000 + 500) / 1000;
        if (seconds > 0) {
            return std::format("{}m {}s", minutes, seconds);
        }
        return std::format("{}m", minutes);
    }

    static std::string truncate_prompt(std::string_view prompt, size_t max_len) {
        if (prompt.length() <= max_len) {
            return std::string(prompt);
        }
        return std::string(prompt.substr(0, max_len - 1)) + "…";
    }

    static std::string build_bridge_connect_url(const std::string& environment_id, 
                                               const std::string& ingress_url) {
        const auto base = ingress_url.empty() ? std::string("https://claude.ai") : ingress_url;
        return base + "/code?bridge=" + environment_id;
    }

    static std::string build_bridge_session_url(const std::string& session_id,
                                                const std::string& environment_id,
                                                const std::string& ingress_url) {
        const auto base = ingress_url.empty() ? std::string("https://claude.ai") : ingress_url;
        return base + "/session/" + session_id + "?bridge=" + environment_id;
    }

    static std::string build_idle_footer_text(const std::string& url) {
        return std::format("Code everywhere with the Claude app or {}", url);
    }

    static std::string build_active_footer_text(const std::string& url) {
        return std::format("Continue coding in the Claude app or {}", url);
    }

    static constexpr std::string_view FAILED_FOOTER_TEXT = 
        "Something went wrong, please try again";
};

} // namespace cc::bridge
