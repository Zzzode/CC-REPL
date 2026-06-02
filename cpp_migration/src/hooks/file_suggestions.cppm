module;
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

export module cc.hooks.file_suggestions;

export namespace cc::hooks {

struct FileSuggestion {
  std::string path;
  std::string display_text;
  double score = 0.0;
  bool is_directory = false;
};

class FileSuggestionsHook {
public:
  FileSuggestionsHook() = default;

  auto suggest(std::string_view partial_path, size_t max_results = 15) const -> std::vector<FileSuggestion> {
    std::vector<FileSuggestion> results;

    try {
      std::filesystem::path base_path = std::filesystem::current_path();
      std::filesystem::path search_dir = base_path;
      std::string partial_name = std::string(partial_path);

      auto last_slash = partial_name.find_last_of("/\\");
      if (last_slash != std::string::npos) {
        search_dir = base_path / partial_name.substr(0, last_slash);
        partial_name = partial_name.substr(last_slash + 1);
      }

      if (std::filesystem::exists(search_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(search_dir)) {
          auto filename = entry.path().filename().string();
          if (partial_name.empty() || filename.find(partial_name) != std::string::npos) {
            FileSuggestion suggestion{
              .path = entry.path().lexically_relative(base_path).string(),
              .display_text = filename,
              .score = 1.0,
              .is_directory = entry.is_directory()
            };
            if (suggestion.is_directory) {
              suggestion.path += "/";
            }
            results.push_back(std::move(suggestion));
            if (results.size() >= max_results) break;
          }
        }
      }
    } catch (...) {
    }

    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
      return a.score > b.score;
    });

    return results;
  }

  auto refresh_index() -> void {
    // Re-scan the working directory to pre-warm suggestions
    cached_paths_.clear();
    try {
      auto base = std::filesystem::current_path();
      for (const auto& entry : std::filesystem::directory_iterator(base)) {
        cached_paths_.push_back(entry.path().filename().string());
      }
    } catch (...) {}
  }

  auto clear_cache() -> void {
    cached_paths_.clear();
  }

private:
  std::vector<std::string> cached_paths_;
};

}
