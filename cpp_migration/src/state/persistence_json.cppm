/// @file persistence_json.cppm
/// @brief Disk-backed persistence for AppStateData using yyjson (Phase 3-F).
///
///   * StateToJson / StateFromJson  — in-memory string round-trip helpers
///   * LoadFromJson / SaveToJson    — disk helpers with atomic rename
///     + fsync on the temp file + fsync on the parent directory so the
///     persisted state is crash-safe on Darwin APFS / ext4.
///
/// Fields are loaded leniently: missing fields fall back to the default
/// AppStateData value so old state files written by earlier builds can
/// always be upgraded by re-saving.  Unknown keys are silently ignored
/// (yyjson discards them naturally) which gives easy forward-compat with
/// fields added in Phase 4+.
module;

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>
#include <system_error>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <fstream>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <yyjson.h>

export module cc.state.persistence_json;

import cc.state.store_impl;

export namespace cc::state::persistence_json {

namespace fs = std::filesystem;
using namespace cc::state::store_impl;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline auto ostr(const char* s, size_t n = 0) -> std::string {
    if (!s) return {};
    if (n == 0) n = std::strlen(s);
    return std::string(s, n);
}

[[nodiscard]] inline auto jstr(yyjson_val* o, const char* k, std::string def = {})
    -> std::string {
    if (!o) return def;
    yyjson_val* v = yyjson_obj_get(o, k);
    if (!v || !yyjson_is_str(v)) return def;
    const char* s = yyjson_get_str(v);
    if (!s) return def;
    return std::string(s, std::strlen(s));
}
[[nodiscard]] inline auto jbool(yyjson_val* o, const char* k, bool def = false) -> bool {
    if (!o) return def;
    yyjson_val* v = yyjson_obj_get(o, k);
    if (!v) return def;
    if (yyjson_is_bool(v)) return yyjson_get_bool(v);
    if (yyjson_is_int(v))  return yyjson_get_int(v) != 0;
    return def;
}
[[nodiscard]] inline auto jint(yyjson_val* o, const char* k, int def = 0) -> int {
    if (!o) return def;
    yyjson_val* v = yyjson_obj_get(o, k);
    if (!v || !yyjson_is_int(v)) return def;
    return static_cast<int>(yyjson_get_int(v));
}
[[nodiscard]] inline auto juint(yyjson_val* o, const char* k, std::uint64_t def = 0) -> std::uint64_t {
    if (!o) return def;
    yyjson_val* v = yyjson_obj_get(o, k);
    if (!v) return def;
    if (yyjson_is_uint(v)) return yyjson_get_uint(v);
    if (yyjson_is_int(v))  return static_cast<std::uint64_t>(yyjson_get_sint(v));
    if (yyjson_is_real(v)) return static_cast<std::uint64_t>(yyjson_get_real(v));
    return def;
}
[[nodiscard]] inline auto jreal(yyjson_val* o, const char* k, double def = 0.0) -> double {
    if (!o) return def;
    yyjson_val* v = yyjson_obj_get(o, k);
    if (!v) return def;
    if (yyjson_is_real(v)) return yyjson_get_real(v);
    if (yyjson_is_int(v))  return static_cast<double>(yyjson_get_sint(v));
    return def;
}
[[nodiscard]] inline auto jtime(yyjson_val* o, const char* k, std::time_t def = 0) -> std::time_t {
    return static_cast<std::time_t>(juint(o, k, static_cast<std::uint64_t>(def)));
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

inline auto add_kv(yyjson_mut_doc* d, yyjson_mut_val* o,
                   const char* k, std::string_view v) -> void {
    // key is NUL-terminated; value is passed with explicit length.
    (void)yyjson_mut_obj_add_strn(d, o, k, v.data(), v.size());
}
inline auto add_kv(yyjson_mut_doc* d, yyjson_mut_val* o,
                   const char* k, bool v) -> void {
    yyjson_mut_obj_add_bool(d, o, k, v);
}
inline auto add_kv(yyjson_mut_doc* d, yyjson_mut_val* o,
                   const char* k, int v) -> void {
    yyjson_mut_obj_add_int(d, o, k, v);
}
inline auto add_kv(yyjson_mut_doc* d, yyjson_mut_val* o,
                   const char* k, std::uint64_t v) -> void {
    yyjson_mut_obj_add_uint(d, o, k, v);
}
inline auto add_kv(yyjson_mut_doc* d, yyjson_mut_val* o,
                   const char* k, double v) -> void {
    yyjson_mut_obj_add_real(d, o, k, v);
}
inline auto add_kv(yyjson_mut_doc* d, yyjson_mut_val* o,
                   const char* k, std::time_t v) -> void {
    yyjson_mut_obj_add_uint(d, o, k, static_cast<std::uint64_t>(v));
}

} // namespace detail

// ---------------------------------------------------------------------------
// StateToJson
// ---------------------------------------------------------------------------

[[nodiscard]] inline auto StateToJson(const AppStateData& s) -> std::string {
    using detail::add_kv;
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (!doc) return {};
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    add_kv(doc, root, "user_id", s.user_id);
    add_kv(doc, root, "revision", s.revision);

    // active_session (nullable)
    if (s.active_session.has_value()) {
        yyjson_mut_val* a = yyjson_mut_obj(doc);
        add_kv(doc, a, "id",       s.active_session->id);
        add_kv(doc, a, "model_id", s.active_session->model_id);
        add_kv(doc, a, "status",   static_cast<int>(s.active_session->status));
        add_kv(doc, a, "branch",   s.active_session->branch);
        add_kv(doc, a, "draft_text", s.active_session->draft_text);
        yyjson_mut_obj_add_val(doc, root, "active_session", a);
    }

    // settings
    {
        yyjson_mut_val* se = yyjson_mut_obj(doc);
        add_kv(doc, se, "theme_name",   s.settings.theme_name);
        add_kv(doc, se, "compact_mode", s.settings.compact_mode);
        add_kv(doc, se, "max_tokens",   s.settings.max_tokens);
        add_kv(doc, se, "temperature",  s.settings.temperature);
        add_kv(doc, se, "model_id",     s.settings.model_id);
        yyjson_mut_obj_add_val(doc, root, "settings", se);
    }

    // flags
    {
        yyjson_mut_val* f = yyjson_mut_obj(doc);
        add_kv(doc, f, "proactive",        s.flags.proactive);
        add_kv(doc, f, "bridge_mode",      s.flags.bridge_mode);
        add_kv(doc, f, "kairos",           s.flags.kairos);
        add_kv(doc, f, "daemon",           s.flags.daemon);
        add_kv(doc, f, "voice_mode",       s.flags.voice_mode);
        add_kv(doc, f, "monitor_tool",     s.flags.monitor_tool);
        add_kv(doc, f, "templates",        s.flags.templates);
        add_kv(doc, f, "bg_sessions",      s.flags.bg_sessions);
        add_kv(doc, f, "byoc_environment", s.flags.byoc_environment);
        add_kv(doc, f, "self_hosted",      s.flags.self_hosted);
        yyjson_mut_obj_add_val(doc, root, "flags", f);
    }

    // recent_sessions
    {
        yyjson_mut_val* arr = yyjson_mut_arr(doc);
        for (const auto& sm : s.recent_sessions) {
            yyjson_mut_val* e = yyjson_mut_obj(doc);
            add_kv(doc, e, "id",         sm.id);
            add_kv(doc, e, "title",      sm.title);
            add_kv(doc, e, "created_at", sm.created_at);
            add_kv(doc, e, "updated_at", sm.updated_at);
            add_kv(doc, e, "turn_count", sm.turn_count);
            yyjson_mut_arr_add_val(arr, e);
        }
        yyjson_mut_obj_add_val(doc, root, "recent_sessions", arr);
    }

    // recent_files
    {
        yyjson_mut_val* arr = yyjson_mut_arr(doc);
        for (const auto& rf : s.recent_files) {
            yyjson_mut_val* e = yyjson_mut_obj(doc);
            add_kv(doc, e, "path",          rf.path);
            add_kv(doc, e, "last_accessed", rf.last_accessed);
            yyjson_mut_arr_add_val(arr, e);
        }
        yyjson_mut_obj_add_val(doc, root, "recent_files", arr);
    }

    // permissions_cache
    {
        yyjson_mut_val* arr = yyjson_mut_arr(doc);
        for (const auto& pe : s.permissions_cache) {
            yyjson_mut_val* e = yyjson_mut_obj(doc);
            add_kv(doc, e, "tool_pattern", pe.tool_pattern);
            add_kv(doc, e, "path_glob",    pe.path_glob);
            add_kv(doc, e, "decision",     pe.decision);
            yyjson_mut_arr_add_val(arr, e);
        }
        yyjson_mut_obj_add_val(doc, root, "permissions_cache", arr);
    }

    size_t olen = 0;
    char* out = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &olen);
    std::string result;
    if (out && olen) result.assign(out, olen);
    if (out) ::free(out);
    yyjson_mut_doc_free(doc);
    return result;
}

