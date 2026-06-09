// Command semantics - command type classification + exit-code interpretation
//
// Combines two related concerns that live in separate TS source files:
//   - Permission/type classification (Read/Write/Execute/Network/Destructive)
//     used by bash_permissions / bash_security layers.
//   - Exit-code semantic interpretation (grep exit 1 == no-match, not error)
//     used by BashTool to build return_code_interpretation messages.
//
// NOTE: React JSX rendering of result cards is deferred to Phase 4 (FTXUI).
module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module cc.tools.command_semantics;

import cc.utils.format;

export namespace cc::tools {

// ---------------------------------------------------------------------------
// 1. Command-type classification (previously the only thing in this module).
//    Used by bash_permissions / bash_security to decide approval prompts.
// ---------------------------------------------------------------------------

enum class CommandType {
    Read,
    Write,
    Execute,
    Network,
    Destructive,
    Unknown
};

// Helper: skip leading whitespace
constexpr std::string_view trim_left(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    return s;
}

// Helper: case-insensitive start-of-command match with trailing boundary.
// `boundary_chars` define what counts as a separator after the matched prefix.
constexpr bool matches_command(std::string_view haystack, std::string_view prefix) {
    if (haystack.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (haystack[i] != prefix[i]) return false;
    }
    if (haystack.size() == prefix.size()) return true;
    const char c = haystack[prefix.size()];
    // Boundary: space, tab, semicolon, pipe, amp, redirect, end-of-part
    return c == ' ' || c == '\t' || c == ';' || c == '|' ||
           c == '&' || c == '>' || c == '<';
}

inline auto classify_command(std::string_view command) -> CommandType {
    auto trimmed = trim_left(command);

    // ---- Destructive ----
    static const std::vector<std::string_view> destructive_cmds = {
        "rm", "rmdir", "mkfs", "format", "dd", "shred", "wipefs",
        "fdisk", "parted", "mke2fs", "fsck -y",
        // migrated: git destructive commands
        "git reset --hard", "git clean -fd", "git clean -fxd",
        "git checkout -- .", "git restore .",
        "git branch -D", "git push --force", "git push -f",
        // migrated: docker destructive commands
        "docker system prune", "docker volume rm",
        "docker rmi -f", "docker rm -f",
        // migrated: kubectl destructive commands
        "kubectl delete", "kubectl scale --replicas=0",
        // migrated: package-manager destructive commands (rare but explicit)
        "apt remove", "apt-get remove", "apt purge", "apt-get purge",
        "brew uninstall", "npm uninstall", "pip uninstall", "pip3 uninstall",
        "yarn remove", "pnpm remove", "gem uninstall",
    };
    for (const auto& cmd : destructive_cmds) {
        if (matches_command(trimmed, cmd)) return CommandType::Destructive;
    }

    // ---- Network ----
    static const std::vector<std::string_view> network_cmds = {
        "curl", "wget", "ssh", "scp", "sftp", "rsync",
        "ping", "traceroute", "nslookup", "dig", "host",
        "nc", "netcat", "telnet", "ftp", "socat",
        // migrated: git network subcommands
        "git clone", "git fetch", "git pull", "git push",
        "git remote add", "git remote set-url",
        "git submodule update --remote",
        // migrated: docker network
        "docker pull", "docker push", "docker login", "docker logout",
        // migrated: kubectl network (it always talks to the API server)
        "kubectl", "helm",
        // migrated: package-manager network subcommands (most PM ops are net)
        "npm publish", "npm install", "npm ci", "npm update",
        "yarn install", "yarn add", "yarn publish",
        "pnpm install", "pnpm add", "pnpm publish",
        "pip install", "pip3 install", "pipenv install", "poetry install",
        "cargo install", "cargo add",
        "go get", "go install",
        "brew install", "brew upgrade", "brew update",
        "apt install", "apt-get install", "apt update", "apt-get update",
        "apt upgrade", "apt-get upgrade",
        "dnf install", "yum install", "pacman -S",
        "gem install", "bundle install",
        "composer install", "composer require", "composer update",
        "nuget install", "dotnet add package", "dotnet restore",
        // migrated: misc network
        "gh ", "gh api", "gh pr", "gh issue",
    };
    for (const auto& cmd : network_cmds) {
        if (matches_command(trimmed, cmd)) return CommandType::Network;
    }

    // ---- Write (mutating local state, not otherwise-classified above) ----
    static const std::vector<std::string_view> write_cmds = {
        "cp", "mv", "mkdir", "mkdir -p", "touch", "chmod", "chown", "chgrp",
        "ln", "ln -sf", "tee", "sed -i", "awk -i inplace",
        "rename", "mktemp", "install",
        // migrated: git write subcommands (not destructive, not network)
        "git add", "git commit", "git tag",
        "git checkout -b", "git switch -c",
        "git mv", "git rm",
        "git merge", "git rebase",
        "git stash", "git cherry-pick",
        // migrated: docker write (config/build local state)
        "docker build", "docker tag", "docker save", "docker import",
        "docker commit", "docker cp",
        "docker volume create", "docker network create",
        // migrated: kubectl write (mutating, non-destructive)
        "kubectl apply", "kubectl create", "kubectl edit",
        "kubectl label", "kubectl annotate", "kubectl patch",
        "kubectl rollout", "kubectl scale", "kubectl exec",
        // migrated: FS write helpers
        "useradd", "userdel", "groupadd", "groupdel",
    };
    for (const auto& cmd : write_cmds) {
        if (matches_command(trimmed, cmd)) return CommandType::Write;
    }

    // migrated: shell redirection always makes a command a Write
    if (command.find('>') != std::string_view::npos ||
        command.find(" 2>") != std::string_view::npos ||
        command.find(">>") != std::string_view::npos) {
        return CommandType::Write;
    }

    // ---- Execute ----
    static const std::vector<std::string_view> execute_cmds = {
        "make", "cmake", "ninja",
        "cargo build", "cargo run", "cargo test", "cargo check",
        "go build", "go run", "go test", "go vet",
        "python", "python3", "pythonw", "ruby", "perl", "lua",
        "node", "nodejs", "deno", "bun run", "bun test",
        "npm run", "npm test", "npx", "yarn", "pnpm run",
        "gcc", "g++", "clang", "clang++", "rustc", "javac", "kotlinc", "tsc",
        "swiftc", "dotnet build", "dotnet run", "dotnet test",
        "docker run", "docker compose", "docker-compose",
        "docker exec", "docker attach", "docker start", "docker stop", "docker restart",
        "kubectl run", "kubectl exec", "helm install", "helm upgrade",
        "sh", "bash", "zsh", "ksh", "fish", "dash",
        "test", "[",
        "bazel build", "bazel test", "bazel run",
        "meson",
        // migrated: generic code checkers / linters (side-effect free run is Execute)
        "eslint", "prettier", "tslint", "ruff", "flake8", "pylint", "mypy",
        "clang-tidy", "cppcheck", "shellcheck",
    };
    for (const auto& cmd : execute_cmds) {
        if (matches_command(trimmed, cmd)) return CommandType::Execute;
    }

    // ---- Read ----
    static const std::vector<std::string_view> read_cmds = {
        "ls", "cat", "head", "tail", "less", "more", "bat",
        "find", "locate",
        "grep", "rg", "ag", "fd",
        "sed", "awk",   // non -i form is read-only by default
        "wc", "file", "stat", "du", "df", "free",
        "tree", "which", "where", "type", "command -v",
        "echo", "printf", "date", "whoami", "pwd", "realpath",
        "env", "printenv", "uname", "id", "groups", "hostname",
        "cut", "sort", "uniq", "tr", "paste", "join", "comm",
        "diff", "cmp", "sdiff", "patch --dry-run",
        "od", "hexdump", "xxd", "strings",
        "ps", "top", "htop", "lsof", "netstat", "ss",
        // migrated: git read subcommands
        "git status", "git log", "git diff", "git show",
        "git branch", "git branch --list",
        "git remote -v", "git rev-parse",
        "git blame", "git ls-files", "git tag -l",
        // migrated: docker read
        "docker ps", "docker images", "docker inspect",
        "docker logs", "docker stats", "docker ls",
        // migrated: kubectl read
        "kubectl get", "kubectl describe", "kubectl logs",
        "kubectl top", "kubectl version",
    };
    for (const auto& cmd : read_cmds) {
        if (matches_command(trimmed, cmd)) return CommandType::Read;
    }

    return CommandType::Unknown;
}

// ---------------------------------------------------------------------------
// 2. Exit-code semantic interpretation (migrated from commandSemantics.ts).
//    Many commands use exit codes for signalling, not just success/failure.
// ---------------------------------------------------------------------------

struct InterpretedResult {
    bool is_error;
    std::optional<std::string> message;
};

// Signature: (exitCode, stdout, stderr) -> InterpretedResult
using CommandSemanticFn = auto(*)(int, std::string_view, std::string_view) -> InterpretedResult;

namespace {
    // migrated: grep family — 0 = match, 1 = no match, 2+ = real error
    inline auto grep_semantic(int code, std::string_view, std::string_view) -> InterpretedResult {
        return {
            /*is_error*/ code >= 2,
            code == 1 ? std::optional{"No matches found"} : std::nullopt
        };
    }
    // migrated: find — 0 = ok, 1 = some dirs inaccessible, 2+ = real error
    inline auto find_semantic(int code, std::string_view, std::string_view) -> InterpretedResult {
        return {
            code >= 2,
            code == 1 ? std::optional{"Some directories were inaccessible"} : std::nullopt
        };
    }
    // migrated: diff — 0 = same, 1 = different, 2+ = error
    inline auto diff_semantic(int code, std::string_view, std::string_view) -> InterpretedResult {
        return {
            code >= 2,
            code == 1 ? std::optional{"Files differ"} : std::nullopt
        };
    }
    // migrated: test / [ — 0 = true, 1 = false, 2+ = error
    inline auto test_semantic(int code, std::string_view, std::string_view) -> InterpretedResult {
        return {
            code >= 2,
            code == 1 ? std::optional{"Condition is false"} : std::nullopt
        };
    }
    // Default semantic: anything non-zero is an error
    inline auto default_semantic(int code, std::string_view, std::string_view) -> InterpretedResult {
        return {
            code != 0,
            code != 0 ? std::optional{std::format("Command failed with exit code {}", code)}
                      : std::nullopt
        };
    }
} // namespace

// migrated: extract the base command (first whitespace-delimited word)
inline auto extract_base_command(std::string_view command) -> std::string {
    auto t = trim_left(command);
    size_t end = 0;
    while (end < t.size() && t[end] != ' ' && t[end] != '\t') ++end;
    return std::string(t.substr(0, end));
}

// migrated: heuristically extract the LAST command in a compound/piped line.
// The last command is what determines the shell exit code.
// Splits on `|`, `||`, `&&`, `;` — conservative, deliberately simple.
inline auto heuristically_extract_base_command(std::string_view command) -> std::string {
    if (command.empty()) return {};

    // Scan from the end to find the start of the last segment.
    // We split on the operators: &&, ||, |, ;
    size_t start = 0;
    for (size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (c == ';') {
            start = i + 1;
        } else if (c == '|') {
            if (i + 1 < command.size() && command[i + 1] == '|') {
                start = i + 2;
                ++i; // skip second char
            } else {
                start = i + 1;
            }
        } else if (c == '&' && i + 1 < command.size() && command[i + 1] == '&') {
            start = i + 2;
            ++i;
        }
    }

    auto tail = command.substr(start);
    return extract_base_command(tail);
}

// migrated: look up the semantic handler by base command name
inline auto get_command_semantic(std::string_view base_command) -> CommandSemanticFn {
    static const std::unordered_map<std::string_view, CommandSemanticFn> table = {
        {"grep", &grep_semantic},
        {"egrep", &grep_semantic},
        {"fgrep", &grep_semantic},
        {"rg",   &grep_semantic},
        {"ag",   &grep_semantic},
        {"find", &find_semantic},
        {"fd",   &find_semantic},
        {"diff", &diff_semantic},
        {"cmp",  &diff_semantic},
        {"sdiff",&diff_semantic},
        {"test", &test_semantic},
        {"[",    &test_semantic},
    };
    auto it = table.find(base_command);
    return it != table.end() ? it->second : &default_semantic;
}

// migrated: top-level entry point — interpret a command's result
inline auto interpret_command_result(
    std::string_view command,
    int exit_code,
    std::string_view stdout_sv,
    std::string_view stderr_sv
) -> InterpretedResult {
    const auto base = heuristically_extract_base_command(command);
    const auto fn = get_command_semantic(base);
    return fn(exit_code, stdout_sv, stderr_sv);
}

// ---------------------------------------------------------------------------
// 3. Helpers used by bash_permissions / bash_security (pre-existing,
//    preserved + a few extra classifiers migrated from TS shellRuleMatching).
// ---------------------------------------------------------------------------

inline auto get_affected_paths(std::string_view command) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> paths;
    std::istringstream stream{std::string(command)};
    std::string token;
    bool skip_next = false;

