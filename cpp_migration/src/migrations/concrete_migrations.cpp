/// @file concrete_migrations.cpp
/// @brief 11 concrete configuration migrations with real JSON mutations.
///
/// Each migration is split into two phases:
///   1. detect(DetectCtx) -> bool
///      Pure read-only check on the three immutable input snapshots
///      (globalConfig, userSettings, localSettings).  Returns true when
///      this migration needs to run.
///   2. apply(ConfigCtx&)
///      Mutates the three logical config trees backed by a shared
///      JsonMutDoc.  Performs real yyjson mutations: key renames, value
///      rewrites, nested object creation, array merges, boolean coercions
///      and completion-flag writes — all of which serialise back to the
///      correct JSON on disk.
///
/// The three logical config trees correspond to the on-disk files:
///   - globalConfig: ~/.config/cc-repl/globalConfig.json
///   - userSettings: ~/.config/cc-repl/settings.json
///   - localSettings: .claude/settings.json  (project-local)
module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <yyjson.h>

module cc.migrations.concrete;

import cc.utils.json;

namespace cc::migrations::concrete {

using cc::utils::json::JsonDoc;
using cc::utils::json::JsonMutDoc;
using cc::utils::json::JsonMutVal;
using cc::utils::json::JsonVal;

// ============================================================
// Timestamp helper
// ============================================================

[[nodiscard]] inline int64_t now_epoch_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

// ============================================================
// Mutation helpers for yyjson 0.10 (mut_* API on yyjson_mut_val)
// ============================================================

namespace {
namespace mut {

// --- raw access -------------------------------------------------------------

inline yyjson_mut_val* raw(const JsonMutVal& v) noexcept {
    return const_cast<JsonMutVal&>(v).raw();
}

// --- type checks on mutable values -----------------------------------------

[[nodiscard]] inline bool is_obj(yyjson_mut_val* v) noexcept {
    return v && yyjson_mut_is_obj(v);
}
[[nodiscard]] inline bool is_arr(yyjson_mut_val* v) noexcept {
    return v && yyjson_mut_is_arr(v);
}
[[nodiscard]] inline bool is_str(yyjson_mut_val* v) noexcept {
    return v && yyjson_mut_is_str(v);
}
[[nodiscard]] inline bool is_true(yyjson_mut_val* v) noexcept {
    return v && yyjson_mut_is_true(v);
}
[[nodiscard]] inline bool is_bool(yyjson_mut_val* v) noexcept {
    return v && yyjson_mut_is_bool(v);
}
[[nodiscard]] inline bool is_sint(yyjson_mut_val* v) noexcept {
    return v && yyjson_mut_is_sint(v);
}
[[nodiscard]] inline bool is_uint(yyjson_mut_val* v) noexcept {
    return v && yyjson_mut_is_uint(v);
}
// --- value reads on mutable values -----------------------------------------

[[nodiscard]] inline std::string_view get_str(yyjson_mut_val* v) noexcept {
    if (!is_str(v)) return {};
    return {yyjson_mut_get_str(v), yyjson_mut_get_len(v)};
}
[[nodiscard]] inline int64_t get_sint(yyjson_mut_val* v) noexcept {
    return is_sint(v) ? yyjson_mut_get_sint(v) : 0;
}
[[nodiscard]] inline uint64_t get_uint(yyjson_mut_val* v) noexcept {
    return is_uint(v) ? yyjson_mut_get_uint(v) : 0;
}
[[nodiscard]] inline bool get_bool(yyjson_mut_val* v) noexcept {
    return is_true(v);
}
// --- lookup / create --------------------------------------------------------

[[nodiscard]] inline yyjson_mut_val* obj_getn(yyjson_mut_val* obj,
                                              std::string_view key) noexcept {
    if (!is_obj(obj)) return nullptr;
    return yyjson_mut_obj_getn(obj, key.data(), key.size());
}

[[nodiscard]] inline bool has(yyjson_mut_val* obj,
                              std::string_view key) noexcept {
    return obj_getn(obj, key) != nullptr;
}

/// Remove a key from a mutable object (no-op if missing).
inline void remove(yyjson_mut_val* obj, std::string_view key) noexcept {
    if (!is_obj(obj)) return;
    yyjson_mut_obj_remove_keyn(obj, key.data(), key.size());
}

/// Create (or lookup) a nested object under parent[key].
/// If the key exists and is an object, return it; otherwise replace the
/// existing value (or add a new key) with a fresh empty object.
[[nodiscard]] inline yyjson_mut_val* ensure_obj(yyjson_mut_doc* doc,
                                                yyjson_mut_val* parent,
                                                std::string_view key) {
    if (!is_obj(parent)) return nullptr;
    auto* existing = obj_getn(parent, key);
    if (is_obj(existing)) return existing;
    if (existing) remove(parent, key);
    auto* new_obj = yyjson_mut_obj(doc);
    auto* k = yyjson_mut_strncpy(doc, key.data(), key.size());
    yyjson_mut_obj_add(parent, k, new_obj);
    return new_obj;
}

// --- key-value putters (upsert semantics) -----------------------------------

inline void put_str(yyjson_mut_doc* doc, yyjson_mut_val* obj,
                    std::string_view key, std::string_view val) {
    if (!is_obj(obj)) return;
    // Remove any existing entry first (yyjson_mut_obj_put can add duplicates
    // if called multiple times without remove in older builds).
    remove(obj, key);
    auto* k = yyjson_mut_strncpy(doc, key.data(), key.size());
    auto* v = yyjson_mut_strncpy(doc, val.data(), val.size());
    yyjson_mut_obj_add(obj, k, v);
}

inline void put_bool(yyjson_mut_doc* doc, yyjson_mut_val* obj,
                     std::string_view key, bool val) {
    if (!is_obj(obj)) return;
    remove(obj, key);
    auto* k = yyjson_mut_strncpy(doc, key.data(), key.size());
    auto* v = val ? yyjson_mut_true(doc) : yyjson_mut_false(doc);
    yyjson_mut_obj_add(obj, k, v);
}

inline void put_sint(yyjson_mut_doc* doc, yyjson_mut_val* obj,
                     std::string_view key, int64_t val) {
    if (!is_obj(obj)) return;
    remove(obj, key);
    auto* k = yyjson_mut_strncpy(doc, key.data(), key.size());
    auto* v = yyjson_mut_sint(doc, val);
    yyjson_mut_obj_add(obj, k, v);
}

// --- truthy coercion (TypeScript-like) --------------------------------------

[[nodiscard]] inline bool truthy(yyjson_val* v) noexcept {
    if (!v) return false;
    if (yyjson_is_true(v)) return true;
    if (yyjson_is_false(v) || yyjson_is_null(v)) return false;
    if (yyjson_is_num(v)) return yyjson_get_sint(v) != 0;
    if (yyjson_is_str(v)) return yyjson_get_len(v) > 0;
    if (yyjson_is_arr(v)) return yyjson_arr_size(v) > 0;
    if (yyjson_is_obj(v)) return yyjson_obj_size(v) > 0;
    return true;
}

[[nodiscard]] inline bool truthy(JsonVal v) noexcept {
    return truthy(v.raw());
}

// --- string array helpers ---------------------------------------------------

/// Collect string elements of a mutable array into a sorted set.
inline void collect_strs(yyjson_mut_val* arr, std::set<std::string>& out) {
    if (!is_arr(arr)) return;
    yyjson_mut_arr_iter it;
    yyjson_mut_arr_iter_init(arr, &it);
    yyjson_mut_val* item;
    while ((item = yyjson_mut_arr_iter_next(&it)) != nullptr) {
        if (is_str(item)) {
            out.emplace(yyjson_mut_get_str(item), yyjson_mut_get_len(item));
        }
    }
}

/// Collect string elements of an immutable array into a sorted set.
inline void collect_strs_immutable(yyjson_val* arr,
                                   std::set<std::string>& out) {
    if (!arr || !yyjson_is_arr(arr)) return;
    yyjson_arr_iter it;
    yyjson_arr_iter_init(arr, &it);
    yyjson_val* item;
    while ((item = yyjson_arr_iter_next(&it)) != nullptr) {
        if (yyjson_is_str(item)) {
            out.emplace(yyjson_get_str(item), yyjson_get_len(item));
        }
    }
}

/// Replace all elements of a mutable array with the given string set.
inline void set_str_array(yyjson_mut_doc* doc, yyjson_mut_val* arr,
                          const std::set<std::string>& values) {
    if (!is_arr(arr)) return;
    yyjson_mut_arr_clear(arr);
    for (const auto& s : values) {
        auto* v = yyjson_mut_strncpy(doc, s.data(), s.size());
        yyjson_mut_arr_append(arr, v);
    }
}

/// Merge unique string elements from a (possibly null) immutable source array
/// into a mutable target array on `parent[key]`.  Creates the target array
/// if it doesn't exist yet.
inline void merge_str_array(yyjson_mut_doc* doc, yyjson_mut_val* parent,
                            std::string_view key,
                            yyjson_val* src_arr_immutable) {
    if (!is_obj(parent)) return;

    std::set<std::string> all;
    auto* existing = obj_getn(parent, key);
    if (is_arr(existing)) collect_strs(existing, all);
    if (src_arr_immutable) collect_strs_immutable(src_arr_immutable, all);

    // Create (or reuse) the target array
    yyjson_mut_val* tgt = is_arr(existing) ? existing : nullptr;
    if (!tgt) {
        if (existing) remove(parent, key);
        tgt = yyjson_mut_arr(doc);
        auto* k = yyjson_mut_strncpy(doc, key.data(), key.size());
        yyjson_mut_obj_add(parent, k, tgt);
    }
    set_str_array(doc, tgt, all);
}

// --- convenience: operate on JsonMutVal& instead of raw pointers ------------

#define CC_MUT_R_(name)                                                        \
    template <typename... Args>                                                \
    inline auto name(JsonMutVal& obj, Args&&... args) noexcept(                \
        noexcept(name(obj.raw(), std::forward<Args>(args)...)))                \
        -> decltype(name(obj.raw(), std::forward<Args>(args)...)) {            \
        return name(obj.raw(), std::forward<Args>(args)...);                   \
    }

CC_MUT_R_(is_obj)
CC_MUT_R_(is_arr)
CC_MUT_R_(is_str)
CC_MUT_R_(is_true)
CC_MUT_R_(is_false)
CC_MUT_R_(is_bool)
CC_MUT_R_(is_sint)
CC_MUT_R_(is_uint)
CC_MUT_R_(is_num)
CC_MUT_R_(is_null)
CC_MUT_R_(get_str)
CC_MUT_R_(get_sint)
CC_MUT_R_(get_uint)
CC_MUT_R_(get_bool)
CC_MUT_R_(arr_size)
CC_MUT_R_(obj_size)
CC_MUT_R_(obj_getn)
CC_MUT_R_(has)
CC_MUT_R_(remove)
#undef CC_MUT_R_

#define CC_MUT_R_DOC_(name)                                                    \
    template <typename... Args>                                                \
    inline auto name(JsonMutDoc& doc, JsonMutVal& obj, Args&&... args)         \
        -> decltype(name(doc.raw(), obj.raw(),                                 \
                        std::forward<Args>(args)...)) {                        \
        return name(doc.raw(), obj.raw(), std::forward<Args>(args)...);        \
    }

CC_MUT_R_DOC_(ensure_obj)
CC_MUT_R_DOC_(put_str)
CC_MUT_R_DOC_(put_bool)
CC_MUT_R_DOC_(put_sint)
CC_MUT_R_DOC_(merge_str_array)
#undef CC_MUT_R_DOC_

}  // namespace mut
}  // namespace

/// Factory: build a ConfigCtx from three (possibly null/empty) input docs.
/// Missing inputs are represented as empty JSON objects.
[[nodiscard]] inline ConfigCtx make_config_ctx(const JsonDoc* global_src,
                                               const JsonDoc* user_src,
                                               const JsonDoc* local_src) {
    ConfigCtx ctx;
    ctx.doc = std::make_unique<JsonMutDoc>();

    // Create a wrapper root with three empty sub-objects.
    JsonMutVal root = ctx.doc->object();
    ctx.doc->set_root(root);

    auto add_empty = [&](std::string_view key) -> JsonMutVal {
        auto obj = ctx.doc->object();
        root.add(key, obj);
        auto* r = mut::raw(root);
        auto* sub = yyjson_mut_obj_getn(r, key.data(), key.size());
        return {sub, ctx.doc->raw()};
    };

    ctx.global = add_empty("globalConfig");
    ctx.user   = add_empty("userSettings");
    ctx.local  = add_empty("localSettings");

    // Overlay each real source on top of the empty sub-object.  We replace
    // the entire sub-object with a deep copy of the source root (when it
    // is an object).
    auto replace_sub = [&](JsonMutVal& slot, JsonVal src_root) {
        if (!src_root.valid() || !src_root.is_obj()) return;
        auto* rroot = mut::raw(root);
        // "globalConfig" / "userSettings" / "localSettings" — derive from
        // the slot pointer by looking it up in the root.
        auto copied = ctx.doc->copy_val(src_root);
        // Find the name of this slot via pointer equality
        const char* keys[] = {"globalConfig", "userSettings", "localSettings"};
        JsonMutVal* slots[] = {&ctx.global, &ctx.user, &ctx.local};
        for (int i = 0; i < 3; ++i) {
            if (slots[i] == &slot) {
                std::string_view key = keys[i];
                yyjson_mut_obj_remove_keyn(rroot, key.data(), key.size());
                auto* k = yyjson_mut_strncpy(ctx.doc->raw(),
                                             key.data(), key.size());
                yyjson_mut_obj_add(rroot, k, copied.raw());
                // Re-fetch the slot pointer
                auto* sub = yyjson_mut_obj_getn(rroot, key.data(), key.size());
                slot = JsonMutVal(sub, ctx.doc->raw());
                return;
            }
        }
    };

    if (global_src) replace_sub(ctx.global, global_src->root());
    if (user_src)   replace_sub(ctx.user,   user_src->root());
    if (local_src)  replace_sub(ctx.local,  local_src->root());

    return ctx;
}

// ============================================================================
// Migration 1: migrateAutoUpdatesToSettings
// ---------------------------------------------------------------------------
// When `globalConfig.autoUpdates` is explicitly false AND the
// `autoUpdatesProtectedForNative` sentinel is not true, the user has
// manually opted out of auto-updates in the old config shape.  We
// translate that opt-out into an environment variable on userSettings
// and delete the now-stale keys from globalConfig.
// ============================================================================

[[nodiscard]] inline bool detect_1(const DetectCtx& c) {
    auto au = c.global.get("autoUpdates");
    if (!au.valid() || !au.is_bool()) return false;
    if (au.as_bool() != false) return false;
    auto pf = c.global.get("autoUpdatesProtectedForNative");
    if (pf.valid() && pf.is_bool() && pf.as_bool()) return false;
    return true;
}

inline void apply_1(ConfigCtx& c) {
    // userSettings.env.DISABLE_AUTOUPDATER = "1"
    auto* env_obj = mut::ensure_obj(*c.doc, c.user, "env");
    mut::put_str(c.doc->raw(), env_obj, "DISABLE_AUTOUPDATER", "1");
    // Clean up the legacy keys
    mut::remove(c.global, "autoUpdates");
    mut::remove(c.global, "autoUpdatesProtectedForNative");
}

// ============================================================================
// Migration 2: migrateBypassPermissionsAcceptedToSettings
// ---------------------------------------------------------------------------
// If the user accepted the "bypass permissions" mode under the old key,
// we migrate the flag to its canonical home in userSettings and drop
// the legacy key.
// ============================================================================

[[nodiscard]] inline bool detect_2(const DetectCtx& c) {
    auto bp = c.global.get("bypassPermissionsModeAccepted");
    if (!bp.valid()) return false;
    return mut::truthy(bp);
}

inline void apply_2(ConfigCtx& c) {
    if (!mut::has(c.user, "skipDangerousModePermissionPrompt")) {
        mut::put_bool(*c.doc, c.user, "skipDangerousModePermissionPrompt",
                      true);
    }
    mut::remove(c.global, "bypassPermissionsModeAccepted");
}

// ============================================================================
// Migration 3: migrateEnableAllProjectMcpServersToSettings
// ---------------------------------------------------------------------------
// Historically MCP server approvals lived in the project-scoped config
// (here represented by keys on globalConfig); they now live in the
// project-local settings file.  We copy the boolean flag and merge +
// dedupe the two string arrays, then drop the source keys.
// ============================================================================

[[nodiscard]] inline bool detect_3(const DetectCtx& c) {
    if (c.global.has("enableAllProjectMcpServers")) return true;
    auto en = c.global.get("enabledMcpjsonServers");
    if (en.valid() && en.is_arr() && en.size() > 0) return true;
    auto dis = c.global.get("disabledMcpjsonServers");
    if (dis.valid() && dis.is_arr() && dis.size() > 0) return true;
    return false;
}

inline void apply_3(ConfigCtx& c) {
    // enableAllProjectMcpServers: scalar copy (only if unset on the target)
    auto* src_flag = mut::obj_getn(c.global, "enableAllProjectMcpServers");
    if (mut::is_bool(src_flag) &&
        !mut::has(c.local, "enableAllProjectMcpServers")) {
        mut::put_bool(*c.doc, c.local, "enableAllProjectMcpServers",
                      mut::get_bool(src_flag));
    }

    // enabledMcpjsonServers: merge + dedupe into localSettings
    // (read from the mutable copy; apply_3 runs before the source key is
    // removed at the end of this function)
    yyjson_val* en_raw = nullptr;
    {
        auto* src = mut::obj_getn(c.global, "enabledMcpjsonServers");
        if (mut::is_arr(src)) {
            // Reborrow as immutable for the collector — yyjson_mut_val and
            // yyjson_val share an identical memory layout so this cast is
            // well-defined for reading.
            en_raw = reinterpret_cast<yyjson_val*>(src);
        }
    }
    mut::merge_str_array(*c.doc, c.local, "enabledMcpjsonServers", en_raw);

    // disabledMcpjsonServers: merge + dedupe into localSettings
    yyjson_val* dis_raw = nullptr;
    {
        auto* src = mut::obj_getn(c.global, "disabledMcpjsonServers");
        if (mut::is_arr(src)) {
            dis_raw = reinterpret_cast<yyjson_val*>(src);
        }
    }
    mut::merge_str_array(*c.doc, c.local, "disabledMcpjsonServers", dis_raw);

    // Remove migrated keys from the source (project-scoped) config
    mut::remove(c.global, "enableAllProjectMcpServers");
    mut::remove(c.global, "enabledMcpjsonServers");
    mut::remove(c.global, "disabledMcpjsonServers");
}

// ============================================================================
// Migration 4: migrateFennecToOpus — ant users only
// ---------------------------------------------------------------------------
// Internal-only model rename.  Rewrites the pinned user model and
// additionally sets fastMode=true for the two fast-track variants.
// ============================================================================

[[nodiscard]] inline bool detect_4(const DetectCtx& c) {
    auto ut = c.global.get("userType");
    if (!ut.valid() || !ut.is_str() || ut.as_str() != "ant") return false;
    auto m = c.user.get("model");
    if (!m.valid() || !m.is_str()) return false;
    auto s = m.as_str();
    return s == "fennec-latest[1m]" || s == "fennec-latest" ||
           s == "fennec-fast-latest" || s == "opus-4-5-fast";
}

inline void apply_4(ConfigCtx& c) {
    auto* raw_model = mut::obj_getn(c.user, "model");
    if (!mut::is_str(raw_model)) return;
    const std::string_view sv = mut::get_str(raw_model);

    std::string new_model;
    bool set_fast = false;
    if (sv == "fennec-latest[1m]") {
        new_model = "opus[1m]";
    } else if (sv == "fennec-latest") {
        new_model = "opus";
    } else if (sv == "fennec-fast-latest" || sv == "opus-4-5-fast") {
        new_model = "opus[1m]";
        set_fast = true;
    }
    if (!new_model.empty()) {
        mut::put_str(*c.doc, c.user, "model", new_model);
    }
    if (set_fast) {
        mut::put_bool(*c.doc, c.user, "fastMode", true);
    }
}

// ============================================================================
// Migration 5: migrateLegacyOpusToCurrent
// ---------------------------------------------------------------------------
// Replaces the explicit legacy Opus 4.0/4.1 model strings with the
// rolling "opus" alias (first-party provider only), records when the
// migration happened for later notification UI.
// ============================================================================

[[nodiscard]] inline bool detect_5(const DetectCtx& c) {
    auto re = c.global.get("legacyModelRemapEnabled");
    if (re.valid() && re.is_bool() && !re.as_bool()) return false;
    auto pr = c.global.get("apiProvider");
    if (pr.valid() && pr.is_str() && pr.as_str() != "firstParty") return false;
    auto m = c.user.get("model");
    if (!m.valid() || !m.is_str()) return false;
    auto s = m.as_str();
    return s == "claude-opus-4-20250514" ||
           s == "claude-opus-4-1-20250805" ||
           s == "claude-opus-4-0" ||
           s == "claude-opus-4-1";
}

inline void apply_5(ConfigCtx& c) {
    mut::put_str(*c.doc, c.user, "model", "opus");
    mut::put_sint(*c.doc, c.global, "legacyOpusMigrationTimestamp",
                  now_epoch_ms());
}

// ============================================================================
// Migration 6: migrateOpusToOpus1m — Max / Team Premium, skip Pro
// ---------------------------------------------------------------------------
// After the Opus / Opus 1M merge, non-Pro users on the "opus" alias are
// migrated to "opus[1m]".  When the server-side default already equals
// opus[1m] (signalled by globalConfig.opus1mIsDefault), the model pin
// is simply cleared instead.
// ============================================================================

[[nodiscard]] inline bool detect_6(const DetectCtx& c) {
    auto me = c.global.get("opus1mMergeEnabled");
    if (!me.valid() || !me.is_bool() || !me.as_bool()) return false;
    auto m = c.user.get("model");
    if (!m.valid() || !m.is_str() || m.as_str() != "opus") return false;
    auto tier = c.global.get("subscriptionTier");
    if (tier.valid() && tier.is_str() && tier.as_str() == "pro") return false;
    return true;
}

inline void apply_6(ConfigCtx& c) {
    auto* def = mut::obj_getn(c.global, "opus1mIsDefault");
    const bool is_default = mut::is_true(def);
    if (is_default) {
        // Clear the model pin — the server default is already opus[1m]
        mut::remove(c.user, "model");
    } else {
        mut::put_str(*c.doc, c.user, "model", "opus[1m]");
    }
}

// ============================================================================
// Migration 7: migrateReplBridgeEnabledToRemoteControlAtStartup
// ---------------------------------------------------------------------------
// Straight key rename with boolean coercion.  Skips when the new key
// already exists (user may have set it explicitly).
// ============================================================================

[[nodiscard]] inline bool detect_7(const DetectCtx& c) {
    auto old = c.global.get("replBridgeEnabled");
    if (!old.valid()) return false;
    auto nw = c.global.get("remoteControlAtStartup");
    if (nw.valid()) return false;
    return true;
}

inline void apply_7(ConfigCtx& c) {
    auto* old = mut::obj_getn(c.global, "replBridgeEnabled");
    bool value = false;
    if (old) {
        if (mut::is_true(old)) {
            value = true;
        } else if (mut::is_str(old)) {
            std::string_view sv = mut::get_str(old);
            value = !sv.empty() && sv != "false" && sv != "0";
        } else if (mut::is_sint(old)) {
            value = mut::get_sint(old) != 0;
        } else if (mut::is_uint(old)) {
            value = static_cast<int64_t>(mut::get_uint(old)) != 0;
        }
    }
    mut::put_bool(*c.doc, c.global, "remoteControlAtStartup", value);
    mut::remove(c.global, "replBridgeEnabled");
}

// ============================================================================
// Migration 8: migrateSonnet1mToSonnet45
// ---------------------------------------------------------------------------
// After the "sonnet" alias switched to Sonnet 4.6, users explicitly
// pinned to the old "sonnet[1m]" must be pinned to the explicit
// Sonnet 4.5 version string.  The migration is recorded as complete
// even for non-matching users (so we never re-check it again).
// ============================================================================

[[nodiscard]] inline bool detect_8(const DetectCtx& c) {
    auto done = c.global.get("sonnet1m45MigrationComplete");
    if (done.valid() && done.is_bool() && done.as_bool()) return false;
    return true;
}

inline void apply_8(ConfigCtx& c) {
    auto* m = mut::obj_getn(c.user, "model");
    if (mut::is_str(m) && mut::get_str(m) == "sonnet[1m]") {
        mut::put_str(*c.doc, c.user, "model", "sonnet-4-5-20250929[1m]");
    }
    mut::put_bool(*c.doc, c.global, "sonnet1m45MigrationComplete", true);
}

// ============================================================================
// Migration 9: migrateSonnet45ToSonnet46 — firstParty only
// ---------------------------------------------------------------------------
// Users pinned to the explicit Sonnet 4.5 model strings are moved to
// the rolling "sonnet" / "sonnet[1m]" aliases (which now resolve to
// Sonnet 4.6).  A timestamp is recorded for later notification UI when
// the user has already used the app (numStartups > 1).
// ============================================================================

[[nodiscard]] inline bool detect_9(const DetectCtx& c) {
    auto pr = c.global.get("apiProvider");
    if (pr.valid() && pr.is_str() && !pr.as_str().empty() &&
        pr.as_str() != "firstParty") return false;
    auto m = c.user.get("model");
    if (!m.valid() || !m.is_str()) return false;
    auto s = m.as_str();
    return s == "claude-sonnet-4-5-20250929" ||
           s == "sonnet-4-5-20250929" ||
           s == "claude-sonnet-4-5-20250929[1m]" ||
           s == "sonnet-4-5-20250929[1m]";
}

inline void apply_9(ConfigCtx& c) {
    auto* m = mut::obj_getn(c.user, "model");
    if (!mut::is_str(m)) return;
    const std::string_view sv = mut::get_str(m);
    if (sv == "claude-sonnet-4-5-20250929" || sv == "sonnet-4-5-20250929") {
        mut::put_str(*c.doc, c.user, "model", "sonnet");
    } else if (sv == "claude-sonnet-4-5-20250929[1m]" ||
               sv == "sonnet-4-5-20250929[1m]") {
        mut::put_str(*c.doc, c.user, "model", "sonnet[1m]");
    }
    auto* ns = mut::obj_getn(c.global, "numStartups");
    if (mut::is_sint(ns) && mut::get_sint(ns) > 1) {
        mut::put_sint(*c.doc, c.global, "sonnet45To46MigrationTimestamp",
                      now_epoch_ms());
    }
}

// ============================================================================
// Migration 10: resetAutoModeOptInForDefaultOffer
// ---------------------------------------------------------------------------
// Re-prompt users about the new default-mode dialog by clearing their
// existing "skip the auto-permission prompt" preference — but only
// when they have auto-mode enabled without defaultMode being "auto".
// Always marks the migration complete afterwards.
// ============================================================================

[[nodiscard]] inline bool detect_10(const DetectCtx& c) {
    auto done = c.global.get("hasResetAutoModeOptInForDefaultOffer");
    if (done.valid() && done.is_bool() && done.as_bool()) return false;
    auto feat = c.global.get("transcriptClassifierEnabled");
    if (!feat.valid() || !feat.is_bool() || !feat.as_bool()) return false;
    return true;
}

inline void apply_10(ConfigCtx& c) {
    auto* ams = mut::obj_getn(c.global, "autoModeState");
    const bool auto_enabled = mut::is_str(ams)
                              && mut::get_str(ams) == "enabled";

    auto* skip = mut::obj_getn(c.user, "skipAutoPermissionPrompt");
    const bool skip_true = mut::is_true(skip);

    // userSettings.permissions.defaultMode == "auto" ?
    bool default_auto = false;
    auto* perms = mut::obj_getn(c.user, "permissions");
    if (mut::is_obj(perms)) {
        auto* dm = yyjson_mut_obj_getn(perms, "defaultMode", 11);
        if (mut::is_str(dm)) {
            default_auto = (mut::get_str(dm) == "auto");
        }
    }

    if (auto_enabled && skip_true && !default_auto) {
        mut::remove(c.user, "skipAutoPermissionPrompt");
    }
    mut::put_bool(*c.doc, c.global, "hasResetAutoModeOptInForDefaultOffer",
                  true);
}

// ============================================================================
// Migration 11: resetProToOpusDefault
// ---------------------------------------------------------------------------
// Marks the user as migrated to the Opus-default lineup.  First-party
// Pro users without a custom model pin also get a notification
// timestamp.  Other users (BYOC, free, etc.) just get the completion
// flag so we never check again.
// ============================================================================

[[nodiscard]] inline bool detect_11(const DetectCtx& c) {
    auto done = c.global.get("opusProMigrationComplete");
    if (done.valid() && done.is_bool() && done.as_bool()) return false;
    return true;
}

inline void apply_11(ConfigCtx& c) {
    auto* pr = mut::obj_getn(c.global, "apiProvider");
    const bool is_first_party = !(mut::is_str(pr) &&
                                  !mut::get_str(pr).empty() &&
                                  mut::get_str(pr) != "firstParty");

    auto* tier = mut::obj_getn(c.global, "subscriptionTier");
    const bool is_pro = mut::is_str(tier)
                        && mut::get_str(tier) == "pro";

    bool has_custom_model = false;
    auto* mdl = mut::obj_getn(c.user, "model");
    if (mut::is_str(mdl) && yyjson_mut_get_len(mdl) > 0) {
        has_custom_model = true;
    }

    if (is_first_party && is_pro && !has_custom_model) {
        mut::put_sint(*c.doc, c.global, "opusProMigrationTimestamp",
                      now_epoch_ms());
    }
    mut::put_bool(*c.doc, c.global, "opusProMigrationComplete", true);
}

// ============================================================================
// Registry: get_all_migrations()
// ============================================================================

/// Returns all 11 migration entries in dependency order.  The returned
/// reference points to a function-local static; pointers into the
/// vector remain valid for the lifetime of the program.
[[nodiscard]] auto get_all_migrations()
    -> const std::vector<MigrationEntry>& {
    static const std::vector<MigrationEntry> kAll = {
        {
            "migrateAutoUpdatesToSettings",
            "Move auto-update config from legacy location to settings",
            1, detect_1, apply_1,
        },
        {
            "migrateBypassPermissionsAcceptedToSettings",
            "Permission bypass flag migration to user settings",
            2, detect_2, apply_2,
        },
        {
            "migrateEnableAllProjectMcpServersToSettings",
            "MCP server approval fields to project-local settings",
            3, detect_3, apply_3,
        },
        {
            "migrateFennecToOpus",
            "Rename fennec model aliases to opus (ant users only)",
            4, detect_4, apply_4,
        },
        {
            "migrateLegacyOpusToCurrent",
            "Legacy opus 4.0/4.1 model strings to the 'opus' alias",
            5, detect_5, apply_5,
        },
        {
            "migrateOpusToOpus1m",
            "Upgrade opus -> opus[1m] for Max/Team Premium",
            6, detect_6, apply_6,
        },
        {
            "migrateReplBridgeEnabledToRemoteControlAtStartup",
            "Rename replBridgeEnabled -> remoteControlAtStartup",
            7, detect_7, apply_7,
        },
        {
            "migrateSonnet1mToSonnet45",
            "Pin sonnet[1m] to the explicit Sonnet 4.5 version string",
            8, detect_8, apply_8,
        },
        {
            "migrateSonnet45ToSonnet46",
            "Rewrite Sonnet 4.5 strings -> 'sonnet' (now Sonnet 4.6) alias",
            9, detect_9, apply_9,
        },
        {
            "resetAutoModeOptInForDefaultOffer",
            "Clear skipAutoPermissionPrompt to show the new default dialog",
            10, detect_10, apply_10,
        },
        {
            "resetProToOpusDefault",
            "Mark Pro users migrated to Opus default + notify when applicable",
            11, detect_11, apply_11,
        },
    };
    return kAll;
}

// ============================================================================
// Top-level orchestration helpers
// ============================================================================

/// Collect migrations whose detect() returned true on the given snapshot.
[[nodiscard]] std::vector<const MigrationEntry*> detect_pending(
    const DetectCtx& ctx) {
    std::vector<const MigrationEntry*> out;
    for (const auto& e : get_all_migrations()) {
        if (e.detect(ctx)) out.push_back(&e);
    }
    return out;
}

/// Apply the supplied list of migrations in order.  All mutations happen
/// inside `ctx.doc`; the caller serialises the three sub-objects after.
inline void apply_migrations(ConfigCtx& ctx,
                             const std::vector<const MigrationEntry*>& list) {
    for (const auto* e : list) {
        e->apply(ctx);
    }
}

/// High-level convenience: run detect + apply on a ConfigCtx built from
/// the three input docs.  Returns the ordered list of migration ids
/// actually executed.
///
/// @note The ConfigCtx is discarded after the call; callers that need to
///       serialise changes should use detect_pending() + make_config_ctx()
///       + apply_migrations() directly instead.
[[nodiscard]] std::vector<std::string> run_all_migrations(
    const JsonDoc* global_src,
    const JsonDoc* user_src,
    const JsonDoc* local_src) {
    DetectCtx detect_view;
    if (global_src) detect_view.global = global_src->root();
    if (user_src)   detect_view.user   = user_src->root();
    if (local_src)  detect_view.local  = local_src->root();

    auto pending = detect_pending(detect_view);
    auto ctx = make_config_ctx(global_src, user_src, local_src);
    apply_migrations(ctx, pending);

    std::vector<std::string> ids;
    ids.reserve(pending.size());
    for (const auto* e : pending) ids.push_back(e->id);
    return ids;
}

}  // namespace cc::migrations::concrete