// ---------------------------------------------------------------------------
// StateFromJson
// ---------------------------------------------------------------------------

[[nodiscard]] inline auto StateFromJson(std::string_view text)
    -> std::optional<AppStateData> {
    if (text.empty()) return std::nullopt;
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    if (!doc) return std::nullopt;
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return std::nullopt;
    }

    using namespace detail;
    AppStateData s;
    s.user_id  = jstr(root, "user_id");
    s.revision = juint(root, "revision", 0);

    // active_session
    if (yyjson_val* a = yyjson_obj_get(root, "active_session");
        a && yyjson_is_obj(a)) {
        ActiveSession as{};
        as.id       = jstr(a, "id");
        as.model_id = jstr(a, "model_id");
        int st      = jint(a, "status", 0);
        as.status   = static_cast<SessionStatus>(std::clamp(st, 0, 4));
        as.branch   = jstr(a, "branch");
        as.draft_text = jstr(a, "draft_text");
        s.active_session = std::move(as);
    }

    // settings
    if (yyjson_val* se = yyjson_obj_get(root, "settings");
        se && yyjson_is_obj(se)) {
        s.settings.theme_name   = jstr(se, "theme_name", "default");
        s.settings.compact_mode = jbool(se, "compact_mode", false);
        s.settings.max_tokens   = jint(se, "max_tokens", 4096);
        s.settings.temperature  = jreal(se, "temperature", 1.0);
        s.settings.model_id     = jstr(se, "model_id", "claude-sonnet-4-6");
    }

    // flags
    if (yyjson_val* f = yyjson_obj_get(root, "flags");
        f && yyjson_is_obj(f)) {
        s.flags.proactive        = jbool(f, "proactive");
        s.flags.bridge_mode      = jbool(f, "bridge_mode");
        s.flags.kairos           = jbool(f, "kairos");
        s.flags.daemon           = jbool(f, "daemon");
        s.flags.voice_mode       = jbool(f, "voice_mode");
        s.flags.monitor_tool     = jbool(f, "monitor_tool");
        s.flags.templates        = jbool(f, "templates");
        s.flags.bg_sessions      = jbool(f, "bg_sessions");
        s.flags.byoc_environment = jbool(f, "byoc_environment");
        s.flags.self_hosted      = jbool(f, "self_hosted");
    }

    // recent_sessions
    if (yyjson_val* arr = yyjson_obj_get(root, "recent_sessions");
        arr && yyjson_is_arr(arr)) {
        size_t i, max; yyjson_val *v;
        yyjson_arr_foreach(arr, i, max, v) {
            if (!yyjson_is_obj(v)) continue;
            SessionMeta sm{};
            sm.id         = jstr(v, "id");
            sm.title      = jstr(v, "title");
            sm.created_at = jtime(v, "created_at");
            sm.updated_at = jtime(v, "updated_at");
            sm.turn_count = jint(v, "turn_count");
            s.recent_sessions.push_back(std::move(sm));
        }
    }

    // recent_files
    if (yyjson_val* arr = yyjson_obj_get(root, "recent_files");
        arr && yyjson_is_arr(arr)) {
        size_t i, max; yyjson_val* v;
        yyjson_arr_foreach(arr, i, max, v) {
            if (!yyjson_is_obj(v)) continue;
            RecentFile rf{};
            rf.path          = jstr(v, "path");
            rf.last_accessed = jtime(v, "last_accessed");
            s.recent_files.push_back(std::move(rf));
        }
    }

    // permissions_cache
    if (yyjson_val* arr = yyjson_obj_get(root, "permissions_cache");
        arr && yyjson_is_arr(arr)) {
        size_t i, max; yyjson_val* v;
        yyjson_arr_foreach(arr, i, max, v) {
            if (!yyjson_is_obj(v)) continue;
            PermissionEntry pe{};
            pe.tool_pattern = jstr(v, "tool_pattern");
            pe.path_glob    = jstr(v, "path_glob");
            pe.decision     = jint(v, "decision", kPermAllowOnce);
            s.permissions_cache.push_back(std::move(pe));
        }
    }

    yyjson_doc_free(doc);
    return s;
}

