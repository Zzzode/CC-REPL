module;
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <expected>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <csignal>

#ifdef __unix__
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

export module cc.cli.bg;

export namespace cc::cli {

inline std::string get_pid_directory();
inline void record_process(int pid, std::string_view command);

// Information about a background process
struct BackgroundProcessInfo {
    int pid;
    std::string command;
    std::chrono::system_clock::time_point started;
};

// Launch a command in the background, returning its PID
std::expected<int, std::string> run_in_background(std::string_view command, std::span<std::string> args) {
    if (command.empty()) {
        return std::unexpected("Command cannot be empty");
    }

#ifdef __unix__
    // Fork and exec the command as a background process
    pid_t pid = fork();

    if (pid < 0) {
        return std::unexpected("Failed to fork process");
    }

    if (pid == 0) {
        // Child process: detach from terminal
        setsid();

        // Build argv for exec
        std::vector<const char*> argv;
        std::string cmd_str(command);
        argv.push_back(cmd_str.c_str());
        std::vector<std::string> arg_copies(args.begin(), args.end());
        for (auto& arg : arg_copies) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        // Redirect stdio to /dev/null
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        execvp(cmd_str.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127); // exec failed
    }

    // Parent: record the background process
    record_process(pid, command);
    return static_cast<int>(pid);
#else
    return std::unexpected("Background processes not supported on this platform");
#endif
}

// Check if the current process is running in background mode
bool is_background_process() {
#ifdef __unix__
    // A process is "background" if it has no controlling terminal
    return (getpgrp() == getpid() && isatty(STDIN_FILENO) == 0);
#else
    return false;
#endif
}

// Daemonize the current process
std::expected<void, std::string> daemonize() {
#ifdef __unix__
    pid_t pid = fork();
    if (pid < 0) {
        return std::unexpected("First fork failed");
    }
    if (pid > 0) {
        // Parent exits
        _exit(0);
    }

    // Create new session
    if (setsid() < 0) {
        return std::unexpected("setsid failed");
    }

    // Second fork to prevent re-acquiring a terminal
    pid = fork();
    if (pid < 0) {
        return std::unexpected("Second fork failed");
    }
    if (pid > 0) {
        _exit(0);
    }

    // Set working directory to root
    if (chdir("/") < 0) {
        return std::unexpected("chdir to / failed");
    }

    // Close and redirect file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Set file creation mask
    umask(0);

    return {};
#else
    return std::unexpected("Daemonize not supported on this platform");
#endif
}

// Get list of tracked background processes
std::vector<BackgroundProcessInfo> get_background_processes() {
    std::vector<BackgroundProcessInfo> processes;

    // Read from the PID tracking file
    std::string pid_dir = get_pid_directory();
    if (!std::filesystem::exists(pid_dir)) {
        return processes;
    }

    for (const auto& entry : std::filesystem::directory_iterator(pid_dir)) {
        if (!entry.is_regular_file()) continue;

        std::ifstream ifs(entry.path());
        if (!ifs.is_open()) continue;

        BackgroundProcessInfo info;
        std::string pid_str;
        if (std::getline(ifs, pid_str)) {
            try {
                info.pid = std::stoi(pid_str);
            } catch (...) {
                continue;
            }
        }
        std::getline(ifs, info.command);

        // Check if process is still running
#ifdef __unix__
        if (kill(info.pid, 0) != 0) {
            // Process no longer exists, skip
            continue;
        }
#endif
        const auto modified = std::filesystem::last_write_time(entry.path());
        info.started = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            modified - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
        processes.push_back(std::move(info));
    }

    return processes;
}

// Internal: get PID file directory
inline std::string get_pid_directory() {
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.config/claude-code/pids";
    }
    return "/tmp/claude-code-pids";
}

// Internal: record a background process PID
inline void record_process(int pid, std::string_view command) {
    std::string pid_dir = get_pid_directory();
    std::filesystem::create_directories(pid_dir);

    std::string pid_file = pid_dir + "/" + std::to_string(pid) + ".pid";
    std::ofstream ofs(pid_file);
    if (ofs.is_open()) {
        ofs << pid << "\n" << command << "\n";
    }
}

} // namespace cc::cli
