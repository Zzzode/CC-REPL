module;

#include <filesystem>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <unordered_map>

export module cc.utils.file_history;

namespace fs = std::filesystem;

export namespace cc::utils {


struct FileChange {
    fs::path file;
    std::chrono::system_clock::time_point time;
    enum class Action { Created, Modified, Deleted } action;
};


class FileHistory {
public:
    FileHistory() = default;


    void record(FileChange change) {
        std::lock_guard lock(mutex_);
        std::string key = change.file.string();
        all_changes_.push_back(change);
        per_file_[key].push_back(std::move(change));
    }


    std::vector<FileChange> get_history(const fs::path& filepath) const {
        std::lock_guard lock(mutex_);
        auto it = per_file_.find(filepath.string());
        if (it == per_file_.end()) {
            return {};
        }
        return it->second;
    }


    std::vector<FileChange> get_recent_changes(size_t n) const {
        std::lock_guard lock(mutex_);
        std::vector<FileChange> result;
        size_t count = std::min(n, all_changes_.size());

        result.assign(
            all_changes_.end() - static_cast<ptrdiff_t>(count),
            all_changes_.end()
        );

        std::ranges::sort(result, [](const FileChange& a, const FileChange& b) {
            return a.time > b.time;
        });
        return result;
    }


    void save(const fs::path& filepath) const {
        std::lock_guard lock(mutex_);
        std::ofstream ofs(filepath, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return;

        for (const auto& change : all_changes_) {
            auto time_since_epoch = change.time.time_since_epoch().count();
            int action_int = static_cast<int>(change.action);

            ofs << time_since_epoch << '|'
                << action_int << '|'
                << change.file.string() << '\n';
        }
    }


    void load(const fs::path& filepath) {
        std::lock_guard lock(mutex_);
        std::ifstream ifs(filepath, std::ios::binary);
        if (!ifs.is_open()) return;

        all_changes_.clear();
        per_file_.clear();

        std::string line;
        while (std::getline(ifs, line)) {
            auto first_sep = line.find('|');
            if (first_sep == std::string::npos) continue;
            auto second_sep = line.find('|', first_sep + 1);
            if (second_sep == std::string::npos) continue;


            auto time_str = line.substr(0, first_sep);
            auto action_str = line.substr(first_sep + 1, second_sep - first_sep - 1);
            auto path_str = line.substr(second_sep + 1);

            long long time_val = 0;
            try {
                time_val = std::stoll(time_str);
            } catch (...) {
                continue;
            }

            int action_int = 0;
            try {
                action_int = std::stoi(action_str);
            } catch (...) {
                continue;
            }

            FileChange change{
                .file = fs::path{path_str},
                .time = std::chrono::system_clock::time_point{
                    std::chrono::system_clock::duration{time_val}
                },
                .action = static_cast<FileChange::Action>(action_int)
            };

            std::string key = change.file.string();
            all_changes_.push_back(change);
            per_file_[key].push_back(std::move(change));
        }
    }


    void clear() {
        std::lock_guard lock(mutex_);
        all_changes_.clear();
        per_file_.clear();
    }


    size_t total_count() const {
        std::lock_guard lock(mutex_);
        return all_changes_.size();
    }

private:
    std::vector<FileChange> all_changes_;
    std::unordered_map<std::string, std::vector<FileChange>> per_file_;
    mutable std::mutex mutex_;
};

} // namespace cc::utils
