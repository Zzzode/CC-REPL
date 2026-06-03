module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.utils.memdir;


export namespace cc::utils {


enum class MemoryScope { global, project, session };


struct MemoryEntry {
    std::string id;
    std::string content;
    MemoryScope scope;
    std::vector<std::string> tags;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_accessed;
    size_t access_count{0};
    double relevance_score{1.0};
};


struct MemdirConfig {
    std::filesystem::path global_dir;        // ~/.cc-repl/memory/
    std::filesystem::path project_dir;
    size_t max_memories_per_scope{500};
};


class Memdir {
    MemdirConfig config_;

public:
    explicit Memdir(MemdirConfig config) : config_(std::move(config)) {
        std::filesystem::create_directories(config_.global_dir);
    }


    Memdir() : Memdir(get_default_config()) {}


    [[nodiscard]] auto add_memory(std::string content, MemoryScope scope,
        std::vector<std::string> tags = {}) -> std::string {
        auto id = generate_id();
        MemoryEntry entry{
            .id = id, .content = std::move(content), .scope = scope,
            .tags = std::move(tags),
            .created_at = std::chrono::system_clock::now(),
            .last_accessed = std::chrono::system_clock::now()
        };
        save_entry(entry);
        return id;
    }


    [[nodiscard]] auto find_relevant(std::string_view query, MemoryScope scope,
        size_t limit = 10) const -> std::vector<MemoryEntry> {
        auto all = get_all(scope);

        for (auto& entry : all) {
            entry.relevance_score = compute_relevance(entry, query);
        }
        std::sort(all.begin(), all.end(),
            [](const auto& a, const auto& b) { return a.relevance_score > b.relevance_score; });
        if (all.size() > limit) all.resize(limit);
        return all;
    }


    [[nodiscard]] auto get_all(MemoryScope scope) const -> std::vector<MemoryEntry> {
        return scan_memory_files(scope_dir(scope));
    }


    void remove(std::string_view id) {
        for (auto scope : {MemoryScope::global, MemoryScope::project, MemoryScope::session}) {
            auto file = scope_dir(scope) / (std::string(id) + ".md");
            std::error_code ec;
            std::filesystem::remove(file, ec);
        }
    }


    void update_access(std::string_view id) {
        for (auto scope : {MemoryScope::global, MemoryScope::project, MemoryScope::session}) {
            auto file = scope_dir(scope) / (std::string(id) + ".md");
            if (!std::filesystem::exists(file)) continue;
            auto entries = scan_memory_files(scope_dir(scope));
            for (auto& entry : entries) {
                if (entry.id != id) continue;
                entry.last_accessed = std::chrono::system_clock::now();
                ++entry.access_count;
                save_entry(entry);
                return;
            }
        }
    }


    [[nodiscard]] auto get_project_context(const std::filesystem::path& cwd) const -> std::string {

        auto claude_md = cwd / "CLAUDE.md";
        if (std::filesystem::exists(claude_md)) {
            std::ifstream in(claude_md);
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        auto parent = cwd.parent_path();
        if (parent != cwd) return get_project_context(parent);
        return "";
    }


    [[nodiscard]] auto scan_memory_files(const std::filesystem::path& dir) const
        -> std::vector<MemoryEntry> {
        std::vector<MemoryEntry> entries;
        if (!std::filesystem::exists(dir)) return entries;
        for (const auto& item : std::filesystem::directory_iterator(dir)) {
            if (!item.is_regular_file() || item.path().extension() != ".md") continue;
            if (auto parsed = read_entry(item.path())) {
                entries.push_back(std::move(*parsed));
            }
        }
        return entries;
    }


    void prune_stale(size_t max_age_days = 90) {
        auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(24 * max_age_days);
        for (auto scope : {MemoryScope::global, MemoryScope::project, MemoryScope::session}) {
            for (const auto& entry : scan_memory_files(scope_dir(scope))) {
                if (entry.last_accessed < cutoff) {
                    remove(entry.id);
                }
            }
        }
    }


    [[nodiscard]] auto get_team_memories(const std::filesystem::path& team_dir) const
        -> std::vector<MemoryEntry> {
        return scan_memory_files(team_dir);
    }

private:
    static auto get_default_config() -> MemdirConfig {
        std::filesystem::path home = std::getenv("HOME") ? std::getenv("HOME") : ".";
        return {
            .global_dir = home / ".cc-repl" / "memory",
            .project_dir = ".cc-repl" / std::filesystem::path("memory"),
            .max_memories_per_scope = 500
        };
    }

    static auto generate_id() -> std::string {
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return "mem_" + std::to_string(now);
    }

    [[nodiscard]] auto scope_dir(MemoryScope scope) const -> std::filesystem::path {
        switch (scope) {
            case MemoryScope::global: return config_.global_dir;
            case MemoryScope::project: return config_.project_dir;
            case MemoryScope::session: return config_.project_dir / "session";
        }
        return config_.global_dir;
    }

    static auto scope_name(MemoryScope scope) -> std::string_view {
        switch (scope) {
            case MemoryScope::global: return "global";
            case MemoryScope::project: return "project";
            case MemoryScope::session: return "session";
        }
        return "global";
    }

    static auto parse_scope(std::string_view text) -> MemoryScope {
        if (text == "project") return MemoryScope::project;
        if (text == "session") return MemoryScope::session;
        return MemoryScope::global;
    }

    static auto to_count(std::chrono::system_clock::time_point time) -> std::int64_t {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    }

    static auto from_count(std::int64_t value) -> std::chrono::system_clock::time_point {
        return std::chrono::system_clock::time_point{
            std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds{value})
        };
    }

