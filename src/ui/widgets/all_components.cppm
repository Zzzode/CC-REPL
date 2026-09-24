module;

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.widgets.all_components;

import std;

// Import and re-export all new components
export import cc.ui.foundation.components_figures;
export import cc.ui.widgets.fast_icon;
export import cc.ui.widgets.pr_badge;
export import cc.ui.widgets.spinner;
export import cc.ui.widgets.dev_bar;
export import cc.ui.widgets.stats;
export import cc.ui.widgets.tag_tabs;
export import cc.ui.widgets.text_input;
export import cc.ui.dialogs.feature_dialogs;
// Unified canonical PromptInputMode enum — all modules import this from here.
export import cc.ui.foundation.ui_types;

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

using ::ui::components::Suggestion;
using ::ui::components::SuggestionCategory;
using ::ui::components::PromptContext;
using ::ui::components::PromptInputMode;  // unified canonical enum from ui_types.cppm
using ::ui::components::PermissionMode;
using ::ui::components::TextInputOptions;
using ::ui::components::TextInput;
using ::ui::components::TextInputImpl;
using ::ui::components::MakeTextInputCore;

} // namespace cc::ui::components
