module;
#include <string>
#include <functional>
#include <optional>
#include <vector>
#include <unordered_map>
#include <chrono>

export module cc.hooks.turn_diffs;

import cc.state.app_state;

export namespace cc::hooks::turn_diffs {

struct FileDiff {
    std::string file_path;
    std::string old_content;
    std::string new_content;
    int additions{0};
    int deletions{0};
};

struct TurnDiffEntry {
    std::string turn_id;
    std::vector<FileDiff> diffs;
    std::chrono::system_clock::time_point timestamp;
};

struct TurnDiffsState {
    std::vector<TurnDiffEntry> turns;
    std::optional<std::string> active_turn_id;
    std::size_t total_additions{0};
    std::size_t total_deletions{0};
};

struct TurnDiffsOptions {
    std::size_t max_turns_tracked{50};
    bool track_content{true};
};

class TurnDiffsHook {
public:
    explicit TurnDiffsHook(const TurnDiffsOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const TurnDiffsState& state() const { return state_; }

    void begin_turn(std::string turn_id) {
        state_.active_turn_id = std::move(turn_id);
        notify();
    }

    void add_diff(std::string_view turn_id, FileDiff diff) {
        for (auto& turn : state_.turns) {
            if (turn.turn_id == turn_id) {
                state_.total_additions += diff.additions;
                state_.total_deletions += diff.deletions;
                turn.diffs.push_back(std::move(diff));
                notify();
                return;
            }
        }
        // New turn
        TurnDiffEntry entry{
            .turn_id = std::string(turn_id),
            .diffs = {std::move(diff)},
            .timestamp = std::chrono::system_clock::now()
        };
        state_.total_additions += entry.diffs.back().additions;
        state_.total_deletions += entry.diffs.back().deletions;
        state_.turns.push_back(std::move(entry));

        if (state_.turns.size() > options_.max_turns_tracked) {
            state_.turns.erase(state_.turns.begin());
        }
        notify();
    }

    void end_turn() {
        state_.active_turn_id = std::nullopt;
        notify();
    }

    [[nodiscard]] std::optional<TurnDiffEntry> get_turn(std::string_view turn_id) const {
        for (const auto& turn : state_.turns) {
            if (turn.turn_id == turn_id) return turn;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::vector<std::string> modified_files_in_turn(std::string_view turn_id) const {
        std::vector<std::string> files;
        for (const auto& turn : state_.turns) {
            if (turn.turn_id == turn_id) {
                for (const auto& d : turn.diffs) files.push_back(d.file_path);
                break;
            }
        }
        return files;
    }

    void on_change(std::function<void(const TurnDiffsState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    TurnDiffsState state_;
    TurnDiffsOptions options_;
    std::vector<std::function<void(const TurnDiffsState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::turn_diffs