// ---------------------------------------------------------------------------
// SaveToJson  (atomic: write tmp → fsync → rename → fsync parent dir)
// ---------------------------------------------------------------------------

[[nodiscard]] inline auto SaveToJson(std::string_view path,
                                     const AppStateData& state)
    -> bool {
    if (path.empty()) return false;
    std::string text = StateToJson(state);
    if (text.empty()) return false;

    std::string tmp{path};
    tmp.append(".tmp");

    // Open O_CREAT|O_TRUNC for best-effort atomicity.
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                    static_cast<mode_t>(0644));
    if (fd < 0) return false;

    const char* data = text.data();
    size_t left = text.size();
    while (left > 0) {
        ssize_t w = ::write(fd, data, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        data += w;
        left -= static_cast<size_t>(w);
    }
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    ::fcntl(fd, F_FULLFSYNC);  // strongest durability guarantee on APFS
#else
    ::fsync(fd);
#endif
    if (::close(fd) != 0) return false;

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); return false; }

    // fsync parent dir so rename is durable.
    fs::path p{std::string{path}};
    fs::path parent = p.has_parent_path() ? p.parent_path() : fs::path{"."};
    int pfd = ::open(parent.c_str(), O_RDONLY);
    if (pfd >= 0) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
        ::fcntl(pfd, F_FULLFSYNC);
#else
        ::fsync(pfd);
#endif
        ::close(pfd);
    }
    return true;
}

// ---------------------------------------------------------------------------
// LoadFromJson
// ---------------------------------------------------------------------------

[[nodiscard]] inline auto LoadFromJson(std::string_view path)
    -> std::optional<AppStateData> {
    if (path.empty()) return std::nullopt;
    std::error_code ec;
    auto sz = fs::file_size(std::string{path}, ec);
    if (ec || sz == 0) return std::nullopt;
    std::ifstream in(std::string{path}, std::ios::binary);
    if (!in) return std::nullopt;
    std::string buf(static_cast<size_t>(sz), '\0');
    in.read(buf.data(), static_cast<std::streamsize>(sz));
    buf.resize(static_cast<size_t>(in.gcount()));
    return StateFromJson(buf);
}

} // namespace cc::state::persistence_json
