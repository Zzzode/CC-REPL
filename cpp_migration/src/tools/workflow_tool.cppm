// WorkflowTool - Executes predefined workflow scripts with steps and conditions
module;
#include <array>
#include <chrono>
#include <cstddef>
#include <cctype>
#include <cstdio>
#include <expected>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.workflow;

import cc.utils.json;
import cc.utils.bash_execution;

export namespace cc::tools {


enum class WorkflowError {
    DefinitionEmpty,
    StepFailed,
    ConditionFailed,
    VariableNotFound,
    InvalidStep,
    LoopLimitExceeded,
    InterpolationFailed,
    Timeout,
};

constexpr auto format_error(WorkflowError err) -> std::string_view {
    switch (err) {
        case WorkflowError::DefinitionEmpty:     return "Workflow definition is empty";
        case WorkflowError::StepFailed:          return "Workflow step execution failed";
        case WorkflowError::ConditionFailed:     return "Workflow condition evaluation failed";
        case WorkflowError::VariableNotFound:    return "Variable not found in workflow context";
        case WorkflowError::InvalidStep:         return "Invalid workflow step definition";
        case WorkflowError::LoopLimitExceeded:   return "Loop iteration limit exceeded";
        case WorkflowError::InterpolationFailed: return "Variable interpolation failed";
        case WorkflowError::Timeout:             return "Workflow execution timed out";
        default:                                 return "Unknown workflow error";
    }
}


enum class StepType {
    Command,
    Condition,
    Loop,
    Assign,
    Log,
};


struct WorkflowStep {
    std::string id;
    std::string name;
    StepType type{StepType::Command};
    std::string action;
    std::optional<std::string> condition;
    std::optional<size_t> max_iterations;
    std::vector<std::string> on_error;
};


struct WorkflowDefinition {
    std::string name;
    std::string description;
    std::vector<WorkflowStep> steps;
    std::unordered_map<std::string, std::string> initial_vars;
};


struct StepResult {
    std::string step_id;
    bool success{true};
    std::string output;
    std::chrono::milliseconds duration{0};
    std::optional<std::string> error_message;
};


struct WorkflowResult {
    std::string workflow_name;
    bool success{true};
    std::vector<StepResult> step_results;
    std::chrono::milliseconds total_duration{0};
    size_t steps_executed{0};
    size_t steps_skipped{0};
};


class VariableContext {
public:
    auto set(std::string key, std::string value) -> void {
        vars_[std::move(key)] = std::move(value);
    }

    auto get(std::string_view key) const -> std::expected<std::string_view, WorkflowError> {
        auto it = vars_.find(std::string(key));
        if (it == vars_.end()) return std::unexpected(WorkflowError::VariableNotFound);
        return std::string_view{it->second};
    }


    auto interpolate(std::string_view tmpl) const -> std::expected<std::string, WorkflowError> {
        std::string result;
        size_t pos = 0;

        while (pos < tmpl.size()) {

            auto start = tmpl.find("${", pos);
            if (start == std::string_view::npos) {
                result.append(tmpl.substr(pos));
                break;
            }


            result.append(tmpl.substr(pos, start - pos));


            auto end = tmpl.find('}', start + 2);
            if (end == std::string_view::npos) {
                return std::unexpected(WorkflowError::InterpolationFailed);
            }


            auto var_name = tmpl.substr(start + 2, end - start - 2);
            auto value = get(var_name);
            if (!value) return std::unexpected(value.error());
            result.append(*value);

            pos = end + 1;
        }
        return result;
    }

