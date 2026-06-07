// Plugin Loader Module
// Consolidates: pluginLoader, loadPluginAgents, loadPluginCommands,
//               loadPluginHooks, loadPluginOutputStyles, walkPluginMarkdown
//
// Responsible for discovering, loading, and validating plugins from various
// sources including marketplaces, git repositories, and local paths.
module;

#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

export module cc.utils.plugin_loader;

import cc.utils.plugin_identifier;
import cc.utils.plugin_versioning;
import cc.utils.json;

export namespace cc::utils::plugin_loader {

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Source Types
// ─────────────────────────────────────────────────────────────────────────────

struct GitHubSource {
    std::string repo;
    std::optional<std::string> ref = std::nullopt;
    std::optional<std::string> sha = std::nullopt;
};

struct GitUrlSource {
    std::string url;
    std::optional<std::string> ref = std::nullopt;
    std::optional<std::string> sha = std::nullopt;
};

struct GitSubdirSource {
    std::string url;
    std::string path;
    std::optional<std::string> ref = std::nullopt;
    std::optional<std::string> sha = std::nullopt;
};

struct NpmSource {
    std::string package_name;
    std::optional<std::string> registry = std::nullopt;
    std::optional<std::string> version = std::nullopt;
};

struct LocalSource {
    std::string path;
};

using PluginSource = std::variant<LocalSource, GitHubSource, GitUrlSource, GitSubdirSource, NpmSource>;

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Manifest & Components
// ─────────────────────────────────────────────────────────────────────────────

struct PluginAuthor {
    std::string name;
    std::optional<std::string> email = std::nullopt;
    std::optional<std::string> url = std::nullopt;
};

struct CommandMetadata {
    std::optional<std::string> source = std::nullopt;
    std::optional<std::string> content = std::nullopt;
    std::optional<std::string> description = std::nullopt;
    bool hidden = false;
};

struct PluginManifest {
    std::string name;
    std::optional<std::string> version = std::nullopt;
    std::optional<std::string> description = std::nullopt;
    std::optional<PluginAuthor> author = std::nullopt;
    std::optional<std::vector<std::string>> keywords = std::nullopt;
    std::optional<std::string> homepage = std::nullopt;
    std::optional<std::vector<std::string>> dependencies = std::nullopt;
    // hooks can be path strings or inline objects
    std::optional<std::vector<std::string>> hooks = std::nullopt;
    // commands can be paths or metadata map
    std::optional<std::vector<std::string>> commands = std::nullopt;
    std::optional<std::vector<std::string>> agents = std::nullopt;
    std::optional<std::vector<std::string>> skills = std::nullopt;
    std::optional<std::vector<std::string>> output_styles = std::nullopt;
};

enum class PluginComponent : unsigned char {
    Commands,
    Agents,
    Skills,
    Hooks,
    OutputStyles,
};

struct PluginError {
    std::string type;  // "path-not-found", "hook-load-failed", "marketplace-blocked-by-policy"
    std::string source;
    std::string plugin;
    std::optional<std::string> path = std::nullopt;
    std::optional<PluginComponent> component = std::nullopt;
    std::optional<std::string> hook_path = std::nullopt;
    std::optional<std::string> reason = std::nullopt;
    std::optional<std::string> marketplace = std::nullopt;
};

// ─────────────────────────────────────────────────────────────────────────────
// Loaded Plugin
// ─────────────────────────────────────────────────────────────────────────────

struct LoadedPlugin {
    std::string name;
    PluginManifest manifest;
    std::filesystem::path path;
    std::string source;
    std::string repository;
    bool enabled = false;

    // Component paths
    std::optional<std::filesystem::path> commands_path = std::nullopt;
    std::optional<std::vector<std::filesystem::path>> commands_paths = std::nullopt;
    std::optional<std::map<std::string, CommandMetadata>> commands_metadata = std::nullopt;
    std::optional<std::filesystem::path> agents_path = std::nullopt;
    std::optional<std::vector<std::filesystem::path>> agents_paths = std::nullopt;
    std::optional<std::filesystem::path> skills_path = std::nullopt;
    std::optional<std::vector<std::filesystem::path>> skills_paths = std::nullopt;
    std::optional<std::filesystem::path> output_styles_path = std::nullopt;
    std::optional<std::vector<std::filesystem::path>> output_styles_paths = std::nullopt;

    // Hooks configuration (simplified — full hooks config is a nested map)
    std::optional<std::map<std::string, std::vector<std::map<std::string, std::string>>>> hooks_config = std::nullopt;

