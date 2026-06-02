module;

#include <chrono>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.utils.session;


export namespace cc::utils {

// 会话元数据
struct SessionMeta {
    std::string id;
    std::string title;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    size_t message_count{0};
    std::string model_id;
    std::vector<std::string> tags;
    std::string cwd;
};

// 会话筛选器
struct SessionFilter {
    std::optional<std::string> tag;
    std::optional<std::string> cwd;
    std::optional<std::chrono::system_clock::time_point> after;
    size_t limit{20};
};

// 会话存储错误
struct SessionError {
    enum Code { not_found, io_error, parse_error, corrupted };
    Code code;
    std::string message;
};

// 会话存储 — 管理 ~/.cc-repl/sessions/ 下的会话文件
class SessionStorage {
    std::filesystem::path sessions_dir_;

public:
    explicit SessionStorage(std::filesystem::path base_dir)
        : sessions_dir_(std::move(base_dir) / "sessions") {
        std::filesystem::create_directories(sessions_dir_);
    }

    // 使用默认路径 (~/.cc-repl)
    SessionStorage() : SessionStorage(get_default_base_dir()) {}

    // 创建新会话
    [[nodiscard]] auto create_session(std::string_view cwd) -> SessionMeta {
        auto now = std::chrono::system_clock::now();
        SessionMeta meta{
            .id = generate_id(),
            .title = "新会话",
            .created_at = now,
            .updated_at = now,
            .message_count = 0,
            .model_id = "claude-sonnet-4-20250514",
            .cwd = std::string(cwd)
        };
        (void)save_session(meta, "[]");
        return meta;
    }

    // 保存会话 (元数据 + 消息)
    [[nodiscard]] auto save_session(const SessionMeta& meta, std::string_view messages_json)
        -> std::expected<void, SessionError> {
        auto path = sessions_dir_ / (meta.id + ".json");
        std::ofstream out(path);
        if (!out) return std::unexpected(SessionError{SessionError::io_error, "无法写入会话文件"});
        out << "id=" << meta.id << "\n";
        out << "title=" << meta.title << "\n";
        out << "cwd=" << meta.cwd << "\n";
        out << "model_id=" << meta.model_id << "\n";
        out << "message_count=" << meta.message_count << "\n";
        out << "created_at=" << std::chrono::duration_cast<std::chrono::seconds>(meta.created_at.time_since_epoch()).count() << "\n";
        out << "updated_at=" << std::chrono::duration_cast<std::chrono::seconds>(meta.updated_at.time_since_epoch()).count() << "\n";
        out << "messages=" << messages_json << "\n";
        return {};
    }

    // 加载会话
    [[nodiscard]] auto load_session(std::string_view id)
        -> std::expected<SessionMeta, SessionError> {
        auto path = sessions_dir_ / (std::string(id) + ".json");
        if (!std::filesystem::exists(path))
            return std::unexpected(SessionError{SessionError::not_found, "会话不存在"});
        std::ifstream in(path);
        if (!in) return std::unexpected(SessionError{SessionError::io_error, "无法读取会话文件"});
        SessionMeta meta{.id = std::string(id)};
        std::string line;
        while (std::getline(in, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = line.substr(0, eq);
            auto value = line.substr(eq + 1);
            if (key == "id") meta.id = value;
            else if (key == "title") meta.title = value;
            else if (key == "cwd") meta.cwd = value;
            else if (key == "model_id") meta.model_id = value;
            else if (key == "message_count") meta.message_count = static_cast<size_t>(std::stoull(value));
            else if (key == "created_at") meta.created_at = std::chrono::system_clock::time_point{std::chrono::seconds{std::stoll(value)}};
            else if (key == "updated_at") meta.updated_at = std::chrono::system_clock::time_point{std::chrono::seconds{std::stoll(value)}};
        }
        return meta;
    }

    // 列出会话
    [[nodiscard]] auto list_sessions(const SessionFilter& filter = {}) const
        -> std::vector<SessionMeta> {
        std::vector<SessionMeta> results;
        if (!std::filesystem::exists(sessions_dir_)) return results;
        for (const auto& entry : std::filesystem::directory_iterator(sessions_dir_)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            auto loaded = const_cast<SessionStorage*>(this)->load_session(entry.path().stem().string());
            if (!loaded) continue;
            if (filter.cwd && loaded->cwd != *filter.cwd) continue;
            if (filter.after && loaded->updated_at < *filter.after) continue;
            results.push_back(*loaded);
            if (results.size() >= filter.limit) break;
        }
        return results;
    }

    // 删除会话
    [[nodiscard]] auto delete_session(std::string_view id) -> std::expected<void, SessionError> {
        auto path = sessions_dir_ / (std::string(id) + ".json");
        if (!std::filesystem::exists(path))
            return std::unexpected(SessionError{SessionError::not_found, "会话不存在"});
        std::filesystem::remove(path);
        return {};
    }

    // 获取最近会话
    [[nodiscard]] auto get_recent(size_t count = 10) const -> std::vector<SessionMeta> {
        auto all = list_sessions({.limit = count});
        std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
            return a.updated_at > b.updated_at;
        });
        if (all.size() > count) all.resize(count);
        return all;
    }

    // 从第一条消息生成标题
    [[nodiscard]] static auto generate_title(std::string_view first_message) -> std::string {
        if (first_message.size() <= 50)
            return std::string(first_message);
        return std::string(first_message.substr(0, 47)) + "...";
    }

    [[nodiscard]] auto get_sessions_dir() const -> const std::filesystem::path& {
        return sessions_dir_;
    }

private:
    static auto get_default_base_dir() -> std::filesystem::path {
        if (auto* home = std::getenv("HOME"))
            return std::filesystem::path(home) / ".cc-repl";
        return std::filesystem::path(".cc-repl");
    }

    static auto generate_id() -> std::string {
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return "sess_" + std::to_string(now);
    }
};

} // namespace cc::utils