    auto all() const -> const std::unordered_map<std::string, std::string>& {
        return vars_;
    }

private:
    std::unordered_map<std::string, std::string> vars_;
};

[[nodiscard]] inline bool workflow_truthy(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    std::string lower;
    lower.reserve(value.size());
    for (unsigned char ch : value) lower.push_back(static_cast<char>(std::tolower(ch)));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

[[nodiscard]] inline std::optional<std::string> workflow_json_string(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    if (!value.is_str()) return std::nullopt;
    return std::string(value.as_str());
}

[[nodiscard]] inline StepType parse_workflow_step_type(std::string_view type) {
    if (type == "condition" || type == "if") return StepType::Condition;
    if (type == "loop" || type == "repeat") return StepType::Loop;
    if (type == "assign" || type == "set") return StepType::Assign;
    if (type == "log" || type == "message") return StepType::Log;
    return StepType::Command;
}

[[nodiscard]] inline std::expected<WorkflowDefinition, WorkflowError> parse_workflow_definition_json(
    std::string_view text
) {
    auto parsed = cc::utils::json::parse(text);
    if (!parsed || !parsed->root().is_obj()) {
        return std::unexpected(WorkflowError::InvalidStep);
    }
    auto root = parsed->root();
    WorkflowDefinition definition;
    definition.name = workflow_json_string(root, "name").value_or("workflow");
    definition.description = workflow_json_string(root, "description").value_or("");

    auto variables = root.get("variables");
    if (!variables.is_obj()) variables = root.get("initial_vars");
    if (variables.is_obj()) {
        variables.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
            if (!key.is_str()) return;
            if (value.is_str()) definition.initial_vars[std::string(key.as_str())] = std::string(value.as_str());
            else if (value.is_num()) definition.initial_vars[std::string(key.as_str())] = std::to_string(value.as_int());
            else if (value.is_bool()) definition.initial_vars[std::string(key.as_str())] = value.as_bool() ? "true" : "false";
        });
    }

    auto steps = root.get("steps");
    if (!steps.is_arr()) return std::unexpected(WorkflowError::DefinitionEmpty);
    std::size_t index = 0;
    steps.iter([&](cc::utils::json::JsonVal item) {
        if (!item.is_obj()) return;
        WorkflowStep step;
        step.id = workflow_json_string(item, "id").value_or(std::format("step-{}", index + 1));
        step.name = workflow_json_string(item, "name").value_or(step.id);
        step.type = parse_workflow_step_type(workflow_json_string(item, "type").value_or("command"));
        step.action = workflow_json_string(item, "action")
            .or_else([&] { return workflow_json_string(item, "command"); })
            .or_else([&] { return workflow_json_string(item, "value"); })
            .value_or("");
        step.condition = workflow_json_string(item, "condition");
        auto max_iterations = item.get("max_iterations");
        if (!max_iterations.is_num()) max_iterations = item.get("maxIterations");
        if (max_iterations.is_num() && max_iterations.as_int() > 0) {
            step.max_iterations = static_cast<std::size_t>(max_iterations.as_int());
        }
        auto on_error = item.get("on_error");
        if (!on_error.is_arr()) on_error = item.get("onError");
        if (on_error.is_arr()) {
            on_error.iter([&](cc::utils::json::JsonVal entry) {
                if (entry.is_str()) step.on_error.push_back(std::string(entry.as_str()));
            });
        }
        definition.steps.push_back(std::move(step));
        ++index;
    });
    return definition;
}


class WorkflowTool {
public:
    static constexpr std::string_view name = "workflow";
    static constexpr std::string_view description = "Execute predefined workflow scripts with steps and conditions";
    static constexpr size_t kDefaultMaxLoop = 100;

    auto validate(const WorkflowDefinition& def) const -> std::expected<void, WorkflowError> {
        if (def.steps.empty()) {
            return std::unexpected(WorkflowError::DefinitionEmpty);
        }
        for (const auto& step : def.steps) {
            if (step.id.empty() || step.action.empty()) {
                return std::unexpected(WorkflowError::InvalidStep);
            }
        }
        return {};
    }