    while (stream >> token) {
        if (skip_next) { skip_next = false; continue; }
        if (token.starts_with("-")) {
            if (token == "-o" || token == "-f" || token == "-d") skip_next = true;
            continue;
        }
        if (token.find('/') != std::string::npos ||
            token.find('.') != std::string::npos) {
            paths.emplace_back(token);
        }
    }
    return paths;
}

inline auto is_git_command(std::string_view command) -> bool {
    auto t = trim_left(command);
    return t.starts_with("git ") || t == "git";
}

// migrated: classify the "operation kind" for a git command (read/write/network/destructive)
// Used by bash_permissions to auto-allow pure reads but prompt for mutating ops.
inline auto classify_git_operation(std::string_view command) -> CommandType {
    auto t = trim_left(command);
    if (!t.starts_with("git") || (t.size() > 3 && t[3] != ' '))
        return CommandType::Unknown;
    // Strip "git "
    if (t.size() > 4) t.remove_prefix(4);
    else return CommandType::Read;

    // Destructive (exact subcommand prefix checks)
    static constexpr std::string_view destructive_sub[] = {
        "reset --hard", "clean -fd", "clean -fxd",
        "checkout -- .", "restore .", "branch -D",
        "push --force", "push -f",
    };
    for (auto p : destructive_sub) if (t.starts_with(p)) return CommandType::Destructive;

    // Network
    static constexpr std::string_view net_sub[] = {
        "clone", "fetch", "pull", "push", "remote", "submodule update --remote",
    };
    for (auto p : net_sub) if (t.starts_with(p)) return CommandType::Network;

    // Write (mutating local state)
    static constexpr std::string_view write_sub[] = {
        "add", "commit", "tag", "checkout -b", "switch -c",
        "mv", "rm", "merge", "rebase", "stash", "cherry-pick",
    };
    for (auto p : write_sub) if (t.starts_with(p)) return CommandType::Write;

    // Everything else defaults to Read (status, log, diff, show, branch, blame, ls-files...)
    return CommandType::Read;
}

