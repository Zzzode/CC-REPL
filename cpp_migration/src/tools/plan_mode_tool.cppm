// PlanModeTool - Enter/exit plan mode for read-only planning sessions
module;
#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.tools.plan_mode;


export namespace cc::tools {

// Error types for plan mode operations
enum class PlanModeError {
    AlreadyInPlanMode,
    NotInPlanMode,
    WriteOperationBlocked,
    PlanDocumentEmpty,
    PlanDocumentTooLarge,
    InvalidTransition,
};

constexpr auto format_error(PlanModeError err) -> std::string_view {
    switch (err) {
        case PlanModeError::AlreadyInPlanMode:     return "Already in plan mode";
        case PlanModeError::NotInPlanMode:         return "Not currently in plan mode";
        case PlanModeError::WriteOperationBlocked: return "Write operations are blocked in plan mode";
        case PlanModeError::PlanDocumentEmpty:     return "Plan document is empty";
        case PlanModeError::PlanDocumentTooLarge:  return "Plan document exceeds size limit";
        case PlanModeError::InvalidTransition:     return "Invalid plan mode state transition";
        default:                                   return "Unknown plan mode error";
    }
}

// Plan document section
struct PlanSection {
    std::string title;
    std::string content;
    bool completed{false};
    size_t order{0};
};

// Plan document representing the planning state
struct PlanDocument {
    std::string title;
    std::string summary;
    std::vector<PlanSection> sections;
    std::chrono::system_clock::time_point created_at;
    std::optional<std::chrono::system_clock::time_point> finalized_at;

    [[nodiscard]] auto total_sections() const -> size_t { return sections.size(); }
    [[nodiscard]] auto completed_sections() const -> size_t {
        return static_cast<size_t>(std::count_if(
            sections.begin(), sections.end(),
            [](const auto& s) { return s.completed; }));
    }
};

// Set of tools allowed in plan mode (read-only operations)
inline constexpr std::array kAllowedToolsInPlanMode = {
    std::string_view{"file_read"},
    std::string_view{"glob"},
    std::string_view{"grep"},
    std::string_view{"web_search"},
    std::string_view{"web_fetch"},
    std::string_view{"lsp"},
    std::string_view{"ask_user"},
    std::string_view{"todo_write"},
};

// Plan mode state tracker (singleton-style)
class PlanModeState {
public:
    static PlanModeState& instance() {
        static PlanModeState state;
        return state;
    }

    [[nodiscard]] bool is_active() const { return active_; }
    [[nodiscard]] const PlanDocument* current_plan() const {
        return active_ ? &plan_ : nullptr;
    }

    // Check if a tool is allowed in the current mode
    [[nodiscard]] bool is_tool_allowed(std::string_view tool_name) const {
        if (!active_) return true;  // All tools allowed outside plan mode
        return std::any_of(kAllowedToolsInPlanMode.begin(), kAllowedToolsInPlanMode.end(), [&](auto allowed) {
            return allowed == tool_name;
        });
    }

    auto enter(std::string title, std::string summary) -> std::expected<void, PlanModeError> {
        if (active_) return std::unexpected(PlanModeError::AlreadyInPlanMode);

        plan_ = PlanDocument{
            .title = std::move(title),
            .summary = std::move(summary),
            .sections = {},
            .created_at = std::chrono::system_clock::now(),
            .finalized_at = std::nullopt
        };
        active_ = true;
        return {};
    }

    auto exit() -> std::expected<PlanDocument, PlanModeError> {
        if (!active_) return std::unexpected(PlanModeError::NotInPlanMode);

        plan_.finalized_at = std::chrono::system_clock::now();
        active_ = false;
        return std::move(plan_);
    }

    auto add_section(std::string title, std::string content) -> std::expected<void, PlanModeError> {
        if (!active_) return std::unexpected(PlanModeError::NotInPlanMode);
        plan_.sections.push_back(PlanSection{
            .title = std::move(title),
            .content = std::move(content),
            .order = plan_.sections.size(),
        });
        return {};
    }

    auto mark_section_complete(size_t index) -> std::expected<void, PlanModeError> {
        if (!active_) return std::unexpected(PlanModeError::NotInPlanMode);
        if (index >= plan_.sections.size()) return std::unexpected(PlanModeError::InvalidTransition);
        plan_.sections[index].completed = true;
        return {};
    }

private:
    PlanModeState() = default;
    bool active_{false};
    PlanDocument plan_;
};

// EnterPlanModeTool - activates plan mode
class EnterPlanModeTool {
public:
    static constexpr std::string_view name = "enter_plan_mode";
    static constexpr std::string_view description = "Enter plan mode for read-only exploration and planning";

    auto execute(std::string title, std::string summary = "")
        -> std::expected<void, PlanModeError>
    {
        if (title.empty()) {
            return std::unexpected(PlanModeError::PlanDocumentEmpty);
        }
        return PlanModeState::instance().enter(std::move(title), std::move(summary));
    }

    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "title": {{ "type": "string", "description": "Title for the plan document" }},
      "summary": {{ "type": "string", "description": "Optional summary of the planning goal" }}
    }},
    "required": ["title"]
  }}
}})", name, description);
    }
};

// ExitPlanModeTool - deactivates plan mode and returns the plan document
class ExitPlanModeTool {
public:
    static constexpr std::string_view name = "exit_plan_mode";
    static constexpr std::string_view description = "Exit plan mode and finalize the plan document";

    auto execute() -> std::expected<PlanDocument, PlanModeError> {
        return PlanModeState::instance().exit();
    }

    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{}}
  }}
}})", name, description);
    }
};

} // namespace cc::tools