    // Plugin settings (allowlisted keys only)
    std::optional<std::map<std::string, std::string>> settings = std::nullopt;
};

struct PluginLoadResult {
    std::vector<LoadedPlugin> plugins;
    std::vector<PluginError> errors;
};

// ─────────────────────────────────────────────────────────────────────────────
// Cache Path Utilities
// ─────────────────────────────────────────────────────────────────────────────

/// Get the base plugin cache directory path
[[nodiscard]] std::filesystem::path get_plugin_cache_path();

/// Compute versioned cache path: cache/{marketplace}/{plugin}/{version}/
[[nodiscard]] std::filesystem::path get_versioned_cache_path(
    std::string_view plugin_id,
    std::string_view version
);

/// Compute versioned cache path under a specific base directory
[[nodiscard]] std::filesystem::path get_versioned_cache_path_in(
    const std::filesystem::path& base_dir,
    std::string_view plugin_id,
    std::string_view version
);

/// Get versioned ZIP cache path: versioned_path + ".zip"
[[nodiscard]] std::filesystem::path get_versioned_zip_cache_path(
    std::string_view plugin_id,
    std::string_view version
);

/// Get legacy (non-versioned) cache path for backward compatibility
[[nodiscard]] std::filesystem::path get_legacy_cache_path(std::string_view plugin_name);

/// Resolve plugin path with fallback to legacy location
[[nodiscard]] std::expected<std::filesystem::path, std::string> resolve_plugin_path(
    std::string_view plugin_id,
    std::optional<std::string_view> version = std::nullopt
);

/// Probe seed directories for a populated cache at this plugin version
[[nodiscard]] std::optional<std::filesystem::path> probe_seed_cache(
    std::string_view plugin_id,
    std::string_view version
);

/// Probe seed cache for any version (first-boot chicken-and-egg resolution)
[[nodiscard]] std::optional<std::filesystem::path> probe_seed_cache_any_version(
    std::string_view plugin_id
);

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Installation from Source
// ─────────────────────────────────────────────────────────────────────────────

/// Validate a git URL (https, http, file, or git@ssh)
[[nodiscard]] std::expected<std::string, std::string> validate_git_url(std::string_view url);

/// Clone a git repository
[[nodiscard]] std::expected<void, std::string> git_clone(
    std::string_view git_url,
    const std::filesystem::path& target_path,
    std::optional<std::string_view> ref = std::nullopt,
    std::optional<std::string_view> sha = std::nullopt
);

/// Install a plugin from npm
[[nodiscard]] std::expected<void, std::string> install_from_npm(
    std::string_view package_name,
    const std::filesystem::path& target_path,
    std::optional<std::string_view> registry = std::nullopt,
    std::optional<std::string_view> version = std::nullopt
);

/// Install a plugin from a git subdirectory (sparse-checkout)
[[nodiscard]] std::expected<std::optional<std::string>, std::string> install_from_git_subdir(
    std::string_view url,
    const std::filesystem::path& target_path,
    std::string_view subdir_path,
    std::optional<std::string_view> ref = std::nullopt,
    std::optional<std::string_view> sha = std::nullopt
);

/// Generate a temporary cache name for a plugin
[[nodiscard]] std::string generate_temporary_cache_name(const PluginSource& source);

struct CachePluginResult {
    std::filesystem::path path;
    PluginManifest manifest;
    std::optional<std::string> git_commit_sha = std::nullopt;
};

/// Cache a plugin from an external source
[[nodiscard]] std::expected<CachePluginResult, std::string> cache_plugin(
    const PluginSource& source,
    std::optional<PluginManifest> manifest_override = std::nullopt
);

/// Copy plugin files to versioned cache directory
[[nodiscard]] std::expected<std::filesystem::path, std::string> copy_plugin_to_versioned_cache(
    const std::filesystem::path& source_path,
    std::string_view plugin_id,
    std::string_view version
);

// ─────────────────────────────────────────────────────────────────────────────
// Plugin Loading & Discovery
// ─────────────────────────────────────────────────────────────────────────────

/// Load and validate a plugin manifest from a JSON file
[[nodiscard]] std::expected<PluginManifest, std::string> load_plugin_manifest(
    const std::filesystem::path& manifest_path,
    std::string_view plugin_name,
    std::string_view source
);

/// Create a LoadedPlugin from a plugin directory path
[[nodiscard]] std::expected<std::pair<LoadedPlugin, std::vector<PluginError>>, std::string>
create_plugin_from_path(
    const std::filesystem::path& plugin_path,
    std::string_view source,
    bool enabled,
    std::string_view fallback_name,
    bool strict = true
);

/// Load all plugins (marketplace-based discovery with full fetch)
[[nodiscard]] PluginLoadResult load_all_plugins();

/// Load all plugins using cache only (no network fetches)
[[nodiscard]] PluginLoadResult load_all_plugins_cache_only();

/// Clear the memoized plugin load cache
void clear_plugin_cache();

// ─────────────────────────────────────────────────────────────────────────────
// Component Loaders (from loadPluginAgents, loadPluginCommands, etc.)
// ─────────────────────────────────────────────────────────────────────────────

struct PluginCommand {
    std::string name;
    std::string content;
    std::string plugin_name;
    std::string source_path;
    std::optional<CommandMetadata> metadata = std::nullopt;
};

struct PluginAgent {
    std::string name;
    std::string content;
    std::string plugin_name;
    std::string source_path;
};

struct PluginOutputStyle {
    std::string name;
    std::string content;
    std::string plugin_name;
    std::string source_path;
};

/// Load commands from all enabled plugins
[[nodiscard]] std::vector<PluginCommand> load_plugin_commands();

/// Load agents from all enabled plugins
[[nodiscard]] std::vector<PluginAgent> load_plugin_agents();

/// Load output styles from all enabled plugins
[[nodiscard]] std::vector<PluginOutputStyle> load_plugin_output_styles();

/// Clear individual component caches
void clear_plugin_command_cache();
void clear_plugin_agent_cache();
void clear_plugin_output_style_cache();
void clear_plugin_hook_cache();

// ─────────────────────────────────────────────────────────────────────────────
// Markdown Walker (from walkPluginMarkdown)
// ─────────────────────────────────────────────────────────────────────────────

struct MarkdownFile {
    std::string name;        // filename without extension (used as command/agent name)
    std::string content;     // full file content
    std::filesystem::path path;
    std::map<std::string, std::string> frontmatter;
};

/// Walk a directory for markdown files, parsing frontmatter
[[nodiscard]] std::vector<MarkdownFile> walk_plugin_markdown(
    const std::filesystem::path& dir_path
);

/// Walk multiple directories for markdown files
[[nodiscard]] std::vector<MarkdownFile> walk_plugin_markdown_paths(
    const std::vector<std::filesystem::path>& paths
);

namespace detail {

namespace fs = std::filesystem;

struct CommandResult {
    int exit_code = 0;
    std::string output;
};

[[nodiscard]] inline std::string shell_quote(std::string_view value) {
#ifdef _WIN32
    std::string out = "\"";
    for (const char ch : value) {
        if (ch == '"' || ch == '\\') out.push_back('\\');
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
#else
    if (value.empty()) return "''";
    std::string out = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
#endif
}

[[nodiscard]] inline int decode_exit_status(int status) {
#ifdef _WIN32
    return status;
#else
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
#endif
}

[[nodiscard]] inline CommandResult run_command(std::string command, const fs::path& cwd = {}) {
    if (!cwd.empty()) {
        command = "cd " + shell_quote(cwd.string()) + " && " + command;
    }
    command += " 2>&1";

    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return CommandResult{.exit_code = 127, .output = "Failed to spawn command"};
    while (auto bytes = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
        output.append(buffer.data(), bytes);
    }
    const int status = pclose(pipe);
    return CommandResult{.exit_code = decode_exit_status(status), .output = std::move(output)};
}

[[nodiscard]] inline std::string sanitize_cache_component(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('-');
        }
    }
    return out.empty() ? "unknown" : out;
}

[[nodiscard]] inline std::string sanitize_version_component(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('-');
        }
    }
    return out.empty() ? "unknown" : out;
}

