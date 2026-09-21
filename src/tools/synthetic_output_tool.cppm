module;
#include <string>
#include <string_view>
#include <map>
#include <vector>
#include <mutex>
#include <sstream>

export module cc.tools.synthetic_output_tool;

export namespace cc::tools {


struct SyntheticOutput {
    std::string content;
    std::string format;
    std::map<std::string, std::string> metadata;
};

namespace detail {

    inline std::mutex g_templates_mutex;
    inline std::map<std::string, std::string> g_templates = {

        {"summary", "## Summary\n\n{{content}}\n\n---\n*Generated at {{timestamp}}*"},
        {"error_report", "## Error Report\n\n**Error**: {{error}}\n**Context**: {{context}}\n**Suggestion**: {{suggestion}}"},
        {"task_complete", "✓ Task completed: {{description}}\n\nDuration: {{duration}}\nResult: {{result}}"},
        {"code_review", "## Code Review\n\n**File**: {{file}}\n**Changes**: {{changes}}\n\n### Findings\n{{findings}}"},
        {"progress", "[{{current}}/{{total}}] {{message}}"},
    };
}


inline auto register_output_template(
    std::string_view name,
    std::string_view template_str
) -> void {
    std::lock_guard lock(detail::g_templates_mutex);
    detail::g_templates[std::string(name)] = std::string(template_str);
}


inline auto generate_synthetic_output(
    std::string_view template_name,
    const std::map<std::string, std::string>& vars
) -> SyntheticOutput {
    std::lock_guard lock(detail::g_templates_mutex);


    auto it = detail::g_templates.find(std::string(template_name));
    if (it == detail::g_templates.end()) {
        return SyntheticOutput{
            .content = "Template not found: " + std::string(template_name),
            .format = "text",
            .metadata = {{"error", "template_not_found"}}
        };
    }


    std::string result = it->second;
    for (const auto& [key, value] : vars) {
        std::string placeholder = "{{" + key + "}}";
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.size(), value);
            pos += value.size();
        }
    }


    size_t pos = 0;
    while ((pos = result.find("{{", pos)) != std::string::npos) {
        auto end = result.find("}}", pos);
        if (end != std::string::npos) {
            result.replace(pos, end - pos + 2, "");
        } else {
            break;
        }
    }


    std::string format = "text";
    if (result.find('#') != std::string::npos ||
        result.find("**") != std::string::npos) {
        format = "markdown";
    }
    if (result.starts_with("{") || result.starts_with("[")) {
        format = "json";
    }
    if (result.starts_with("<")) {
        format = "html";
    }


    std::map<std::string, std::string> metadata;
    metadata["template"] = std::string(template_name);
    metadata["vars_count"] = std::to_string(vars.size());

    return SyntheticOutput{
        .content = result,
        .format = format,
        .metadata = metadata
    };
}

} // namespace cc::tools
