module;

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.components_extended;

// Import and re-export all new components
export import ui.components.figures;
export import ui.components.fast_icon;
export import ui.components.pr_badge;
export import ui.components.spinner;
export import ui.components.dev_bar;
export import ui.components.stats;
export import cc.ui.components.tag_tabs;
export import ui.components.status_line;
export import ui.components.text_input;
export import cc.ui.components.feature_dialogs;

export namespace cc::ui::components {

// Re-export all types and functions from ui::components into cc::ui::components
// for consistency with the existing codebase
using ::ui::components::FastIconOptions;
using ::ui::components::FastIcon;
using ::ui::components::GetFastIconString;

using ::ui::components::PrReviewState;
using ::ui::components::PrBadgeOptions;
using ::ui::components::PrBadge;

using ::ui::components::SpinnerMode;
using ::ui::components::SpinnerOptions;
using ::ui::components::Spinner;
using ::ui::components::SpinnerElement;

using ::ui::components::SlowOperation;
using ::ui::components::DevBarOptions;
using ::ui::components::DevBar;

using ::ui::components::ModelUsage;
using ::ui::components::DailyActivity;
using ::ui::components::StatsData;
using ::ui::components::StatsOptions;
using ::ui::components::Stats;

using cc::ui::components::Tab;
using cc::ui::components::TagTabsOptions;
using cc::ui::components::TagTabs;
using cc::ui::components::TagTabsComponent;

using ::ui::components::ConnectionStatus;
using ::ui::components::StatusLineOptions;
using ::ui::components::StatusLine;

using ::ui::components::Suggestion;
using ::ui::components::SuggestionCategory;
using ::ui::components::PromptContext;
using ::ui::components::PromptMode;
using ::ui::components::PermissionMode;
using ::ui::components::TextInputOptions;
using ::ui::components::TextInput;
using ::ui::components::TextInputImpl;
using ::ui::components::MakeTextInputCore;

} // namespace cc::ui::components