[[nodiscard]] inline bool directory_has_entries(const fs::path& path) {
    if (!fs::is_directory(path)) return false;
    std::error_code ec;
    auto it = fs::directory_iterator(path, ec);
    return !ec && it != fs::directory_iterator{};
}

[[nodiscard]] inline std::vector<fs::path> plugin_seed_dirs() {
    std::vector<fs::path> dirs;
    const char* raw = std::getenv("CLAUDE_CODE_PLUGIN_SEED_DIR");
    if (!raw || !raw[0]) return dirs;
#ifdef _WIN32
    constexpr char delimiter = ';';
#else
    constexpr char delimiter = ':';
#endif
    std::stringstream parts(raw);
    std::string part;
    while (std::getline(parts, part, delimiter)) {
        if (!part.empty()) dirs.emplace_back(part);
    }
    return dirs;
}

[[nodiscard]] inline std::expected<fs::path, std::string> validate_path_within_base(
    const fs::path& base,
    std::string_view relative
) {
    auto base_abs = fs::absolute(base).lexically_normal();
    auto resolved = fs::absolute(base / std::string(relative)).lexically_normal();
    const auto base_text = base_abs.string();
    const auto resolved_text = resolved.string();
    const auto prefix = base_text.ends_with(fs::path::preferred_separator)
        ? base_text
        : base_text + fs::path::preferred_separator;
    if (resolved_text != base_text && !resolved_text.starts_with(prefix)) {
        return std::unexpected("Path traversal detected: " + std::string(relative));
    }
    return resolved;
}

[[nodiscard]] inline std::expected<void, std::string> copy_dir(const fs::path& source, const fs::path& target) {
    if (!fs::exists(source)) return std::unexpected("Source path does not exist: " + source.string());
    if (!fs::is_directory(source)) return std::unexpected("Source path is not a directory: " + source.string());
    auto copy_source = source;
    std::error_code ec;
    if (fs::is_symlink(source)) {
        copy_source = fs::canonical(source, ec);
        if (ec) return std::unexpected("Failed to resolve source symlink: " + ec.message());
    }
    fs::remove_all(target, ec);
    ec.clear();
    fs::create_directories(target.parent_path(), ec);
    if (ec) return std::unexpected("Failed to create target parent directory: " + ec.message());
    fs::copy(copy_source, target,
        fs::copy_options::recursive | fs::copy_options::overwrite_existing,
        ec);
    if (ec) return std::unexpected("Failed to copy plugin directory: " + ec.message());
    return {};
}

inline void remove_git_metadata(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path / ".git", ec);
}

[[nodiscard]] inline std::optional<std::string> rev_parse_head(const fs::path& repo) {
    auto result = run_command("git rev-parse HEAD", repo);
    if (result.exit_code != 0) return std::nullopt;
    while (!result.output.empty() && (result.output.back() == '\n' || result.output.back() == '\r')) {
        result.output.pop_back();
    }
    return result.output.empty() ? std::nullopt : std::optional<std::string>{result.output};
}

[[nodiscard]] inline bool is_env_truthy(const char* name) {
    const char* raw = std::getenv(name);
    if (!raw) return false;
    std::string value(raw);
    std::ranges::transform(value, value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

[[nodiscard]] inline bool is_github_repo_shorthand(std::string_view value) {
    const auto slash = value.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= value.size()) return false;
    if (value.find('/', slash + 1) != std::string_view::npos) return false;
    const auto valid = [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.';
    };
    return std::ranges::all_of(value, valid);
}

[[nodiscard]] inline std::string github_clone_url(std::string_view repo) {
    if (is_env_truthy("CLAUDE_CODE_REMOTE")) {
        return "https://github.com/" + std::string(repo) + ".git";
    }
    return "git@github.com:" + std::string(repo) + ".git";
}

[[nodiscard]] inline std::string read_text_file(const fs::path& path);

[[nodiscard]] inline std::expected<std::string, std::string> git_subdir_url(std::string_view url) {
    if (is_github_repo_shorthand(url)) return github_clone_url(url);
    return validate_git_url(url);
}

[[nodiscard]] inline std::optional<std::string> package_json_name(const fs::path& package_root) {
    const auto package_json = package_root / "package.json";
    if (!fs::exists(package_json)) return std::nullopt;
    auto parsed = cc::utils::json::parse(read_text_file(package_json));
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto name = parsed->root().get("name");
    if (!name.is_str()) return std::nullopt;
    return std::string(name.as_str());
}

[[nodiscard]] inline fs::path npm_spec_local_path(std::string_view spec) {
    if (spec.starts_with("file:")) return fs::path(std::string(spec.substr(5)));
    fs::path path{std::string(spec)};
    if (fs::exists(path)) return path;
    return {};
}

[[nodiscard]] inline fs::path npm_package_path(const fs::path& npm_cache_path, std::string_view package_spec) {
    auto local_path = npm_spec_local_path(package_spec);
    if (!local_path.empty()) {
        if (auto name = package_json_name(local_path)) {
            return npm_cache_path / "node_modules" / *name;
        }
    }
    return npm_cache_path / "node_modules" / std::string(package_spec);
}

[[nodiscard]] inline std::string read_text_file(const fs::path& path) {
    std::ifstream input(path);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] inline std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return std::string(value.substr(begin, end - begin));
}

