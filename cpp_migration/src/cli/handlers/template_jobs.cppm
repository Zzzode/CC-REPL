module;
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <cstdlib>
#include <algorithm>
#include <regex>

export module cc.cli.handlers.template_jobs;

export namespace cc::cli::handlers {

// A template job configuration
struct TemplateJob {
    std::string name;
    std::string template_id;
    std::map<std::string, std::string> variables;
    std::optional<std::string> output_path;
};

// Template metadata for listing
struct TemplateInfo {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> required_vars;
    std::vector<std::string> optional_vars;
    std::string source; // "builtin", "global", "project"
};

// Template content loaded from file
struct TemplateContent {
    std::string id;
    std::string body;
    std::vector<std::string> steps;
    std::map<std::string, std::string> defaults;
};

namespace detail {

inline std::filesystem::path get_global_templates_dir() {
    const char* home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".config" / "claude-code" / "templates";
    }
    return std::filesystem::temp_directory_path() / "claude-code-templates";
}

inline std::filesystem::path get_project_templates_dir() {
    auto cwd = std::filesystem::current_path();
    auto dir = cwd;
    while (true) {
        if (std::filesystem::exists(dir / ".claude" / "templates")) {
            return dir / ".claude" / "templates";
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return cwd / ".claude" / "templates";
}

/// Simple JSON field extraction
inline std::string json_extract(const std::string& content, const std::string& key) {
    auto pos = content.find("\"" + key + "\"");
    if (pos == std::string::npos) return {};
    auto colon = content.find(':', pos);
    if (colon == std::string::npos) return {};
    auto q1 = content.find('"', colon);
    if (q1 == std::string::npos) return {};
    auto q2 = content.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return content.substr(q1 + 1, q2 - q1 - 1);
}

/// Extract JSON array of strings
inline std::vector<std::string> json_extract_array(const std::string& content, const std::string& key) {
    std::vector<std::string> result;
    auto pos = content.find("\"" + key + "\"");
    if (pos == std::string::npos) return result;
    auto arr_start = content.find('[', pos);
    if (arr_start == std::string::npos) return result;
    auto arr_end = content.find(']', arr_start);
    if (arr_end == std::string::npos) return result;
    std::string arr = content.substr(arr_start + 1, arr_end - arr_start - 1);
    size_t p = 0;
    while (p < arr.size()) {
        auto q1 = arr.find('"', p);
        if (q1 == std::string::npos) break;
        auto q2 = arr.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
        p = q2 + 1;
    }
    return result;
}

/// Load a template from a directory (expects template.json + template.md)
inline std::optional<TemplateContent> load_template_from_dir(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;

    // Look for template body file
    std::string body;
    for (const auto& ext : {"md", "txt", "prompt"}) {
        auto path = dir / ("template." + std::string(ext));
        if (fs::exists(path)) {
            std::ifstream ifs(path);
            body = std::string((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
            break;
        }
    }
    if (body.empty()) {
        // Try reading the manifest body field
        auto manifest = dir / "template.json";
        if (fs::exists(manifest)) {
            std::ifstream ifs(manifest);
            std::string content((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
            body = json_extract(content, "body");
        }
    }
    if (body.empty()) return std::nullopt;

    TemplateContent tc;
    tc.id = dir.filename().string();
    tc.body = body;

    // Parse steps (split body on "---" or "## Step" markers)
    std::string_view sv(tc.body);
    size_t pos = 0;
    std::string current_step;
    while (pos < sv.size()) {
        auto nl = sv.find('\n', pos);
        if (nl == std::string_view::npos) nl = sv.size();
        auto line = sv.substr(pos, nl - pos);
        if (line == "---" || line.starts_with("## Step")) {
            if (!current_step.empty()) {
                tc.steps.push_back(std::move(current_step));
                current_step.clear();
            }
        } else {
            current_step += std::string(line) + "\n";
        }
        pos = nl + 1;
    }
    if (!current_step.empty()) {
        tc.steps.push_back(std::move(current_step));
    }

    // Load defaults from manifest
    auto manifest = dir / "template.json";
    if (fs::exists(manifest)) {
        std::ifstream ifs(manifest);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());
        // Extract defaults object (simplified)
        auto defaults_pos = content.find("\"defaults\"");
        if (defaults_pos != std::string::npos) {
            auto brace = content.find('{', defaults_pos);
            auto end_brace = content.find('}', brace);
            if (brace != std::string::npos && end_brace != std::string::npos) {
                std::string obj = content.substr(brace + 1, end_brace - brace - 1);
                size_t p = 0;
                while (p < obj.size()) {
                    auto kq1 = obj.find('"', p);
                    if (kq1 == std::string::npos) break;
                    auto kq2 = obj.find('"', kq1 + 1);
                    if (kq2 == std::string::npos) break;
                    std::string key = obj.substr(kq1 + 1, kq2 - kq1 - 1);
                    auto vq1 = obj.find('"', kq2 + 1);
                    if (vq1 == std::string::npos) break;
                    auto vq2 = obj.find('"', vq1 + 1);
                    if (vq2 == std::string::npos) break;
                    std::string val = obj.substr(vq1 + 1, vq2 - vq1 - 1);
                    tc.defaults[key] = val;
                    p = vq2 + 1;
                }
            }
        }
    }

    return tc;
}

/// Perform variable substitution: replace {{var}} with values
inline std::string substitute_variables(
    const std::string& text,
    const std::map<std::string, std::string>& vars,
    const std::map<std::string, std::string>& defaults) {

    std::string result;
    result.reserve(text.size());
    size_t pos = 0;

    while (pos < text.size()) {
        auto open = text.find("{{", pos);
        if (open == std::string::npos) {
            result += text.substr(pos);
            break;
        }
        result += text.substr(pos, open - pos);

        auto close = text.find("}}", open);
        if (close == std::string::npos) {
            result += text.substr(open);
            break;
        }

        std::string var_name = text.substr(open + 2, close - open - 2);
        // Trim whitespace from var name
        while (!var_name.empty() && var_name.front() == ' ') var_name.erase(var_name.begin());
        while (!var_name.empty() && var_name.back() == ' ') var_name.pop_back();

        // Look up value: provided vars first, then defaults
        auto it = vars.find(var_name);
        if (it != vars.end()) {
            result += it->second;
        } else {
            auto dit = defaults.find(var_name);
            if (dit != defaults.end()) {
                result += dit->second;
            } else {
                // Leave the placeholder as-is if no value available
                result += "{{" + var_name + "}}";
            }
        }

        pos = close + 2;
    }

    return result;
}

/// Extract all {{var}} placeholders from text
inline std::vector<std::string> extract_variables(const std::string& text) {
    std::vector<std::string> vars;
    size_t pos = 0;
    while (pos < text.size()) {
        auto open = text.find("{{", pos);
        if (open == std::string::npos) break;
        auto close = text.find("}}", open);
        if (close == std::string::npos) break;
        std::string var = text.substr(open + 2, close - open - 2);
        while (!var.empty() && var.front() == ' ') var.erase(var.begin());
        while (!var.empty() && var.back() == ' ') var.pop_back();
        if (!var.empty() &&
            std::find(vars.begin(), vars.end(), var) == vars.end()) {
            vars.push_back(var);
        }
        pos = close + 2;
    }
    return vars;
}

} // namespace detail

std::vector<std::string> validate_template(std::string_view id, const std::map<std::string, std::string>& vars);

// Run a template job with the given configuration
std::expected<std::string, std::string> run_template_job(TemplateJob job) {
    namespace fs = std::filesystem;

    if (job.template_id.empty()) {
        return std::unexpected("Template ID cannot be empty");
    }

    // Validate the template exists and variables are satisfied
    auto validation_errors = validate_template(job.template_id, job.variables);
    if (!validation_errors.empty()) {
        std::string error_msg = "Template validation failed:\n";
        for (const auto& err : validation_errors) {
            error_msg += "  - " + err + "\n";
        }
        return std::unexpected(error_msg);
    }

    // Load template content
    std::optional<TemplateContent> content;

    // Search for template in project directory first, then global
    auto project_dir = detail::get_project_templates_dir() / job.template_id;
    auto global_dir = detail::get_global_templates_dir() / job.template_id;

    if (fs::exists(project_dir) && fs::is_directory(project_dir)) {
        content = detail::load_template_from_dir(project_dir);
    } else if (fs::exists(global_dir) && fs::is_directory(global_dir)) {
        content = detail::load_template_from_dir(global_dir);
    }

    if (!content) {
        // Try built-in templates (inline content)
        if (job.template_id == "code-review") {
            content = TemplateContent{
                .id = "code-review",
                .body = "Review the code changes in {{branch}}.\n"
                        "Focus on: correctness, security, performance, readability.\n"
                        "Provide specific suggestions for improvement.",
                .steps = {},
                .defaults = {{"branch", "HEAD"}}
            };
        } else if (job.template_id == "refactor") {
            content = TemplateContent{
                .id = "refactor",
                .body = "Analyze {{target}} for refactoring opportunities.\n"
                        "Consider: DRY violations, complex conditionals, long methods,\n"
                        "poor naming, missing abstractions.",
                .steps = {},
                .defaults = {{"target", "."}}
            };
        } else if (job.template_id == "test-gen") {
            content = TemplateContent{
                .id = "test-gen",
                .body = "Generate unit tests for {{target}}.\n"
                        "Ensure good edge case coverage and use appropriate test framework.",
                .steps = {},
                .defaults = {}
            };
        } else if (job.template_id == "doc-gen") {
            content = TemplateContent{
                .id = "doc-gen",
                .body = "Generate documentation for {{target}}.\n"
                        "Include: API docs, usage examples, and architecture notes.",
                .steps = {},
                .defaults = {}
            };
        }
    }

    if (!content) {
        return std::unexpected("Template '" + job.template_id + "' not found in any template directory");
    }

    // Apply variable substitution
    std::string rendered = detail::substitute_variables(
        content->body, job.variables, content->defaults);

    // Check for unresolved variables
    auto unresolved = detail::extract_variables(rendered);
    if (!unresolved.empty()) {
        std::string warn = "Warning: unresolved variables: ";
        for (size_t i = 0; i < unresolved.size(); ++i) {
            if (i > 0) warn += ", ";
            warn += "{{" + unresolved[i] + "}}";
        }
        rendered += "\n\n" + warn;
    }

    // If output_path is specified, write rendered content to file
    if (job.output_path) {
        std::ofstream ofs(*job.output_path);
        if (!ofs.is_open()) {
            return std::unexpected("Failed to write output to: " + *job.output_path);
        }
        ofs << rendered;
        return std::string("Template job '" + job.name + "' completed. Output: " + *job.output_path);
    }

    // Return the rendered prompt content for execution
    return rendered;
}

// List all available templates from built-in and filesystem directories
std::vector<TemplateInfo> list_templates() {
    namespace fs = std::filesystem;
    std::vector<TemplateInfo> templates;

    // Built-in templates
    templates.push_back(TemplateInfo{
        .id = "code-review",
        .name = "Code Review",
        .description = "Review code changes in the current branch",
        .required_vars = {},
        .optional_vars = {"branch", "files"},
        .source = "builtin"
    });
    templates.push_back(TemplateInfo{
        .id = "refactor",
        .name = "Refactoring",
        .description = "Analyze and refactor code for better maintainability",
        .required_vars = {},
        .optional_vars = {"target"},
        .source = "builtin"
    });
    templates.push_back(TemplateInfo{
        .id = "test-gen",
        .name = "Test Generation",
        .description = "Generate unit tests for specified files",
        .required_vars = {"target"},
        .optional_vars = {"framework"},
        .source = "builtin"
    });
    templates.push_back(TemplateInfo{
        .id = "doc-gen",
        .name = "Documentation",
        .description = "Generate documentation for specified modules",
        .required_vars = {"target"},
        .optional_vars = {"format"},
        .source = "builtin"
    });

    // Scan project templates directory
    auto project_dir = detail::get_project_templates_dir();
    if (fs::exists(project_dir) && fs::is_directory(project_dir)) {
        for (const auto& entry : fs::directory_iterator(project_dir)) {
            if (!entry.is_directory()) continue;
            std::string id = entry.path().filename().string();
            // Skip if already defined as builtin
            bool already_exists = false;
            for (const auto& t : templates) {
                if (t.id == id) { already_exists = true; break; }
            }
            if (already_exists) continue;

            TemplateInfo info;
            info.id = id;
            info.name = id;
            info.source = "project";

            // Try to read metadata from template.json
            auto manifest_path = entry.path() / "template.json";
            if (fs::exists(manifest_path)) {
                std::ifstream ifs(manifest_path);
                std::string content((std::istreambuf_iterator<char>(ifs)),
                                   std::istreambuf_iterator<char>());
                auto name = detail::json_extract(content, "name");
                auto desc = detail::json_extract(content, "description");
                if (!name.empty()) info.name = name;
                if (!desc.empty()) info.description = desc;
                info.required_vars = detail::json_extract_array(content, "required");
                info.optional_vars = detail::json_extract_array(content, "optional");
            }

            templates.push_back(std::move(info));
        }
    }

    // Scan global templates directory
    auto global_dir = detail::get_global_templates_dir();
    if (fs::exists(global_dir) && fs::is_directory(global_dir)) {
        for (const auto& entry : fs::directory_iterator(global_dir)) {
            if (!entry.is_directory()) continue;
            std::string id = entry.path().filename().string();
            bool already_exists = false;
            for (const auto& t : templates) {
                if (t.id == id) { already_exists = true; break; }
            }
            if (already_exists) continue;

            TemplateInfo info;
            info.id = id;
            info.name = id;
            info.source = "global";

            auto manifest_path = entry.path() / "template.json";
            if (fs::exists(manifest_path)) {
                std::ifstream ifs(manifest_path);
                std::string content((std::istreambuf_iterator<char>(ifs)),
                                   std::istreambuf_iterator<char>());
                auto name = detail::json_extract(content, "name");
                auto desc = detail::json_extract(content, "description");
                if (!name.empty()) info.name = name;
                if (!desc.empty()) info.description = desc;
                info.required_vars = detail::json_extract_array(content, "required");
                info.optional_vars = detail::json_extract_array(content, "optional");
            }

            templates.push_back(std::move(info));
        }
    }

    return templates;
}

// Validate that all required variables are provided for a template
std::vector<std::string> validate_template(std::string_view id, const std::map<std::string, std::string>& vars) {
    std::vector<std::string> errors;

    if (id.empty()) {
        errors.push_back("Template ID is required");
        return errors;
    }

    std::string id_str(id);

    // Get template info to check required vars
    auto all_templates = list_templates();
    const TemplateInfo* found = nullptr;
    for (const auto& t : all_templates) {
        if (t.id == id_str) {
            found = &t;
            break;
        }
    }

    if (!found) {
        // Check if it exists on disk
        namespace fs = std::filesystem;
        auto project_dir = detail::get_project_templates_dir() / id_str;
        auto global_dir = detail::get_global_templates_dir() / id_str;
        if (!fs::exists(project_dir) && !fs::exists(global_dir)) {
            errors.push_back("Template '" + id_str + "' not found");
        }
        return errors;
    }

    // Check required variables
    for (const auto& req : found->required_vars) {
        if (vars.find(req) == vars.end()) {
            errors.push_back("Template '" + id_str + "' requires variable '" + req + "'");
        }
    }

    return errors;
}

} // namespace cc::cli::handlers
