/// @file tool_ui_skill.cppm
/// @brief Skill tool UI — userFacingName, renderToolUseMessage, etc.
///
/// MODULE:   cc.ui.tools.skill
/// LICENCE:  Exported.  Imported by the tool UI registry initialization.
///
/// TS REFERENCE:
///   src/tools/SkillTool/SkillTool.tsx
///   - userFacingName: "Skill"
///   - renderToolUseMessage: skill name
///   - isTransparentWrapper: false
module;

#include <string>
#include <string_view>
#include <optional>

export module cc.ui.tools.skill;

import cc.ui.tools.registry;

export namespace cc::ui::tools::skill_ui {

using namespace cc::ui::tools;

namespace detail {

/// Extract "skill" field from input JSON.
[[nodiscard]] inline std::string extract_skill_name(std::string_view input_json) {
    auto pos = input_json.find("\"skill\"");
    if (pos == std::string_view::npos) {
        pos = input_json.find("\"name\"");
        if (pos == std::string_view::npos) return {};
    }
    auto colon = input_json.find(':', pos);
    if (colon == std::string_view::npos) return {};
    auto quote = input_json.find('"', colon + 1);
    if (quote == std::string_view::npos) return {};
    auto end = input_json.find('"', quote + 1);
    if (end == std::string_view::npos) return {};
    return std::string(input_json.substr(quote + 1, end - quote - 1));
}

}  // namespace detail

// ============================================================
// Skill tool UI functions
// ============================================================

[[nodiscard]] inline ToolUIFunctions make_skill_ui() {
    ToolUIFunctions fns;

    fns.user_facing_name = [](std::string_view) {
        return std::string{"Skill"};
    };

    fns.message = [](std::string_view input_json) {
        std::string name = detail::extract_skill_name(input_json);
        if (!name.empty()) {
            if (name.size() > 50) {
                return name.substr(0, 47) + "\xE2\x80\xA6";
            }
            return name;
        }
        return std::string{"running skill…"};
    };

    fns.tag = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    fns.progress = [](std::string_view input_json,
                       std::string_view partial_result) {
        std::string name = detail::extract_skill_name(input_json);
        if (!partial_result.empty()) {
            auto nl = partial_result.find('\n');
            std::string line = (nl == std::string_view::npos)
                ? std::string(partial_result)
                : std::string(partial_result.substr(0, nl));
            if (line.size() > 60) {
                line = line.substr(0, 57) + "\xE2\x80\xA6";
            }
            if (!name.empty()) {
                return name + ": " + line;
            }
            return line;
        }
        if (!name.empty()) {
            return "Running " + name + "…";
        }
        return std::string{"Running skill…"};
    };

    fns.queued = [](std::string_view input_json) {
        std::string name = detail::extract_skill_name(input_json);
        if (!name.empty()) {
            return "Waiting to run " + name + "…";
        }
        return std::string{"Waiting to run skill…"};
    };

    fns.is_transparent_wrapper = false;

    // TS REF: SkillTool — renderToolResultMessage shows the skill's output
    // (workflow results, generated text).  The output IS visible on screen,
    // so we index it for search.
    fns.extract_search_text = [](
        std::string_view output_text,
        std::string_view /*error_text*/) -> std::optional<std::string>
    {
        return std::string{output_text};
    };

    return fns;
}

/// Register Skill tool UI in the global registry.
inline void register_skill_ui() {
    global_tool_ui_registry().register_tool_ui("skill", make_skill_ui());
    global_tool_ui_registry().register_tool_ui("Skill", make_skill_ui());
    global_tool_ui_registry().register_tool_ui("SkillTool", make_skill_ui());
}

}  // namespace cc::ui::tools::skill_ui