[[nodiscard]] inline std::string strip_quotes(std::string value) {
    value = trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

[[nodiscard]] inline std::optional<std::string> json_string(cc::utils::json::JsonVal obj, std::string_view key) {
    auto value = obj.get(key);
    if (!value.is_str()) return std::nullopt;
    return std::string(value.as_str());
}

[[nodiscard]] inline std::optional<std::vector<std::string>> json_string_array(
    cc::utils::json::JsonVal obj,
    std::string_view key
) {
    auto value = obj.get(key);
    if (!value.is_arr()) {
        if (value.is_str()) return std::vector<std::string>{std::string(value.as_str())};
        return std::nullopt;
    }
    std::vector<std::string> out;
    value.iter([&](cc::utils::json::JsonVal item) {
        if (item.is_str()) out.emplace_back(item.as_str());
    });
    return out;
}

[[nodiscard]] inline std::optional<PluginAuthor> json_author(cc::utils::json::JsonVal obj) {
    auto value = obj.get("author");
    if (value.is_str()) return PluginAuthor{.name = std::string(value.as_str())};
    if (!value.is_obj()) return std::nullopt;
    auto name = json_string(value, "name");
    if (!name || name->empty()) return std::nullopt;
    return PluginAuthor{
        .name = *name,
        .email = json_string(value, "email"),
        .url = json_string(value, "url"),
    };
}

[[nodiscard]] inline fs::path resolve_plugin_component_path(const fs::path& root, std::string_view raw) {
    fs::path path(raw);
    if (path.is_relative()) path = root / path;
    return path;
}

inline void add_existing_component_paths(
    const fs::path& plugin_root,
    const std::optional<std::vector<std::string>>& manifest_paths,
    std::optional<std::vector<fs::path>>& target,
    std::vector<PluginError>& errors,
    std::string_view source,
    std::string_view plugin,
    PluginComponent component
) {
    if (!manifest_paths) return;
    std::vector<fs::path> paths;
    for (const auto& raw : *manifest_paths) {
        auto path = resolve_plugin_component_path(plugin_root, raw);
        if (fs::exists(path)) {
            paths.push_back(path);
        } else {
            errors.push_back(PluginError{
                .type = "path-not-found",
                .source = std::string(source),
                .plugin = std::string(plugin),
                .path = path.string(),
                .component = component,
            });
        }
    }
    if (!paths.empty()) target = std::move(paths);
}

[[nodiscard]] inline bool is_plugin_root(const fs::path& path) {
    return fs::exists(path / ".claude-plugin" / "plugin.json") ||
        fs::exists(path / "plugin.json") ||
        fs::is_directory(path / "commands") ||
        fs::is_directory(path / "agents") ||
        fs::is_directory(path / "skills") ||
        fs::is_directory(path / "output-styles");
}

inline void discover_plugin_roots(const fs::path& root, std::vector<fs::path>& out) {
    if (!fs::is_directory(root)) return;
    if (is_plugin_root(root)) {
        out.push_back(root);
        return;
    }
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec || !entry.is_directory()) continue;
        const auto name = entry.path().filename().string();
        if (name == "npm-cache" || name.starts_with("temp-")) continue;
        discover_plugin_roots(entry.path(), out);
    }
}

[[nodiscard]] inline std::string markdown_name(const fs::path& file) {
    auto name = file.stem().string();
    return name.empty() ? file.filename().string() : name;
}

[[nodiscard]] inline std::pair<std::map<std::string, std::string>, std::string> parse_markdown_frontmatter(
    std::string content
) {
    std::map<std::string, std::string> frontmatter;
    if (!content.starts_with("---\n") && !content.starts_with("---\r\n")) {
        return {frontmatter, std::move(content)};
    }
    const auto begin_len = content.starts_with("---\r\n") ? 5u : 4u;
    auto end = content.find("\n---", begin_len);
    if (end == std::string::npos) return {frontmatter, std::move(content)};
    std::stringstream lines(content.substr(begin_len, end - begin_len));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        frontmatter[trim(line.substr(0, colon))] = strip_quotes(line.substr(colon + 1));
    }
    auto body_start = content.find('\n', end + 1);
    if (body_start == std::string::npos) return {frontmatter, std::string{}};
    return {frontmatter, content.substr(body_start + 1)};
}

[[nodiscard]] inline std::vector<fs::path> plugin_component_paths(
    const LoadedPlugin& plugin,
    const std::optional<fs::path>& single_path,
    const std::optional<std::vector<fs::path>>& multiple_paths
) {
    std::vector<fs::path> paths;
    if (single_path) paths.push_back(*single_path);
    if (multiple_paths) {
        for (const auto& path : *multiple_paths) paths.push_back(path);
    }
    for (auto& path : paths) {
        if (path.is_relative()) path = plugin.path / path;
    }
    return paths;
}

} // namespace detail

[[nodiscard]] std::filesystem::path get_plugin_cache_path() {
    if (auto* value = std::getenv("CLAUDE_CODE_PLUGIN_CACHE_DIR"); value && value[0]) return value;
    if (auto* home = std::getenv("HOME"); home && home[0]) return std::filesystem::path(home) / ".claude" / "plugins";
    return std::filesystem::current_path() / ".claude" / "plugins";
}

[[nodiscard]] std::filesystem::path get_versioned_cache_path(
    std::string_view plugin_id,
    std::string_view version
) {
    return get_versioned_cache_path_in(get_plugin_cache_path(), plugin_id, version);
}

[[nodiscard]] std::filesystem::path get_versioned_cache_path_in(
    const std::filesystem::path& base_dir,
    std::string_view plugin_id,
    std::string_view version
) {
    auto parsed = cc::utils::plugin_identifier::parse_plugin_identifier(plugin_id);
    const auto marketplace = parsed.marketplace.value_or("unknown");
    const auto version_text = version.empty() ? std::string("unknown") : std::string(version);
    return base_dir / "cache" /
        detail::sanitize_cache_component(marketplace) /
        detail::sanitize_cache_component(parsed.name.empty() ? std::string(plugin_id) : parsed.name) /
        detail::sanitize_version_component(version_text);
}

