// C++23 Module: Input suggestion system

module;
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module cc.utils.suggestions;


export namespace cc::utils::suggestions {


enum class SuggestionCategory : uint8_t {
    Command,
    FilePath,
    ShellHistory,
    Skill,
    Custom
};


struct Suggestion {
    std::string text;
    std::string description;
    double score{0.0};
    SuggestionCategory category;


    auto operator<=>(const Suggestion& other) const {
        return other.score <=> score;
    }
    bool operator==(const Suggestion& other) const {
        return text == other.text && category == other.category;
    }
};


struct SlashCommand {
    std::string name;
    std::string description;
    std::vector<std::string> aliases;
};


struct SkillInfo {
    std::string name;
    std::string description;
    std::vector<std::string> triggers;
};


class SuggestionEngine {
public:
    SuggestionEngine() {

        register_default_commands();
    }


    [[nodiscard]] std::vector<Suggestion> suggest_commands(std::string_view partial) const {
        std::vector<Suggestion> results;
        auto lower_partial = to_lower(partial);

        for (const auto& cmd : commands_) {
            double score = compute_match_score(lower_partial, cmd.name);

            for (const auto& alias : cmd.aliases) {
                score = std::max(score, compute_match_score(lower_partial, alias));
            }
            if (score > 0.0) {
                results.push_back({
                    std::format("/{}", cmd.name),
                    cmd.description,
                    score,
                    SuggestionCategory::Command
                });
            }
        }

        std::ranges::sort(results);
        return results;
    }


    [[nodiscard]] std::vector<Suggestion> suggest_files(std::string_view partial) const {
        std::vector<Suggestion> results;
        namespace fs = std::filesystem;


        std::string search_dir = ".";
        std::string prefix;

        if (auto last_sep = partial.rfind('/'); last_sep != std::string_view::npos) {
            search_dir = std::string(partial.substr(0, last_sep));
            if (search_dir.empty()) search_dir = "/";
            prefix = std::string(partial.substr(last_sep + 1));
        } else {
            prefix = std::string(partial);
        }

        std::error_code ec;
        auto dir_iter = fs::directory_iterator(search_dir, ec);
        if (ec) return results;

        auto lower_prefix = to_lower(prefix);
        for (const auto& entry : dir_iter) {
            auto filename = entry.path().filename().string();
            auto lower_filename = to_lower(filename);

            if (lower_filename.starts_with(lower_prefix)) {
                double score = static_cast<double>(lower_prefix.size()) /
                              static_cast<double>(lower_filename.size());
                std::string display = entry.is_directory()
                    ? std::format("{}/", filename) : filename;
                results.push_back({
                    display,
                    entry.is_directory() ? "directory" : "file",
                    score,
                    SuggestionCategory::FilePath
                });
            }
        }

        std::ranges::sort(results);
        return results;
    }


    [[nodiscard]] std::vector<Suggestion> suggest_from_history(std::string_view partial) const {
        std::vector<Suggestion> results;
        auto lower_partial = to_lower(partial);

        for (const auto& entry : history_) {
            double score = compute_match_score(lower_partial, entry);
            if (score > 0.0) {
                results.push_back({
                    entry, "from history", score,
                    SuggestionCategory::ShellHistory
                });
            }
        }

        std::ranges::sort(results);

        if (results.size() > max_history_suggestions_) {
            results.resize(max_history_suggestions_);
        }
        return results;
    }


    [[nodiscard]] std::vector<Suggestion> suggest_skills(std::string_view partial) const {
        std::vector<Suggestion> results;
        auto lower_partial = to_lower(partial);

        for (const auto& skill : skills_) {
            double score = compute_match_score(lower_partial, skill.name);

            for (const auto& trigger : skill.triggers) {
                score = std::max(score, compute_match_score(lower_partial, trigger));
            }
            if (score > 0.0) {
                results.push_back({
                    skill.name, skill.description, score,
                    SuggestionCategory::Skill
                });
            }
        }

        std::ranges::sort(results);
        return results;
    }


    [[nodiscard]] std::vector<Suggestion> combine_and_rank(
        std::vector<Suggestion> all_suggestions) const {

        std::ranges::sort(all_suggestions, {}, &Suggestion::text);
        auto [first, last] = std::ranges::unique(all_suggestions);
        all_suggestions.erase(first, last);


        std::ranges::sort(all_suggestions);


        if (all_suggestions.size() > max_total_suggestions_) {
            all_suggestions.resize(max_total_suggestions_);
        }
        return all_suggestions;
    }


    [[nodiscard]] std::vector<Suggestion> suggest(std::string_view partial) const {
        std::vector<Suggestion> all;


        if (partial.starts_with('/')) {
            auto cmd_partial = partial.substr(1);
            auto cmds = suggest_commands(cmd_partial);
            all.insert(all.end(), cmds.begin(), cmds.end());
        } else {

            auto files = suggest_files(partial);
            auto history = suggest_from_history(partial);
            auto skills = suggest_skills(partial);

            all.insert(all.end(), files.begin(), files.end());
            all.insert(all.end(), history.begin(), history.end());
            all.insert(all.end(), skills.begin(), skills.end());
        }

        return combine_and_rank(std::move(all));
    }


    void register_command(SlashCommand cmd) { commands_.push_back(std::move(cmd)); }

    void register_skill(SkillInfo skill) { skills_.push_back(std::move(skill)); }

    void add_history(std::string entry) { history_.push_back(std::move(entry)); }

private:
    std::vector<SlashCommand> commands_;
    std::vector<SkillInfo> skills_;
    std::vector<std::string> history_;
    size_t max_history_suggestions_{5};
    size_t max_total_suggestions_{10};


    void register_default_commands() {
        commands_ = {
            {"help",    "Show help information",      {"h", "?"}},
            {"exit",    "Exit the REPL",              {"quit", "q"}},
            {"clear",   "Clear screen",               {"cls"}},
            {"compact", "Compact conversation",       {}},
            {"config",  "Edit configuration",         {"settings"}},
            {"doctor",  "Run diagnostics",            {"diag"}},
            {"init",    "Initialize project",         {}},
            {"review",  "Code review mode",           {"cr"}},
            {"model",   "Switch model",               {}},
            {"cost",    "Show token usage",           {"usage"}},
        };
    }


    [[nodiscard]] static double compute_match_score(
        std::string_view query, std::string_view target) {
        if (query.empty()) return 0.1;
        auto lower_target = to_lower(target);


        if (lower_target.starts_with(query)) {
            return 0.9 + 0.1 * (static_cast<double>(query.size()) /
                                 static_cast<double>(lower_target.size()));
        }


        if (lower_target.find(query) != std::string::npos) {
            return 0.5 + 0.3 * (static_cast<double>(query.size()) /
                                 static_cast<double>(lower_target.size()));
        }


        size_t qi = 0;
        for (size_t ti = 0; ti < lower_target.size() && qi < query.size(); ++ti) {
            if (lower_target[ti] == query[qi]) ++qi;
        }
        if (qi == query.size()) {
            return 0.3 * (static_cast<double>(query.size()) /
                          static_cast<double>(lower_target.size()));
        }

        return 0.0;
    }


    [[nodiscard]] static std::string to_lower(std::string_view sv) {
        std::string result(sv);
        std::ranges::transform(result, result.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        return result;
    }
};

} // namespace cc::utils::suggestions