inline auto is_package_manager_command(std::string_view command) -> bool {
    auto trimmed = trim_left(command);
    static const std::vector<std::string_view> pm_prefixes = {
        "npm", "yarn", "pnpm", "bun",
        "pip", "pip3", "pipenv", "poetry",
        "cargo",
        "go get", "go install",
        "brew", "apt", "apt-get", "dnf", "yum", "pacman",
        "gem", "bundle",
        "composer",
        "nuget", "dotnet add",
        // migrated: extras
        "port", "zypper",
    };
    return std::any_of(pm_prefixes.begin(), pm_prefixes.end(),
        [&](std::string_view prefix) { return matches_command(trimmed, prefix); });
}

// migrated: coarse classification of package-manager operations
inline auto classify_package_manager_operation(std::string_view command) -> CommandType {
    if (!is_package_manager_command(command)) return CommandType::Unknown;
    auto t = trim_left(command);

    // uninstall/remove/purge -> Destructive
    static constexpr std::string_view destructive_tokens[] = {
        "uninstall", "remove", "purge",
    };
    for (auto tok : destructive_tokens)
        if (t.find(tok) != std::string_view::npos) return CommandType::Destructive;

    // install/add/update/upgrade/publish/push -> mostly Network
    static constexpr std::string_view network_tokens[] = {
        "install", "add", "update", "upgrade", "publish", "fetch",
        "ci", "restore", "create",
    };
    for (auto tok : network_tokens)
        if (t.find(tok) != std::string_view::npos) return CommandType::Network;

    // run/test/build/etc -> Execute, else default to Write
    static constexpr std::string_view exec_tokens[] = {
        "run", "test", "build", "exec", "check", "lint", "audit",
    };
    for (auto tok : exec_tokens)
        if (t.find(tok) != std::string_view::npos) return CommandType::Execute;
    return CommandType::Write;
}

inline auto estimate_duration(std::string_view command) -> std::chrono::seconds {
    auto type = classify_command(command);
    switch (type) {
        case CommandType::Read:         return std::chrono::seconds{1};
        case CommandType::Write:        return std::chrono::seconds{3};
        case CommandType::Execute: {
            if (command.find("build") != std::string_view::npos ||
                command.find("compile") != std::string_view::npos) {
                return std::chrono::seconds{30};
            }
            return std::chrono::seconds{10};
        }
        case CommandType::Network:      return std::chrono::seconds{15};
        case CommandType::Destructive:  return std::chrono::seconds{5};
        case CommandType::Unknown:      return std::chrono::seconds{10};
    }
    return std::chrono::seconds{10};
}

} // namespace cc::tools