[[nodiscard]] std::filesystem::path get_versioned_zip_cache_path(
    std::string_view plugin_id,
    std::string_view version
) {
    auto path = get_versioned_cache_path(plugin_id, version);
    return std::filesystem::path(path.string() + ".zip");
}

[[nodiscard]] std::filesystem::path get_legacy_cache_path(std::string_view plugin_name) {
    return get_plugin_cache_path() / detail::sanitize_cache_component(plugin_name);
}

[[nodiscard]] std::expected<std::filesystem::path, std::string> resolve_plugin_path(
    std::string_view plugin_id,
    std::optional<std::string_view> version
) {
    if (version) {
        auto versioned = get_versioned_cache_path(plugin_id, *version);
        if (std::filesystem::exists(versioned)) return versioned;
    }
    auto parsed = cc::utils::plugin_identifier::parse_plugin_identifier(plugin_id);
    auto legacy = get_legacy_cache_path(parsed.name.empty() ? std::string(plugin_id) : parsed.name);
    if (std::filesystem::exists(legacy)) return legacy;
    return std::unexpected("Plugin cache path not found: " + std::string(plugin_id));
}

[[nodiscard]] std::optional<std::filesystem::path> probe_seed_cache(
    std::string_view plugin_id,
    std::string_view version
) {
    for (const auto& seed_dir : detail::plugin_seed_dirs()) {
        auto seed_path = get_versioned_cache_path_in(seed_dir, plugin_id, version);
        if (detail::directory_has_entries(seed_path)) return seed_path;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> probe_seed_cache_any_version(std::string_view plugin_id) {
    for (const auto& seed_dir : detail::plugin_seed_dirs()) {
        auto parent = get_versioned_cache_path_in(seed_dir, plugin_id, "_").parent_path();
        if (!std::filesystem::is_directory(parent)) continue;
        std::vector<std::filesystem::path> versions;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
            if (ec) break;
            if (entry.is_directory() && detail::directory_has_entries(entry.path())) {
                versions.push_back(entry.path());
            }
        }
        if (versions.size() == 1) return versions.front();
    }
    return std::nullopt;
}

[[nodiscard]] std::expected<std::string, std::string> validate_git_url(std::string_view url) {
    if (url.starts_with("https://") || url.starts_with("http://") ||
        url.starts_with("file://") || url.starts_with("git@")) {
        return std::string(url);
    }
    return std::unexpected("Unsupported git URL");
}

[[nodiscard]] std::expected<void, std::string> git_clone(
    std::string_view git_url,
    const std::filesystem::path& target_path,
    std::optional<std::string_view> ref,
    std::optional<std::string_view> sha
) {
    auto valid_url = validate_git_url(git_url);
    if (!valid_url) return std::unexpected(valid_url.error());

    std::error_code ec;
    std::filesystem::remove_all(target_path, ec);
    ec.clear();
    std::filesystem::create_directories(target_path.parent_path(), ec);
    if (ec) return std::unexpected("Failed to create clone parent directory: " + ec.message());

    std::string command = "git clone --depth 1 --recurse-submodules --shallow-submodules";
    if (ref && !ref->empty()) {
        command += " --branch " + detail::shell_quote(*ref);
    }
    if (sha && !sha->empty()) {
        command += " --no-checkout";
    }
    command += " " + detail::shell_quote(*valid_url) + " " + detail::shell_quote(target_path.string());

    auto clone = detail::run_command(command);
    if (clone.exit_code != 0) {
        return std::unexpected("Failed to clone repository: " + clone.output);
    }

    if (sha && !sha->empty()) {
        auto fetch = detail::run_command("git fetch --depth 1 origin " + detail::shell_quote(*sha), target_path);
        if (fetch.exit_code != 0) {
            auto unshallow = detail::run_command("git fetch --unshallow", target_path);
            if (unshallow.exit_code != 0) {
                return std::unexpected("Failed to fetch commit " + std::string(*sha) + ": " + unshallow.output);
            }
        }
        auto checkout = detail::run_command("git checkout " + detail::shell_quote(*sha), target_path);
        if (checkout.exit_code != 0) {
            return std::unexpected("Failed to checkout commit " + std::string(*sha) + ": " + checkout.output);
        }
    }

    return {};
}

[[nodiscard]] std::expected<void, std::string> install_from_npm(
    std::string_view package_name,
    const std::filesystem::path& target_path,
    std::optional<std::string_view> registry,
    std::optional<std::string_view> version
) {
    const auto npm_cache_path = get_plugin_cache_path() / "npm-cache";
    std::error_code ec;
    std::filesystem::create_directories(npm_cache_path, ec);
    if (ec) return std::unexpected("Failed to create npm cache directory: " + ec.message());

    std::string package_spec(package_name);
    if (version && !version->empty() && detail::npm_spec_local_path(package_name).empty()) {
        package_spec += "@" + std::string(*version);
    }

    auto package_path = detail::npm_package_path(npm_cache_path, package_name);
    if (!std::filesystem::exists(package_path)) {
        std::string command = "npm install " + detail::shell_quote(package_spec) +
            " --prefix " + detail::shell_quote(npm_cache_path.string());
        if (registry && !registry->empty()) {
            command += " --registry " + detail::shell_quote(*registry);
        }
        auto installed = detail::run_command(command);
        if (installed.exit_code != 0) {
            return std::unexpected("Failed to install npm package: " + installed.output);
        }
    }

    if (!std::filesystem::exists(package_path)) {
        return std::unexpected("NPM package installed but was not found at: " + package_path.string());
    }
    auto copied = detail::copy_dir(package_path, target_path);
    if (!copied) return copied;
    detail::remove_git_metadata(target_path);
    return {};
}

[[nodiscard]] std::expected<std::optional<std::string>, std::string> install_from_git_subdir(
    std::string_view url,
    const std::filesystem::path& target_path,
    std::string_view subdir_path,
    std::optional<std::string_view> ref,
    std::optional<std::string_view> sha
) {
    auto git_url = detail::git_subdir_url(url);
    if (!git_url) return std::unexpected(git_url.error());

    const auto clone_dir = std::filesystem::path(target_path.string() + ".clone");
    std::error_code ec;
    std::filesystem::remove_all(target_path, ec);
    std::filesystem::remove_all(clone_dir, ec);
    ec.clear();
    std::filesystem::create_directories(target_path.parent_path(), ec);
    if (ec) return std::unexpected("Failed to create git-subdir parent directory: " + ec.message());

    std::string clone_command = "git clone --depth 1 --filter=tree:0 --no-checkout";
    if (ref && !ref->empty()) clone_command += " --branch " + detail::shell_quote(*ref);
    clone_command += " " + detail::shell_quote(*git_url) + " " + detail::shell_quote(clone_dir.string());

    auto clone = detail::run_command(clone_command);
    if (clone.exit_code != 0) {
        return std::unexpected("Failed to clone repository for git-subdir source: " + clone.output);
    }

    auto cleanup = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1),
        [clone_dir](void*) {
            std::error_code cleanup_ec;
            std::filesystem::remove_all(clone_dir, cleanup_ec);
        }
    );

    auto sparse = detail::run_command(
        "git sparse-checkout set --cone -- " + detail::shell_quote(subdir_path),
        clone_dir
    );
    if (sparse.exit_code != 0) {
        return std::unexpected("git sparse-checkout set failed: " + sparse.output);
    }

    std::optional<std::string> resolved_sha;
    if (sha && !sha->empty()) {
        auto fetch = detail::run_command("git fetch --depth 1 origin " + detail::shell_quote(*sha), clone_dir);
        if (fetch.exit_code != 0) {
            auto unshallow = detail::run_command("git fetch --unshallow", clone_dir);
            if (unshallow.exit_code != 0) {
                return std::unexpected("Failed to fetch commit " + std::string(*sha) + ": " + unshallow.output);
            }
        }
        auto checkout = detail::run_command("git checkout " + detail::shell_quote(*sha), clone_dir);
        if (checkout.exit_code != 0) {
            return std::unexpected("Failed to checkout commit " + std::string(*sha) + ": " + checkout.output);
        }
        resolved_sha = std::string(*sha);
    } else {
        auto checkout = detail::run_command("git checkout HEAD", clone_dir);
        if (checkout.exit_code != 0) {
            return std::unexpected("git checkout after sparse-checkout failed: " + checkout.output);
        }
        resolved_sha = detail::rev_parse_head(clone_dir);
    }

    auto resolved_subdir = detail::validate_path_within_base(clone_dir, subdir_path);
    if (!resolved_subdir) return std::unexpected(resolved_subdir.error());
    if (!std::filesystem::exists(*resolved_subdir)) {
        return std::unexpected("Subdirectory '" + std::string(subdir_path) + "' not found in repository");
    }
    std::filesystem::rename(*resolved_subdir, target_path, ec);
    if (ec) {
        auto copied = detail::copy_dir(*resolved_subdir, target_path);
        if (!copied) return std::unexpected(copied.error());
    }
    detail::remove_git_metadata(target_path);
    return resolved_sha;
}

