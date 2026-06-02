/// @file ftxui_integration.cppm
/// @brief FTXUI integration for reactive UI with Claude Code REPL state.
/// Provides utilities to connect FTXUI components to the AppState store.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>

// FTXUI headers would be imported here in a real setup
// #include <ftxui/component/component.hpp>
// #include <ftxui/component/screen_interactive.hpp>

export module cc.state.ftxui_integration;

import cc.state.app_state;
import cc.state.store;
import cc.state.selectors;

export namespace cc::state::ftxui {

// ============================================================
// Reactive Component Wrapper
// ============================================================

/// A wrapper that connects a component to the state store
/// and triggers re-renders on state changes
class ReactiveComponentBase {
protected:
    std::weak_ptr<Store<AppState, decltype(&app_reducer)>> store_;
    SubscriptionId subscription_id_{0};
    std::atomic<bool> needs_render_{false};
    AppState last_state_;

public:
    ReactiveComponentBase() = default;
    virtual ~ReactiveComponentBase() {
        unsubscribe();
    }

    /// Connect to the store and start listening for changes
    void connect(std::shared_ptr<Store<AppState, decltype(&app_reducer)>> store) {
        unsubscribe();
        store_ = store;
        if (auto s = store_.lock()) {
            last_state_ = s->get_state();
            subscription_id_ = s->subscribe([this](const AppState&, const AppState&) {
                on_state_change();
            });
        }
    }

    /// Unsubscribe from store updates
    void unsubscribe() {
        if (auto s = store_.lock()) {
            if (subscription_id_ != 0) {
                s->unsubscribe(subscription_id_);
                subscription_id_ = 0;
            }
        }
    }

    /// Check if a re-render is needed
    [[nodiscard]] bool needs_render() const {
        return needs_render_.load();
    }

    /// Reset the needs-render flag (call after rendering)
    void reset_needs_render() {
        needs_render_.store(false);
    }

    /// Get the current state
    [[nodiscard]] const AppState& get_last_state() const {
        return last_state_;
    }

protected:
    /// Called when state changes - override to handle specific state changes
    virtual void on_state_change() {
        if (auto s = store_.lock()) {
            last_state_ = s->get_state();
            needs_render_.store(true);
        }
    }
};

// ============================================================
// Selector-Based Component
// ============================================================

/// A component that only re-renders when a specific selector's result changes
template <typename SelectorResult>
class SelectorComponent : public ReactiveComponentBase {
    std::function<SelectorResult(const AppState&)> selector_;
    SelectorResult last_selector_result_;
    std::function<void(const SelectorResult&)> on_change_;

public:
    SelectorComponent(
        std::function<SelectorResult(const AppState&)> selector,
        std::function<void(const SelectorResult&)> on_change = nullptr
    ) : selector_(std::move(selector)),
        on_change_(std::move(on_change)) {}

    /// Set the change callback
    void set_on_change(std::function<void(const SelectorResult&)> callback) {
        on_change_ = std::move(callback);
    }

    /// Get the current selector result from the last state
    [[nodiscard]] const SelectorResult& get_selector_result() const {
        return last_selector_result_;
    }

protected:
    void on_state_change() override {
        if (auto s = store_.lock()) {
            auto new_state = s->get_state();
            auto new_result = selector_(new_state);
            if (new_result != last_selector_result_) {
                last_selector_result_ = std::move(new_result);
                last_state_ = std::move(new_state);
                needs_render_.store(true);
                if (on_change_) {
                    on_change_(last_selector_result_);
                }
            }
        }
    }
};

// ============================================================
// Common Reactive Components
// ============================================================

/// A simple component that shows the verbose mode status
class VerboseIndicator : public SelectorComponent<bool> {
public:
    VerboseIndicator()
        : SelectorComponent<bool>(
            [](const AppState& s) { return selectors::is_verbose(s); }
        ) {}

    /// Get display text for the current status
    [[nodiscard]] std::string get_text() const {
        return get_selector_result() ? "VERBOSE" : "";
    }
};

/// Component that shows the loading state
class LoadingIndicator : public SelectorComponent<bool> {
public:
    LoadingIndicator()
        : SelectorComponent<bool>(
            [](const AppState& s) { return selectors::is_loading(s) || selectors::is_streaming(s); }
        ) {}

    /// Get display text for loading state
    [[nodiscard]] std::string get_text() const {
        return get_selector_result() ? "Loading..." : "";
    }
};

/// Component that tracks message count
class MessageCounter : public SelectorComponent<std::size_t> {
public:
    MessageCounter()
        : SelectorComponent<std::size_t>(
            [](const AppState& s) { return selectors::get_message_count(s); }
        ) {}

    /// Get formatted message count text
    [[nodiscard]] std::string get_text() const {
        return std::to_string(get_selector_result()) + " messages";
    }
};

// ============================================================
// Screen Integration
// ============================================================

/// Helper to manage the FTXUI screen with state store integration
class ReactiveScreenManager {
    std::shared_ptr<Store<AppState, decltype(&app_reducer)>> store_;
    std::vector<std::shared_ptr<ReactiveComponentBase>> components_;
    mutable std::mutex components_mutex_;

public:
    explicit ReactiveScreenManager(
        std::shared_ptr<Store<AppState, decltype(&app_reducer)>> store
    ) : store_(std::move(store)) {}

    /// Add a reactive component
    void add_component(std::shared_ptr<ReactiveComponentBase> component) {
        std::lock_guard lock(components_mutex_);
        component->connect(store_);
        components_.push_back(std::move(component));
    }

    /// Remove all components
    void clear_components() {
        std::lock_guard lock(components_mutex_);
        for (auto& comp : components_) {
            comp->unsubscribe();
        }
        components_.clear();
    }

    /// Check if any component needs a re-render
    [[nodiscard]] bool needs_redraw() const {
        std::lock_guard lock(components_mutex_);
        for (const auto& comp : components_) {
            if (comp->needs_render()) {
                return true;
            }
        }
        return false;
    }

    /// Reset needs-render flags on all components
    void reset_needs_render() {
        std::lock_guard lock(components_mutex_);
        for (auto& comp : components_) {
            comp->reset_needs_render();
        }
    }

    /// Get the store
    [[nodiscard]] std::shared_ptr<Store<AppState, decltype(&app_reducer)>> get_store() const {
        return store_;
    }
};

// ============================================================
// Convenience Helpers
// ============================================================

/// Create a reactive component for a specific selector
template <typename SelectorResult>
[[nodiscard]] std::shared_ptr<SelectorComponent<SelectorResult>> make_selector_component(
    std::function<SelectorResult(const AppState&)> selector,
    std::function<void(const SelectorResult&)> on_change = nullptr
) {
    return std::make_shared<SelectorComponent<SelectorResult>>(
        std::move(selector),
        std::move(on_change)
    );
}

/// Create a screen manager with the default store
[[nodiscard]] std::shared_ptr<ReactiveScreenManager> make_reactive_screen_manager(
    std::shared_ptr<Store<AppState, decltype(&app_reducer)>> store
) {
    return std::make_shared<ReactiveScreenManager>(std::move(store));
}

} // namespace cc::state::ftxui
