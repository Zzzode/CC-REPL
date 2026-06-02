module;
#include <string>
#include <string_view>
#include <map>
#include <vector>
#include <mutex>
#include <sstream>

export module cc.tools.synthetic_output_tool;

export namespace cc::tools {

// 合成输出结构体
struct SyntheticOutput {
    std::string content;                        // 生成的内容
    std::string format;                         // 输出格式（text, markdown, json, html）
    std::map<std::string, std::string> metadata; // 附加元数据
};

namespace detail {
    // 全局模板注册表（线程安全）
    inline std::mutex g_templates_mutex;
    inline std::map<std::string, std::string> g_templates = {
        // 内置模板
        {"summary", "## Summary\n\n{{content}}\n\n---\n*Generated at {{timestamp}}*"},
        {"error_report", "## Error Report\n\n**Error**: {{error}}\n**Context**: {{context}}\n**Suggestion**: {{suggestion}}"},
        {"task_complete", "✓ Task completed: {{description}}\n\nDuration: {{duration}}\nResult: {{result}}"},
        {"code_review", "## Code Review\n\n**File**: {{file}}\n**Changes**: {{changes}}\n\n### Findings\n{{findings}}"},
        {"progress", "[{{current}}/{{total}}] {{message}}"},
    };
}

// 注册自定义输出模板
inline auto register_output_template(
    std::string_view name,
    std::string_view template_str
) -> void {
    std::lock_guard lock(detail::g_templates_mutex);
    detail::g_templates[std::string(name)] = std::string(template_str);
}

// 根据模板和变量生成合成输出
inline auto generate_synthetic_output(
    std::string_view template_name,
    const std::map<std::string, std::string>& vars
) -> SyntheticOutput {
    std::lock_guard lock(detail::g_templates_mutex);

    // 查找模板
    auto it = detail::g_templates.find(std::string(template_name));
    if (it == detail::g_templates.end()) {
        return SyntheticOutput{
            .content = "Template not found: " + std::string(template_name),
            .format = "text",
            .metadata = {{"error", "template_not_found"}}
        };
    }

    // 模板变量替换：将 {{var}} 替换为对应的值
    std::string result = it->second;
    for (const auto& [key, value] : vars) {
        std::string placeholder = "{{" + key + "}}";
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.size(), value);
            pos += value.size();
        }
    }

    // 清理未替换的占位符（设为空字符串）
    size_t pos = 0;
    while ((pos = result.find("{{", pos)) != std::string::npos) {
        auto end = result.find("}}", pos);
        if (end != std::string::npos) {
            result.replace(pos, end - pos + 2, "");
        } else {
            break;
        }
    }

    // 推断输出格式
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

    // 构建元数据
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