[[nodiscard]] std::string generate_temporary_cache_name(const PluginSource& source) {
    static std::atomic<unsigned long long> counter{0};
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    const auto suffix = "-" + std::to_string(now) + "-" + std::to_string(counter.fetch_add(1));
    auto prefix = std::visit([](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, LocalSource>) return "local-" + std::filesystem::path(value.path).filename().string();
        if constexpr (std::is_same_v<T, GitHubSource>) return "github-" + value.repo;
        if constexpr (std::is_same_v<T, GitUrlSource>) return "git-" + value.url;
        if constexpr (std::is_same_v<T, GitSubdirSource>) return "git-subdir-" + value.path;
        if constexpr (std::is_same_v<T, NpmSource>) return "npm-" + value.package_name;
        return std::string("plugin");
    }, source);
    return "temp-" + detail::sanitize_version_component(prefix) + suffix;
}

[[nodiscard]] std::expected<CachePluginResult, std::string> cache_plugin(
    const PluginSource& source,
    std::optional<PluginManifest> manifest_override
) {
    const auto cache_root = get_plugin_cache_path();
    std::error_code ec;
    std::filesystem::create_directories(cache_root, ec);
    if (ec) return std::unexpected("Failed to create plugin cache directory: " + ec.message());

    const auto temp_path = cache_root / generate_temporary_cache_name(source);
    std::optional<std::string> git_commit_sha;

    auto cleanup_temp = [&]() {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(temp_path, cleanup_ec);
    };

    auto install_result = std::visit([&](const auto& value) -> std::expected<void, std::string> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, LocalSource>) {
            auto copied = detail::copy_dir(value.path, temp_path);
            if (!copied) return copied;
            detail::remove_git_metadata(temp_path);
            return {};
        } else if constexpr (std::is_same_v<T, GitHubSource>) {
            auto cloned = git_clone(detail::github_clone_url(value.repo), temp_path, value.ref, value.sha);
            if (!cloned) return cloned;
            git_commit_sha = detail::rev_parse_head(temp_path);
            return {};
        } else if constexpr (std::is_same_v<T, GitUrlSource>) {
            auto cloned = git_clone(value.url, temp_path, value.ref, value.sha);
            if (!cloned) return cloned;
            git_commit_sha = detail::rev_parse_head(temp_path);
            return {};
        } else if constexpr (std::is_same_v<T, GitSubdirSource>) {
            auto installed = install_from_git_subdir(value.url, temp_path, value.path, value.ref, value.sha);
            if (!installed) return std::unexpected(installed.error());
            git_commit_sha = *installed;
            return {};
        } else if constexpr (std::is_same_v<T, NpmSource>) {
            return install_from_npm(value.package_name, temp_path, value.registry, value.version);
        } else {
            return std::unexpected("Unsupported plugin source type");
        }
    }, source);

    if (!install_result) {
        cleanup_temp();
        return std::unexpected(install_result.error());
    }

    const auto manifest_path = temp_path / ".claude-plugin" / "plugin.json";
    const auto legacy_manifest_path = temp_path / "plugin.json";
    std::expected<PluginManifest, std::string> manifest = std::unexpected("missing");
    if (std::filesystem::exists(manifest_path)) {
        manifest = load_plugin_manifest(manifest_path, temp_path.filename().string(), temp_path.string());
    } else if (std::filesystem::exists(legacy_manifest_path)) {
        manifest = load_plugin_manifest(legacy_manifest_path, temp_path.filename().string(), temp_path.string());
    } else if (manifest_override) {
        manifest = *manifest_override;
    } else {
        manifest = PluginManifest{
            .name = temp_path.filename().string(),
            .description = "Plugin cached from source",
        };
    }

    if (!manifest) {
        cleanup_temp();
        return std::unexpected(manifest.error());
    }

    const auto final_name = detail::sanitize_cache_component(manifest->name);
    const auto final_path = cache_root / final_name;
    if (final_path != temp_path) {
        std::filesystem::remove_all(final_path, ec);
        ec.clear();
        std::filesystem::rename(temp_path, final_path, ec);
        if (ec) {
            auto copied = detail::copy_dir(temp_path, final_path);
            cleanup_temp();
            if (!copied) return std::unexpected(copied.error());
        }
    }

    return CachePluginResult{.path = final_path, .manifest = *manifest, .git_commit_sha = git_commit_sha};
}

