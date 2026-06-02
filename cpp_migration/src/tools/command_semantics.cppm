module;
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <sstream>

export module cc.tools.command_semantics;

export namespace cc::tools {

// 命令的语义类型分类
enum class CommandType {
    Read,         // 只读操作（ls, cat, grep 等）
    Write,        // 写操作（写文件、修改配置等）
    Execute,      // 执行/构建操作（编译、运行脚本等）
    Network,      // 网络操作（curl, wget, ssh 等）
    Destructive,  // 破坏性操作（rm, format 等）
    Unknown       // 无法分类
};

// 对命令进行语义分类
inline auto classify_command(std::string_view command) -> CommandType {
    // 提取命令的基础部分（去除前导空格）
    auto trimmed = command;
    while (!trimmed.empty() && trimmed.front() == ' ') {
        trimmed.remove_prefix(1);
    }

    // 破坏性命令优先检查
    static const std::vector<std::string_view> destructive_cmds = {
        "rm", "rmdir", "mkfs", "format", "dd ", "shred", "wipefs"
    };
    for (const auto& cmd : destructive_cmds) {
        if (trimmed.starts_with(cmd) &&
            (trimmed.size() == cmd.size() || trimmed[cmd.size()] == ' ')) {
            return CommandType::Destructive;
        }
    }

    // 网络操作
    static const std::vector<std::string_view> network_cmds = {
        "curl", "wget", "ssh", "scp", "sftp", "rsync",
        "ping", "traceroute", "nslookup", "dig",
        "nc", "netcat", "telnet", "ftp",
        "git clone", "git fetch", "git pull", "git push",
        "npm publish", "docker push", "docker pull"
    };
    for (const auto& cmd : network_cmds) {
        if (trimmed.starts_with(cmd) &&
            (trimmed.size() == cmd.size() || trimmed[cmd.size()] == ' ')) {
            return CommandType::Network;
        }
    }

    // 写操作
    static const std::vector<std::string_view> write_cmds = {
        "cp", "mv", "mkdir", "touch", "chmod", "chown",
        "tee", "sed -i", "git add", "git commit",
        "npm install", "pip install", "brew install",
        "apt install", "apt-get install"
    };
    for (const auto& cmd : write_cmds) {
        if (trimmed.starts_with(cmd) &&
            (trimmed.size() == cmd.size() || trimmed[cmd.size()] == ' ')) {
            return CommandType::Write;
        }
    }
    // 重定向写入检测
    if (command.find('>') != std::string_view::npos) {
        return CommandType::Write;
    }

    // 执行/构建操作
    static const std::vector<std::string_view> execute_cmds = {
        "make", "cmake", "cargo build", "cargo run",
        "go build", "go run", "python", "node",
        "bun run", "npm run", "npx", "yarn",
        "gcc", "g++", "clang", "rustc", "javac",
        "docker run", "docker compose", "kubectl"
    };
    for (const auto& cmd : execute_cmds) {
        if (trimmed.starts_with(cmd) &&
            (trimmed.size() == cmd.size() || trimmed[cmd.size()] == ' ')) {
            return CommandType::Execute;
        }
    }

    // 只读操作
    static const std::vector<std::string_view> read_cmds = {
        "ls", "cat", "head", "tail", "less", "more",
        "find", "grep", "rg", "ag", "fd",
        "wc", "file", "stat", "du", "df",
        "tree", "which", "where", "type",
        "echo", "printf", "date", "whoami", "pwd",
        "env", "printenv", "uname", "id",
        "git status", "git log", "git diff", "git show",
        "git branch", "ps", "top", "htop"
    };
    for (const auto& cmd : read_cmds) {
        if (trimmed.starts_with(cmd) &&
            (trimmed.size() == cmd.size() || trimmed[cmd.size()] == ' ')) {
            return CommandType::Read;
        }
    }

    return CommandType::Unknown;
}

// 提取命令中涉及的文件路径
inline auto get_affected_paths(std::string_view command) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> paths;

    // 简单的路径提取逻辑：按空格分词，识别路径模式
    std::istringstream stream{std::string(command)};
    std::string token;
    bool skip_next = false;

    while (stream >> token) {
        if (skip_next) {
            skip_next = false;
            continue;
        }
        // 跳过 flag 参数
        if (token.starts_with("-")) {
            // 带值的 flag（如 -o output.txt）需要跳过下一个 token
            if (token == "-o" || token == "-f" || token == "-d") {
                skip_next = true;
            }
            continue;
        }
        // 检测像路径的 token
        if (token.find('/') != std::string::npos ||
            token.find('.') != std::string::npos) {
            // 跳过命令本身（第一个 token）
            if (&token == &token) { // 简单排除常见命令名
                paths.emplace_back(token);
            }
        }
    }

    return paths;
}

// 判断是否为 git 命令
inline auto is_git_command(std::string_view command) -> bool {
    auto trimmed = command;
    while (!trimmed.empty() && trimmed.front() == ' ') {
        trimmed.remove_prefix(1);
    }
    return trimmed.starts_with("git ") || trimmed == "git";
}

// 判断是否为包管理器命令
inline auto is_package_manager_command(std::string_view command) -> bool {
    auto trimmed = command;
    while (!trimmed.empty() && trimmed.front() == ' ') {
        trimmed.remove_prefix(1);
    }

    static const std::vector<std::string_view> pm_prefixes = {
        "npm", "yarn", "pnpm", "bun",      // JS/TS
        "pip", "pip3", "pipenv", "poetry",  // Python
        "cargo",                             // Rust
        "go get", "go install",             // Go
        "brew", "apt", "apt-get", "dnf",   // System
        "gem", "bundle",                     // Ruby
        "composer",                          // PHP
        "nuget", "dotnet add"               // .NET
    };

    return std::any_of(pm_prefixes.begin(), pm_prefixes.end(),
        [&](std::string_view prefix) {
            return trimmed.starts_with(prefix) &&
                   (trimmed.size() == prefix.size() || trimmed[prefix.size()] == ' ');
        });
}

// 基于命令语义估算执行时间
inline auto estimate_duration(std::string_view command) -> std::chrono::seconds {
    auto type = classify_command(command);

    switch (type) {
        case CommandType::Read:
            return std::chrono::seconds{1};
        case CommandType::Write:
            return std::chrono::seconds{3};
        case CommandType::Execute: {
            // 编译和构建通常耗时较长
            if (command.find("build") != std::string_view::npos ||
                command.find("compile") != std::string_view::npos) {
                return std::chrono::seconds{30};
            }
            return std::chrono::seconds{10};
        }
        case CommandType::Network:
            return std::chrono::seconds{15};
        case CommandType::Destructive:
            return std::chrono::seconds{5};
        case CommandType::Unknown:
            return std::chrono::seconds{10};
    }
    return std::chrono::seconds{10};
}

} // namespace cc::tools