    static auto join_tags(const std::vector<std::string>& tags) -> std::string {
        std::string out;
        for (const auto& tag : tags) {
            if (!out.empty()) out += ",";
            out += tag;
        }
        return out;
    }

    static auto split_tags(std::string_view text) -> std::vector<std::string> {
        std::vector<std::string> tags;
        std::string current;
        for (char ch : text) {
            if (ch == ',') {
                if (!current.empty()) tags.push_back(std::move(current));
                current.clear();
            } else {
                current.push_back(ch);
            }
        }
        if (!current.empty()) tags.push_back(std::move(current));
        return tags;
    }

    void save_entry(const MemoryEntry& entry) const {
        auto dir = scope_dir(entry.scope);
        std::filesystem::create_directories(dir);
        std::ofstream out(dir / (entry.id + ".md"), std::ios::trunc);
        out << "---\n";
        out << "id: " << entry.id << "\n";
        out << "scope: " << scope_name(entry.scope) << "\n";
        out << "created_at: " << to_count(entry.created_at) << "\n";
        out << "last_accessed: " << to_count(entry.last_accessed) << "\n";
        out << "access_count: " << entry.access_count << "\n";
        out << "tags: " << join_tags(entry.tags) << "\n";
        out << "---\n";
        out << entry.content;
        if (!entry.content.ends_with('\n')) out << "\n";
    }

    [[nodiscard]] static auto read_entry(const std::filesystem::path& file) -> std::optional<MemoryEntry> {
        std::ifstream in(file);
        if (!in) return std::nullopt;

        MemoryEntry entry;
        entry.id = file.stem().string();
        entry.scope = MemoryScope::global;
        entry.created_at = std::chrono::system_clock::now();
        entry.last_accessed = entry.created_at;

        std::string line;
        bool in_header = false;
        bool header_done = false;
        std::ostringstream body;
        while (std::getline(in, line)) {
            if (line == "---") {
                if (!in_header && !header_done) {
                    in_header = true;
                    continue;
                }
                if (in_header) {
                    in_header = false;
                    header_done = true;
                    continue;
                }
            }

            if (in_header) {
                auto pos = line.find(':');
                if (pos == std::string::npos) continue;
                auto key = line.substr(0, pos);
                auto value = line.substr(pos + 1);
                while (!value.empty() && value.front() == ' ') value.erase(value.begin());
                if (key == "id") entry.id = value;
                else if (key == "scope") entry.scope = parse_scope(value);
                else if (key == "created_at") entry.created_at = from_count(std::stoll(value));
                else if (key == "last_accessed") entry.last_accessed = from_count(std::stoll(value));
                else if (key == "access_count") entry.access_count = static_cast<size_t>(std::stoull(value));
                else if (key == "tags") entry.tags = split_tags(value);
            } else if (header_done) {
                body << line << "\n";
            }
        }
        entry.content = body.str();
        return entry;
    }

    static auto compute_relevance(const MemoryEntry& entry, std::string_view query) -> double {
        if (query.empty()) return 1.0;
        double score = 0.0;

        if (entry.content.find(query) != std::string::npos) score += 5.0;
        for (const auto& tag : entry.tags) {
            if (tag.find(query) != std::string::npos) score += 3.0;
        }

        auto age = std::chrono::system_clock::now() - entry.last_accessed;
        auto days = std::chrono::duration_cast<std::chrono::hours>(age).count() / 24;
        score *= 1.0 / (1.0 + days * 0.01);
        return score;
    }
};

} // namespace cc::utils
