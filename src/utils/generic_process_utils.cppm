module;

#include <string>
#include <optional>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <cerrno>

#ifdef __APPLE__
#include <libproc.h>
#include <sys/sysctl.h>
#else
#include <fstream>
#include <filesystem>
#endif

export module cc.utils.generic_process_utils;

export namespace cc::utils {

// Get the current process ID
inline int get_pid() {
    return static_cast<int>(getpid());
}

// Get the parent process ID
inline int get_parent_pid() {
    return static_cast<int>(getppid());
}

// Check if a process is alive by sending signal 0
inline bool is_process_alive(int pid) {
    if (pid <= 0) return false;
    return kill(static_cast<pid_t>(pid), 0) == 0;
}

// Send a signal to a process (default SIGTERM)
inline bool kill_process(int pid, int signal = SIGTERM) {
    if (pid <= 0) return false;
    return kill(static_cast<pid_t>(pid), signal) == 0;
}

// Kill an entire process tree rooted at the given PID
inline bool kill_process_tree(int pid) {
    if (pid <= 0) return false;

    // Send SIGTERM to the process group (negative pid)
    // This works if the process is a process group leader
    int result = kill(-static_cast<pid_t>(pid), SIGTERM);

    // Also kill the process itself
    if (result != 0) {
        result = kill(static_cast<pid_t>(pid), SIGTERM);
    }

    // If SIGTERM didn't work, escalate to SIGKILL
    if (is_process_alive(pid)) {
        kill(-static_cast<pid_t>(pid), SIGKILL);
        kill(static_cast<pid_t>(pid), SIGKILL);
    }

    return !is_process_alive(pid);
}

// Get the name of a process by PID (platform-specific)
inline std::optional<std::string> get_process_name(int pid) {
    if (pid <= 0) return std::nullopt;

#ifdef __APPLE__
    char name[PROC_PIDPATHINFO_MAXSIZE];
    int ret = proc_name(pid, name, sizeof(name));
    if (ret > 0) {
        return std::string(name);
    }
    return std::nullopt;
#else
    // Linux: read from /proc/<pid>/comm
    std::string proc_path = "/proc/" + std::to_string(pid) + "/comm";
    std::ifstream file(proc_path);
    if (!file.is_open()) return std::nullopt;

    std::string name;
    std::getline(file, name);
    if (name.empty()) return std::nullopt;
    return name;
#endif
}

} // namespace cc::utils