[[nodiscard]] std::expected<std::filesystem::path, std::string> copy_plugin_to_versioned_cache(
    const std::filesystem::path& source_path,
    std::string_view plugin_id,
    std::string_view version
) {
    if (!std::filesystem::exists(source_path)) return std::unexpected("Plugin source path does not exist");
    auto target = get_versioned_cache_path(plugin_id, version);
    if (auto seed = probe_seed_cache(plugin_id, version)) return *seed;
    std::error_code ec;
    std::filesystem::remove_all(target, ec);
    ec.clear();
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) return std::unexpected("Failed to create plugin cache directory");
    std::filesystem::copy(source_path, target,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::copy_symlinks |
            std::filesystem::copy_options::overwrite_existing,
        ec);
    if (ec) return std::unexpected("Failed to copy plugin to cache");
    detail::remove_git_metadata(target);
    if (!detail::directory_has_entries(target)) {
        return std::unexpected("Failed to copy plugin to cache: destination is empty");
    }
    return target;
}

[[nodiscard]] std::expected<PluginManifest, std::string> load_plugin_manifest(
    const std::filesystem::path& manifest_path,
    std::string_view plugin_name,
    std::string_view source
) {
    if (!std::filesystem::exists(manifest_path)) {
        return PluginManifest{
            .name = std::string(plugin_name),
            .description = "Plugin from " + std::string(source),
        };
    }

    auto parsed = cc::utils::json::parse(detail::read_text_file(manifest_path));
    if (!parsed || !parsed->root().is_obj()) {
        return std::unexpected("Plugin manifest is invalid JSON: " + manifest_path.string());
    }
    auto root = parsed->root();
    auto name = detail::json_string(root, "name");
    if (!name || name->empty()) {
        return std::unexpected("Plugin manifest is missing name: " + manifest_path.string());
    }
    PluginManifest manifest;
    manifest.name = *name;
    manifest.version = detail::json_string(root, "version");
    manifest.description = detail::json_string(root, "description");
    manifest.author = detail::json_author(root);
    manifest.keywords = detail::json_string_array(root, "keywords");
    manifest.homepage = detail::json_string(root, "homepage");
    manifest.dependencies = detail::json_string_array(root, "dependencies");
    manifest.hooks = detail::json_string_array(root, "hooks");
    manifest.commands = detail::json_string_array(root, "commands");
    manifest.agents = detail::json_string_array(root, "agents");
    manifest.skills = detail::json_string_array(root, "skills");
    manifest.output_styles = detail::json_string_array(root, "outputStyles")
        .or_else([&] { return detail::json_string_array(root, "output-styles"); });
    return manifest;
}

[[nodiscard]] std::expected<std::pair<LoadedPlugin, std::vector<PluginError>>, std::string>
create_plugin_from_path(
    const std::filesystem::path& plugin_path,
    std::string_view source,
    bool enabled,
    std::string_view fallback_name,
    bool
) {
    auto manifest_path = plugin_path / ".claude-plugin" / "plugin.json";
    if (!std::filesystem::exists(manifest_path) && std::filesystem::exists(plugin_path / "plugin.json")) {
        manifest_path = plugin_path / "plugin.json";
    }
    auto manifest = load_plugin_manifest(manifest_path, fallback_name, source);
    if (!manifest) return std::unexpected(manifest.error());
    std::vector<PluginError> errors;
    LoadedPlugin plugin{
        .name = manifest->name,
        .manifest = *manifest,
        .path = plugin_path,
        .source = std::string(source),
        .repository = std::string(source),
        .enabled = enabled,
    };

    if (!plugin.manifest.commands && std::filesystem::is_directory(plugin_path / "commands")) {
        plugin.commands_path = plugin_path / "commands";
    }
    detail::add_existing_component_paths(plugin_path, plugin.manifest.commands, plugin.commands_paths,
        errors, source, plugin.name, PluginComponent::Commands);

    if (!plugin.manifest.agents && std::filesystem::is_directory(plugin_path / "agents")) {
        plugin.agents_path = plugin_path / "agents";
    }
    detail::add_existing_component_paths(plugin_path, plugin.manifest.agents, plugin.agents_paths,
        errors, source, plugin.name, PluginComponent::Agents);

    if (!plugin.manifest.skills && std::filesystem::is_directory(plugin_path / "skills")) {
        plugin.skills_path = plugin_path / "skills";
    }
    detail::add_existing_component_paths(plugin_path, plugin.manifest.skills, plugin.skills_paths,
        errors, source, plugin.name, PluginComponent::Skills);

    if (!plugin.manifest.output_styles && std::filesystem::is_directory(plugin_path / "output-styles")) {
        plugin.output_styles_path = plugin_path / "output-styles";
    }
    detail::add_existing_component_paths(plugin_path, plugin.manifest.output_styles, plugin.output_styles_paths,
        errors, source, plugin.name, PluginComponent::OutputStyles);

    return std::pair<LoadedPlugin, std::vector<PluginError>>{std::move(plugin), std::move(errors)};
}

