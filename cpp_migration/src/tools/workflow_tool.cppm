// WorkflowTool - Executes predefined workflow scripts with steps and conditions
module;
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.workflow;


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


        auto action = ctx.interpolate(step.action);
        if (!action) return std::unexpected(action.error());

        std::string output;

        switch (step.type) {
            case StepType::Command: {

                FILE* pipe = ::popen(action->c_str(), "r");
                if (!pipe) return std::unexpected(WorkflowError::StepFailed);
                std::array<char, 2048> buffer{};
                while (auto n = ::fread(buffer.data(), 1, buffer.size(), pipe)) {
                    output.append(buffer.data(), n);
                }
                int status = ::pclose(pipe);
                if (status != 0) {
                    ctx.set(step.id + ".exit_code", std::to_string(status));
                }
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