    auto execute(WorkflowDefinition definition) -> std::expected<WorkflowResult, WorkflowError> {
        if (auto v = validate(definition); !v) return std::unexpected(v.error());

        auto start_time = std::chrono::steady_clock::now();
        VariableContext ctx;


        for (const auto& [key, value] : definition.initial_vars) {
            ctx.set(key, value);
        }

        WorkflowResult result{.workflow_name = definition.name};


        for (const auto& step : definition.steps) {
            if (step.condition) {
                auto condition = ctx.interpolate(*step.condition);
                if (!condition) return std::unexpected(condition.error());
                if (!workflow_truthy(*condition)) {
                    result.steps_skipped++;
                    continue;
                }
            }

            auto step_result = execute_step(step, ctx);

            if (step_result) {
                result.step_results.push_back(std::move(*step_result));
                result.steps_executed++;
            } else {

                StepResult err_result{
                    .step_id = step.id,
                    .success = false,
                    .error_message = std::string(format_error(step_result.error())),
                };
                result.step_results.push_back(std::move(err_result));
                result.success = false;

                if (step.on_error.empty()) break;
            }
        }

        result.total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        return result;
    }

    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "name": {{ "type": "string", "description": "Workflow name" }},
      "steps": {{ "type": "array", "items": {{ "type": "object" }}, "description": "Workflow step definitions" }},
      "variables": {{ "type": "object", "description": "Initial variable values" }}
    }},
    "required": ["name", "steps"]
  }}
}})", name, description);
    }

private:

    auto execute_step(const WorkflowStep& step, VariableContext& ctx)
        -> std::expected<StepResult, WorkflowError>
    {
        auto start = std::chrono::steady_clock::now();


        std::optional<std::string> action;
        if (step.type != StepType::Loop) {
            auto interpolated = ctx.interpolate(step.action);
            if (!interpolated) return std::unexpected(interpolated.error());
            action = std::move(*interpolated);
        }

        std::string output;

        switch (step.type) {
            case StepType::Command: {

                FILE* pipe = cc::utils::bash::popen_spawn(action->c_str());
                if (!pipe) return std::unexpected(WorkflowError::StepFailed);
                std::array<char, 2048> buffer{};
                while (auto n = ::fread(buffer.data(), 1, buffer.size(), pipe)) {
                    output.append(buffer.data(), n);
                }
                int status = cc::utils::bash::pclose_spawn(pipe);
                if (status != 0) {
                    ctx.set(step.id + ".exit_code", std::to_string(status));
                    ctx.set(step.id + ".output", output);
                    return std::unexpected(WorkflowError::StepFailed);
                }
                break;
            }
            case StepType::Condition: {
                if (!workflow_truthy(*action)) return std::unexpected(WorkflowError::ConditionFailed);
                output = *action;
                break;
            }
            case StepType::Loop: {
                const auto limit = step.max_iterations.value_or(kDefaultMaxLoop);
                if (limit > kDefaultMaxLoop) return std::unexpected(WorkflowError::LoopLimitExceeded);
                std::ostringstream combined;
                for (std::size_t i = 0; i < limit; ++i) {
                    ctx.set(step.id + ".index", std::to_string(i));
                    auto loop_action = ctx.interpolate(step.action);
                    if (!loop_action) return std::unexpected(loop_action.error());
                    FILE* pipe = cc::utils::bash::popen_spawn(loop_action->c_str());
                    if (!pipe) return std::unexpected(WorkflowError::StepFailed);
                    std::array<char, 2048> buffer{};
                    while (auto n = ::fread(buffer.data(), 1, buffer.size(), pipe)) {
                        combined.write(buffer.data(), static_cast<std::streamsize>(n));
                    }
                    int status = cc::utils::bash::pclose_spawn(pipe);
                    if (status != 0) {
                        output = combined.str();
                        ctx.set(step.id + ".exit_code", std::to_string(status));
                        ctx.set(step.id + ".output", output);
                        return std::unexpected(WorkflowError::StepFailed);
                    }
                }
                output = combined.str();
                break;
            }
            case StepType::Assign: {

                auto eq_pos = action->find('=');
                if (eq_pos != std::string::npos) {
                    ctx.set(action->substr(0, eq_pos), action->substr(eq_pos + 1));
                }
                output = *action;
                break;
            }
            case StepType::Log: {
                output = *action;
                break;
            }
            default:
                output = std::format("[Step type {} not fully implemented]",
                    static_cast<int>(step.type));
                break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);


        ctx.set(step.id + ".output", output);

        return StepResult{
            .step_id = step.id,
            .success = true,
            .output = std::move(output),
            .duration = elapsed,
        };
    }
};

} // namespace cc::tools
