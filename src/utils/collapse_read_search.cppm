module;
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.collapse_read_search;

export namespace cc::utils::collapse_read_search {

struct MemoryCounts {
    std::size_t memory_search_count = 0;
    std::size_t memory_read_count = 0;
    std::size_t memory_write_count = 0;
    std::size_t team_memory_search_count = 0;
    std::size_t team_memory_read_count = 0;
    std::size_t team_memory_write_count = 0;
};

struct RecentActivity {
    std::optional<std::string> activity_description = std::nullopt;
    bool is_search = false;
    bool is_read = false;
};

namespace detail {

inline void append_with_count(
    std::vector<std::string>& parts,
    bool is_active,
    std::string_view active_first,
    std::string_view active_later,
    std::string_view completed_first,
    std::string_view completed_later,
    std::size_t count,
    std::string_view singular,
    std::string_view plural
) {
    const bool first = parts.empty();
    std::string part;
    if (is_active) {
        part = std::string(first ? active_first : active_later);
    } else {
        part = std::string(first ? completed_first : completed_later);
    }
    part.push_back(' ');
    part += std::to_string(count);
    part.push_back(' ');
    part += (count == 1 ? singular : plural);
    parts.push_back(std::move(part));
}

inline void append_phrase(
    std::vector<std::string>& parts,
    bool is_active,
    std::string_view active_first,
    std::string_view active_later,
    std::string_view completed_first,
    std::string_view completed_later,
    std::string_view object
) {
    const bool first = parts.empty();
    std::string part;
    if (is_active) {
        part = std::string(first ? active_first : active_later);
    } else {
        part = std::string(first ? completed_first : completed_later);
    }
    part.push_back(' ');
    part += object;
    parts.push_back(std::move(part));
}

[[nodiscard]] inline std::string join_parts(const std::vector<std::string>& parts) {
    std::string result;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) result += ", ";
        result += parts[i];
    }
    return result;
}

} // namespace detail

[[nodiscard]] inline std::string get_search_read_summary_text(
    std::size_t search_count,
    std::size_t read_count,
    bool is_active,
    std::size_t repl_count = 0,
    std::optional<MemoryCounts> memory_counts = std::nullopt,
    std::size_t list_count = 0
) {
    std::vector<std::string> parts;

    if (memory_counts.has_value()) {
        if (memory_counts->memory_read_count > 0) {
            detail::append_with_count(
                parts, is_active, "Recalling", "recalling", "Recalled", "recalled",
                memory_counts->memory_read_count, "memory", "memories");
        }
        if (memory_counts->memory_search_count > 0) {
            detail::append_phrase(
                parts, is_active, "Searching", "searching", "Searched", "searched", "memories");
        }
        if (memory_counts->memory_write_count > 0) {
            detail::append_with_count(
                parts, is_active, "Writing", "writing", "Wrote", "wrote",
                memory_counts->memory_write_count, "memory", "memories");
        }
        if (memory_counts->team_memory_read_count > 0) {
            detail::append_with_count(
                parts, is_active, "Recalling", "recalling", "Recalled", "recalled",
                memory_counts->team_memory_read_count, "team memory", "team memories");
        }
        if (memory_counts->team_memory_search_count > 0) {
            detail::append_phrase(
                parts, is_active, "Searching", "searching", "Searched", "searched", "team memories");
        }
        if (memory_counts->team_memory_write_count > 0) {
            detail::append_with_count(
                parts, is_active, "Writing", "writing", "Wrote", "wrote",
                memory_counts->team_memory_write_count, "team memory", "team memories");
        }
    }

    if (search_count > 0) {
        detail::append_with_count(
            parts, is_active, "Searching for", "searching for", "Searched for", "searched for",
            search_count, "pattern", "patterns");
    }

    if (read_count > 0) {
        detail::append_with_count(
            parts, is_active, "Reading", "reading", "Read", "read",
            read_count, "file", "files");
    }

    if (list_count > 0) {
        detail::append_with_count(
            parts, is_active, "Listing", "listing", "Listed", "listed",
            list_count, "directory", "directories");
    }

    if (repl_count > 0) {
        const std::string repl_verb = is_active ? "REPL'ing" : "REPL'd";
        std::string part = repl_verb + " " + std::to_string(repl_count) + " " +
            (repl_count == 1 ? "time" : "times");
        parts.push_back(std::move(part));
    }

    std::string text = detail::join_parts(parts);
    if (is_active) text += "…";
    return text;
}

[[nodiscard]] inline std::optional<std::string> summarize_recent_activities(
    const std::vector<RecentActivity>& activities
) {
    if (activities.empty()) return std::nullopt;

    std::size_t search_count = 0;
    std::size_t read_count = 0;
    for (std::size_t i = activities.size(); i > 0; --i) {
        const auto& activity = activities[i - 1];
        if (activity.is_search) {
            ++search_count;
        } else if (activity.is_read) {
            ++read_count;
        } else {
            break;
        }
    }

    if (search_count + read_count >= 2) {
        return get_search_read_summary_text(search_count, read_count, true);
    }

    for (std::size_t i = activities.size(); i > 0; --i) {
        const auto& description = activities[i - 1].activity_description;
        if (description.has_value() && !description->empty()) {
            return description;
        }
    }
    return std::nullopt;
}

} // namespace cc::utils::collapse_read_search
