module;

#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.skill_tool;

export namespace cc::tools::skill {

struct SkillInvocationRequest {
    std::string name;
    std::vector<std::string> arguments;
    std::optional<std::string> variant;
};

struct SkillInvocationResult {
    bool accepted{false};
    std::string summary;
};

[[nodiscard]] inline auto format_invocation(const SkillInvocationRequest& request) -> std::string {
    std::string out = request.name;
    if (request.variant) {
        out += ":" + *request.variant;
    }
    for (const auto& arg : request.arguments) {
        out += " " + arg;
    }
    return out;
}

[[nodiscard]] inline auto prepare_skill_invocation(const SkillInvocationRequest& request) -> SkillInvocationResult {
    if (request.name.empty()) {
        return {.accepted = false, .summary = "Skill name is required"};
    }
    return {.accepted = true, .summary = "Prepared skill invocation: " + format_invocation(request)};
}

} // namespace cc::tools::skill
