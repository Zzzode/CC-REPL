module;
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

export module cc.hooks.history_search;

export namespace cc::hooks {

struct HistoryEntry {
  std::string id;
  std::string display_text;
  std::string value;
  std::chrono::system_clock::time_point timestamp;
  std::unordered_map<std::string, std::string> metadata;
};

using HistorySelectCallback = std::function<void(const HistoryEntry&)>;

class HistorySearchHook {
public:
  HistorySearchHook() = default;

  auto set_history(std::vector<HistoryEntry> history) -> void {
    history_ = std::move(history);
  }

  auto start_search(std::string_view initial_query = "") -> void {
    searching_ = true;
    query_ = std::string(initial_query);
    update_matches();
  }

  auto end_search() -> void {
    searching_ = false;
    query_.clear();
    matches_.clear();
    current_match_index_ = 0;
  }

  auto set_query(std::string_view query) -> void {
    query_ = std::string(query);
    update_matches();
  }

  auto get_query() const -> std::string_view {
    return query_;
  }

  auto get_current_match() const -> std::optional<HistoryEntry> {
    if (matches_.empty() || current_match_index_ >= matches_.size()) {
      return std::nullopt;
    }
    return history_[matches_[current_match_index_]];
  }

  auto next_match() -> void {
    if (!matches_.empty()) {
      current_match_index_ = (current_match_index_ + 1) % matches_.size();
    }
  }

  auto prev_match() -> void {
    if (!matches_.empty()) {
      current_match_index_ = (current_match_index_ - 1 + matches_.size()) % matches_.size();
    }
  }

  auto accept_match(HistorySelectCallback callback) -> void {
    if (auto match = get_current_match()) {
      callback(*match);
    }
    end_search();
  }

  auto is_searching() const -> bool { return searching_; }
  auto has_matches() const -> bool { return !matches_.empty(); }
  auto match_count() const -> size_t { return matches_.size(); }

private:
  auto update_matches() -> void {
    matches_.clear();
    for (size_t i = 0; i < history_.size(); ++i) {
      if (query_.empty() || history_[i].display_text.find(query_) != std::string::npos) {
        matches_.push_back(i);
      }
    }
    current_match_index_ = 0;
  }

  std::vector<HistoryEntry> history_;
  std::vector<size_t> matches_;
  size_t current_match_index_ = 0;
  std::string query_;
  bool searching_ = false;
};

}
