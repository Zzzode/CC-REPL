// Root-level history module: canonical conversation-history primitives.
// NOTE on deduplication: assistant_history.cppm exports an unrelated
// HistoryMessage type (per-message UI cache with string ids and system-clock
// timestamps).  This module provides the canonical persisted-history types
// that hook/source files will eventually migrate toward.
module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <yyjson.h>

export module cc.history;

import cc.utils.json;

export namespace cc::history {

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

enum class Role : uint8_t {
    System,
    User,
    Assistant,
    Tool,
    ToolResult,
};

struct HistoryMessage {
    uint64_t id = 0;
    Role role = Role::User;
    std::string content_json;
    int64_t created_at_ms = 0;
    std::string model_used;
    uint64_t token_estimate = 0;
    bool compacted = false;
};

struct SessionHistory {
    std::string session_id;
    std::vector<HistoryMessage> messages;
    uint64_t next_id = 1;
    int64_t last_active_ms = 0;

    uint64_t append(HistoryMessage m) {
        m.id = next_id++;
        if (m.created_at_ms == 0) {
            using namespace std::chrono;
            m.created_at_ms = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count();
        }
        last_active_ms = m.created_at_ms;
        messages.push_back(std::move(m));
        return messages.back().id;
    }

    std::vector<HistoryMessage> query(size_t last_N,
                                      std::optional<Role> only_role = std::nullopt) const {
        std::vector<HistoryMessage> out;
        out.reserve(last_N);
        size_t count = 0;
        for (auto it = messages.rbegin(); it != messages.rend() && count < last_N; ++it) {
            if (only_role && it->role != *only_role) continue;
            out.push_back(*it);
            ++count;
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    // Drop messages with id < before_msg_id, insert a compacted summary at
    // the boundary position.  Returns the number of messages dropped.
    size_t compact_before(uint64_t before_msg_id,
                          const std::string& replacement_summary_json) {
        auto it = std::find_if(messages.begin(), messages.end(),
            [&](const HistoryMessage& m) { return m.id >= before_msg_id; });
        size_t dropped = static_cast<size_t>(std::distance(messages.begin(), it));
        if (dropped == 0) return 0;
        HistoryMessage summary;
        summary.id = 0; // placeholder; id only meaningful for uncompacted msgs
        summary.role = Role::System;
        summary.content_json = replacement_summary_json;
        summary.created_at_ms = last_active_ms;
        summary.compacted = true;
        it = messages.erase(messages.begin(), it);
        messages.insert(it, std::move(summary));
        return dropped;
    }

    size_t size() const { return messages.size(); }

    void clear() {
        messages.clear();
        next_id = 1;
    }
};

// ---------------------------------------------------------------------------
// Ser/de
// ---------------------------------------------------------------------------

namespace detail {

inline const char* role_to_str(Role r) {
    switch (r) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool: return "tool";
        case Role::ToolResult: return "tool_result";
    }
    return "user";
}

inline Role role_from_str(std::string_view s) {
    if (s == "system") return Role::System;
    if (s == "user") return Role::User;
    if (s == "assistant") return Role::Assistant;
    if (s == "tool") return Role::Tool;
    if (s == "tool_result") return Role::ToolResult;
    return Role::User;
}

} // namespace detail

inline std::string to_json(const SessionHistory& h) {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    doc.set_root(root);

    root.add("session_id", doc.string(h.session_id));
    root.add("next_id", doc.number(static_cast<int64_t>(h.next_id)));
    root.add("last_active_ms", doc.number(h.last_active_ms));

    auto arr = doc.array();
    for (const auto& m : h.messages) {
        auto obj = doc.object();
        obj.add("id", doc.number(static_cast<int64_t>(m.id)));
        obj.add("role", doc.string(detail::role_to_str(m.role)));
        // content_json: embed parsed JSON if valid, otherwise store as string.
        auto parsed_content = cc::utils::json::parse(m.content_json);
        if (parsed_content) {
            obj.add("content_json", doc.copy_val(parsed_content->root()));
        } else {
            obj.add("content_json", doc.string(m.content_json));
        }
        obj.add("created_at_ms", doc.number(m.created_at_ms));
        if (!m.model_used.empty()) obj.add("model_used", doc.string(m.model_used));
        if (m.token_estimate != 0) obj.add("token_estimate", doc.number(static_cast<int64_t>(m.token_estimate)));
        if (m.compacted) obj.add("compacted", doc.boolean(true));
        arr.append(obj);
    }
    root.add("messages", arr);
    return doc.to_string();
}

inline std::expected<SessionHistory, std::string> history_from_json(std::string_view json_s) {
    auto parsed = cc::utils::json::parse(json_s);
    if (!parsed) return std::unexpected("failed to parse session history JSON");
    auto root = parsed->root();
    if (!root.is_obj()) return std::unexpected("session history JSON root not object");

    SessionHistory h;
    h.session_id = std::string(root.get_string("session_id"));
    h.next_id = static_cast<uint64_t>(std::max<int64_t>(1, root.get_int("next_id")));
    h.last_active_ms = root.get_int("last_active_ms");

    auto msgs = root.get("messages");
    if (msgs.is_arr()) {
        msgs.iter([&](cc::utils::json::JsonVal v) {
            HistoryMessage m;
            m.id = static_cast<uint64_t>(v.get_int("id"));
            m.role = detail::role_from_str(v.get_string("role"));
            if (v.has("content_json")) {
                auto cv = v.get("content_json");
                if (cv.is_str()) {
                    m.content_json = std::string(cv.as_str());
                } else {
                    m.content_json = cv.to_string();
                }
            }
            m.created_at_ms = v.get_int("created_at_ms");
            m.model_used = std::string(v.get_string("model_used"));
            m.token_estimate = static_cast<uint64_t>(v.get_int("token_estimate"));
            m.compacted = v.has("compacted") && v.get("compacted").as_bool();
            h.messages.push_back(std::move(m));
        });
    }
    return h;
}

// ---------------------------------------------------------------------------
// HistoryManager (singleton, thread-safe)
// ---------------------------------------------------------------------------

class HistoryManager {
public:
    static HistoryManager& instance() {
        static HistoryManager inst;
        return inst;
    }