[[nodiscard]] PluginLoadResult load_all_plugins_cache_only() {
    PluginLoadResult result;
    std::vector<std::filesystem::path> roots;
    detail::discover_plugin_roots(get_plugin_cache_path(), roots);
    for (const auto& root : roots) {
        const auto fallback = root.filename().string();
        auto loaded = create_plugin_from_path(root, fallback + "@cache", true, fallback);
        if (!loaded) {
            result.errors.push_back(PluginError{
                .type = "load-failed",
                .source = root.string(),
                .plugin = fallback,
                .reason = loaded.error(),
            });
            continue;
        }
        result.plugins.push_back(std::move(loaded->first));
        for (auto& error : loaded->second) result.errors.push_back(std::move(error));
    }
    return result;
}

[[nodiscard]] PluginLoadResult load_all_plugins() {
    return load_all_plugins_cache_only();
}

void clear_plugin_cache() {}
void clear_plugin_command_cache() {}
void clear_plugin_agent_cache() {}
void clear_plugin_output_style_cache() {}
void clear_plugin_hook_cache() {}

[[nodiscard]] std::vector<MarkdownFile> walk_plugin_markdown(
    const std::filesystem::path& dir_path
) {
    std::vector<MarkdownFile> files;
    if (!std::filesystem::is_directory(dir_path)) return files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        auto path = entry.path();
        auto ext = path.extension().string();
        std::ranges::transform(ext, ext.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (ext != ".md") continue;
        auto parsed = detail::parse_markdown_frontmatter(detail::read_text_file(path));
        files.push_back(MarkdownFile{
            .name = detail::markdown_name(path),
            .content = std::move(parsed.second),
            .path = path,
            .frontmatter = std::move(parsed.first),
        });
    }
    std::ranges::sort(files, {}, &MarkdownFile::path);
    return files;
}

[[nodiscard]] std::vector<MarkdownFile> walk_plugin_markdown_paths(
    const std::vector<std::filesystem::path>& paths
) {
    std::vector<MarkdownFile> files;
    for (const auto& path : paths) {
        if (std::filesystem::is_directory(path)) {
            auto nested = walk_plugin_markdown(path);
            files.insert(files.end(), std::make_move_iterator(nested.begin()), std::make_move_iterator(nested.end()));
        } else if (std::filesystem::is_regular_file(path) && path.extension() == ".md") {
            auto parsed = detail::parse_markdown_frontmatter(detail::read_text_file(path));
            files.push_back(MarkdownFile{
                .name = detail::markdown_name(path),
                .content = std::move(parsed.second),
                .path = path,
                .frontmatter = std::move(parsed.first),
            });
        }
    }
    std::ranges::sort(files, {}, &MarkdownFile::path);
    return files;
}

[[nodiscard]] std::vector<PluginCommand> load_plugin_commands() {
    std::vector<PluginCommand> commands;
    auto loaded = load_all_plugins_cache_only();
    for (const auto& plugin : loaded.plugins) {
        if (!plugin.enabled) continue;
        auto paths = detail::plugin_component_paths(plugin, plugin.commands_path, plugin.commands_paths);
        auto files = walk_plugin_markdown_paths(paths);
        for (const auto& file : files) {
            commands.push_back(PluginCommand{
                .name = plugin.name + ":" + file.name,
                .content = file.content,
                .plugin_name = plugin.name,
                .source_path = file.path.string(),
            });
        }
        if (plugin.commands_metadata) {
            for (const auto& [name, metadata] : *plugin.commands_metadata) {
                if (!metadata.content) continue;
                commands.push_back(PluginCommand{
                    .name = plugin.name + ":" + name,
                    .content = *metadata.content,
                    .plugin_name = plugin.name,
                    .source_path = {},
                    .metadata = metadata,
                });
            }
        }
    }
    return commands;
}

[[nodiscard]] std::vector<PluginAgent> load_plugin_agents() {
    std::vector<PluginAgent> agents;
    auto loaded = load_all_plugins_cache_only();
    for (const auto& plugin : loaded.plugins) {
        if (!plugin.enabled) continue;
        auto paths = detail::plugin_component_paths(plugin, plugin.agents_path, plugin.agents_paths);
        auto files = walk_plugin_markdown_paths(paths);
        for (const auto& file : files) {
            agents.push_back(PluginAgent{
                .name = plugin.name + ":" + file.name,
                .content = file.content,
                .plugin_name = plugin.name,
                .source_path = file.path.string(),
            });
        }
    }
    return agents;
}

[[nodiscard]] std::vector<PluginOutputStyle> load_plugin_output_styles() {
    std::vector<PluginOutputStyle> styles;
    auto loaded = load_all_plugins_cache_only();
    for (const auto& plugin : loaded.plugins) {
        if (!plugin.enabled) continue;
        auto paths = detail::plugin_component_paths(plugin, plugin.output_styles_path, plugin.output_styles_paths);
        auto files = walk_plugin_markdown_paths(paths);
        for (const auto& file : files) {
            styles.push_back(PluginOutputStyle{
                .name = plugin.name + ":" + file.name,
                .content = file.content,
                .plugin_name = plugin.name,
                .source_path = file.path.string(),
            });
        }
    }
    return styles;
}

} // namespace cc::utils::plugin_loader