    SessionHistory* create_session(const std::string& sid) {
        std::unique_lock lk(mu_);
        auto [it, ok] = sessions_.try_emplace(sid, SessionHistory{});
        if (ok) it->second.session_id = sid;
        return &it->second;
    }

    SessionHistory* get_session(const std::string& sid) {
        std::shared_lock lk(mu_);
        auto it = sessions_.find(sid);
        return it == sessions_.end() ? nullptr : &it->second;
    }

    bool delete_session(const std::string& sid) {
        std::unique_lock lk(mu_);
        return sessions_.erase(sid) > 0;
    }

    std::vector<std::string> list_sessions() const {
        std::shared_lock lk(mu_);
        std::vector<std::string> out;
        out.reserve(sessions_.size());
        for (const auto& kv : sessions_) out.push_back(kv.first);
        return out;
    }

    std::expected<void, std::string> save_all(const std::filesystem::path& file) const {
        // Serialize under shared lock so we release the mutex during I/O.
        std::string serialized;
        {
            std::shared_lock lk(mu_);
            cc::utils::json::JsonMutDoc doc;
            auto root = doc.object();
            doc.set_root(root);
            auto arr = doc.array();
            for (const auto& kv : sessions_) {
                auto session_json = to_json(kv.second);
                auto parsed = cc::utils::json::parse(session_json);
                if (!parsed) continue;
                arr.append(doc.copy_val(parsed->root()));
            }
            root.add("sessions", arr);
            serialized = doc.to_string();
        }

        std::error_code ec;
        auto dir = file.parent_path();
        if (!dir.empty()) {
            std::filesystem::create_directories(dir, ec);
            if (ec) return std::unexpected("failed to create dir: " + ec.message());
        }
        auto tmp = file;
        tmp += std::string(".tmp.") + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs) return std::unexpected("failed to open tmp file");
            ofs.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
            if (!ofs) return std::unexpected("failed writing tmp file");
        }
        std::filesystem::rename(tmp, file, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return std::unexpected("atomic rename failed: " + ec.message());
        }
        return {};
    }

    std::expected<void, std::string> load_all(const std::filesystem::path& file) {
        auto parsed = cc::utils::json::parse_file(file);
        if (!parsed) return std::unexpected("failed to read history file");
        auto root = parsed->root();
        auto sessions_arr = root.get("sessions");
        if (!sessions_arr.is_arr()) return std::unexpected("sessions array missing");

        std::unordered_map<std::string, SessionHistory> loaded;
        bool ok = true;
        std::string first_err;
        sessions_arr.iter([&](cc::utils::json::JsonVal v) {
            if (!ok) return;
            auto sh = history_from_json(v.to_string());
            if (!sh) { ok = false; first_err = sh.error(); return; }
            if (sh->session_id.empty()) { ok = false; first_err = "session with empty id"; return; }
            loaded.emplace(sh->session_id, std::move(*sh));
        });
        if (!ok) return std::unexpected(first_err);

        std::unique_lock lk(mu_);
        sessions_ = std::move(loaded);
        return {};
    }

private:
    HistoryManager() = default;

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, SessionHistory> sessions_;
};

} // namespace cc::history
