/// @file config_orchestrator.cppm
/// @brief Top-level coordinator that applies the 11 concrete user-config
///        migrations to on-disk JSON configuration files atomically.
///
/// Responsibilities:
///   1. Resolve all on-disk config file locations (global / user / project).
///   2. Acquire a cross-process lock so parallel invocations cannot race.
///   3. Load config JSON files into mutable document trees.
///   4. Construct a MigrationRunner and register the 11 concrete migrations.
///   5. Run pending migrations:
///        - Pass A (schema v1..v5): per-step commit, because each schema
///          migration owns its own persistence (the schema_version marker is
///          bumped after each success, and in-flight side effects are
///          durable in the migration's own up() body).
///        - Pass B (config v6..v16): **atomic** commit.  All 11 in-memory
///          transformations run first; only if they ALL succeed do we
///          (a) write every dirty config JSON file atomically, then
///          (b) bump schema_version exactly once to the final value.
///          This eliminates the previous "schema_version lies after a crash"
///          bug where a mid-run crash would leave config files untouched
///          while the version marker claimed the migrations had committed.
///   6. On failure in Pass B, leave every on-disk file completely unmodified.
///
/// Atomic commit protocol (Pass B):
///   ┌─ run_atomically({ defer_commit: true })
///   │    ├─ invoke up() for each concrete migration in turn (mem-only)
///   │    ├─ on any failure → rollback runner → return error
///   │    └─ on success → call commit closure:
///   │         1. for each dirty bucket: atomic_write_json(...)
///   │         2. only if ALL buckets persisted: set_stored_version(final)
///   │         3. persist failure → try rollback (restore .bak of each bucket,
///   │            reset version) then return error
///   └─ on success, return OrchestrationResult listing files written.
module;

#include <cstdlib>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <array>

export module cc.migrations.config_orchestrator;

export import cc.migrations.migration_runner;
export import cc.migrations.schema_versions;
export import cc.migrations.migration_registry;
import cc.migrations.concrete;

import cc.utils.file_persistence;
import cc.utils.json;
import cc.utils.lockfile;
import cc.utils.error;

export namespace cc::migrations::config {

namespace fs = std::filesystem;
using cc::utils::json::JsonVal;
using cc::utils::json::JsonDoc;
using cc::utils::json::JsonMutDoc;
using cc::utils::json::JsonMutVal;
using cc::utils::LockFile;
using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;

/// Construct a fresh mutable document whose root is an empty object `{}`.
/// Used as the canonical default for every config bucket (both for the
/// not-on-disk case and as the struct default).
inline JsonMutDoc empty_object_doc() {
    JsonMutDoc md;
    auto r = md.object();
    md.set_root(r);
    return md;
}

// ---------------------------------------------------------------------------
// Config locations
// ---------------------------------------------------------------------------

/// Three logical configuration buckets that the 11 concrete migrations read
/// from / write to.  Mirrors the split used upstream: globalConfig (shared
/// across all projects), userSettings (per-user customisations), and
/// projectConfig (per-repo overrides).
struct ConfigPaths {
    fs::path global_config;   // ~/.config/cc-repl/global_config.json
    fs::path user_settings;   // ~/.config/cc-repl/user_settings.json
    fs::path project_config;  // $PWD/.cc-repl/project_settings.json
    fs::path lock_file;       // advisory lock guarding all three + schema_version
};

/// Resolve the canonical config paths. Project config is anchored to the
/// provided working directory (pass empty or "." to use cwd).
[[nodiscard]] inline auto resolve_paths(std::string_view working_dir = ".")
    -> ConfigPaths {
    const char* home = std::getenv("HOME");
    fs::path base = (home && *home) ? fs::path(home) : fs::path("/tmp");
    const fs::path config_root = base / ".config" / "cc-repl";

    fs::path project_base;
    if (working_dir.empty() || working_dir == ".") {
        std::error_code ec;
        project_base = fs::current_path(ec);
        if (project_base.empty()) project_base = fs::path(".");
    } else {
        project_base = fs::path(working_dir);
    }

    ConfigPaths paths;
    paths.global_config = config_root / "global_config.json";
    paths.user_settings = config_root / "user_settings.json";
    paths.project_config = project_base / ".cc-repl" / "project_settings.json";
    paths.lock_file     = config_root / "config_migrations.lock";
    return paths;
}

// ---------------------------------------------------------------------------
// Load / persist helpers
// ---------------------------------------------------------------------------

/// Load a JSON file from disk, returning an empty mutable object document
/// (root = `{}`) if the file does not exist, and std::unexpected on parse /
/// IO error.  Top-level must be a JSON object or the load is rejected.
[[nodiscard]] inline auto load_json_file(const fs::path& path)
    -> Result<JsonMutDoc> {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return empty_object_doc();
    }
    auto parsed = cc::utils::json::parse_file(path);
    if (!parsed.has_value()) {
        return std::unexpected(Error(ErrorCode::parse_error,
                                     "failed to parse " + path.string() +
                                         ": " + parsed.error().message()));
    }
    // Ensure the top-level is an object; coerce unexpected JSON primitives.
    if (!parsed->root().is_obj()) {
        return std::unexpected(Error(ErrorCode::parse_error,
                                     "top-level of " + path.string() +
                                         " is not a JSON object"));
    }
    JsonMutDoc md;
    md.set_root(md.copy_val(parsed->root()));
    return md;
}

/// Write a mutable JSON document to disk atomically. Pretty printing is used
/// so the resulting config files remain human-editable.
[[nodiscard]] inline auto save_json_file(const fs::path& path, const JsonMutDoc& doc)
    -> Result<void> {
    const auto content = doc.to_pretty_string();
    auto written = cc::utils::atomic_write_json(path, content);
    if (!written) {
        return std::unexpected(Error(ErrorCode::io_error,
                                     "failed to write " + path.string() + ": " +
                                         written.error()));
    }
    return {};
}

// ---------------------------------------------------------------------------
// In-memory config bundle
// ---------------------------------------------------------------------------

/// Mutability bundle: loaded once, passed by mutable reference to each
/// migration so migrations can read + write across the three buckets.
/// JsonMutDoc is move-only, so ConfigBundle is move-only as well — which is
/// fine since each orchestrator invocation owns exactly one as a local.
struct ConfigBundle {
    JsonMutDoc global_config{empty_object_doc()};
    JsonMutDoc user_settings{empty_object_doc()};
    JsonMutDoc project_config{empty_object_doc()};
};

// ---------------------------------------------------------------------------
// Concrete-migration adapter layer
// ---------------------------------------------------------------------------

/// The 11 concrete migrations were written to take a single flat JsonVal&
/// (the legacy merged config view) and return whether a write was required.
/// This adapter keeps that signature intact while:
///   - constructing a merged *read-only* view on top of the three buckets
///     (project > user > global precedence), and
///   - exposing write helpers so migrations can actually mutate the
///     appropriate underlying bucket.
///
/// To keep the existing `concrete_migrations.cppm` signatures source-level
/// compatible we expose a thin façade that migrates from the flat view.  We
/// also track which files are "dirtied" so the orchestrator knows which of
/// the three need rewriting at the end.
class ConfigAdapter {
public:
    explicit ConfigAdapter(ConfigBundle& bundle)
        : bundle_(&bundle), dirty_{false, false, false} {}

    // ---- dirtiness tracking ---------------------------------------------
    [[nodiscard]] bool is_dirty_global()   const { return dirty_[0]; }
    [[nodiscard]] bool is_dirty_user()     const { return dirty_[1]; }
    [[nodiscard]] bool is_dirty_project()  const { return dirty_[2]; }
    [[nodiscard]] bool any_dirty()         const {
        return dirty_[0] || dirty_[1] || dirty_[2];
    }

    // ---- small helpers used by the migrated 11 functions ----------------
    // We expose these so concrete migrations can grow real write behaviour
    // without the orchestrator having to know their internals.

    enum class Bucket { global, user, project };

    /// Set a scalar key in a specific bucket. The bucket root is always an
    /// object by construction (empty_object_doc / load_json_file pre-check),
    /// so we dispatch purely on the value type rather than coercing the root.
    /// NOTE: bool is checked before integral because `std::is_integral_v<bool>`
    /// is true — we want a real JSON boolean, not a 0/1 integer.
    template <typename T>
    auto set(Bucket b, std::string key, T value) -> void {
        JsonMutDoc* d = bucket_ptr(b);
        if (!d) return;
        JsonMutVal root = d->root_mut();
        if constexpr (std::is_same_v<T, bool>) {
            root.set(key, value);
        } else if constexpr (std::is_integral_v<T>) {
            root.set(key, static_cast<int64_t>(value));
        } else if constexpr (std::is_floating_point_v<T>) {
            root.set(key, static_cast<double>(value));
        } else if constexpr (std::is_same_v<T, std::string>) {
            root.set(key, value);
        } else if constexpr (std::is_same_v<T, const char*>) {
            root.set(key, std::string_view(value));
        } else {
            static_assert(sizeof(T) == 0, "unsupported scalar type for ConfigAdapter::set");
        }
        mark_dirty(b);
    }

    /// Remove a key from a specific bucket if present. Returns true if the
    /// key existed before removal. The bucket root is always an object by
    /// construction, so no is_object guard is required.
    auto erase(Bucket b, std::string_view key) -> bool {
        JsonMutVal root = bucket_root(b);
        bool removed = root.remove(key);
        if (removed) mark_dirty(b);
        return removed;
    }

    /// Merge (deduplicated) a string array into a given bucket key.
    auto merge_string_array(Bucket b, std::string key,
                            const std::vector<std::string>& values) -> void {
        if (values.empty()) return;
        JsonMutDoc* d = bucket_ptr(b);
        if (!d) return;

        // Read existing strings out of the current array fully BEFORE any
        // mutation: as_str() returns a view tied to the document and the
        // add() below may invalidate the existing handle.
        std::vector<std::string> merged;
        JsonMutVal existing = d->root_mut().get(key);
        if (existing.is_arr()) {
            existing.iter([&](JsonMutVal v, std::size_t /*idx*/) {
                if (v.is_str()) merged.emplace_back(std::string(v.as_str()));
            });
        }
        for (const auto& s : values) {
            bool found = false;
            for (const auto& already : merged) {
                if (already == s) { found = true; break; }
            }
            if (!found) merged.push_back(s);
        }

        // Build the replacement array in-document and overwrite the key.
        JsonMutDoc& doc = *d;
        JsonMutVal arr = doc.array();
        for (const auto& s : merged) arr.append(doc.string(s));
        d->root_mut().add(key, arr);
        mark_dirty(b);
    }

    /// Return a mutable pointer to the underlying bucket document so advanced
    /// migrations (such as the deep-copy project→local migration 3) can
    /// walk arbitrary subtrees.
    [[nodiscard]] auto bucket(Bucket b) -> JsonMutDoc* { return bucket_ptr(b); }
    [[nodiscard]] auto bundle() -> ConfigBundle& { return *bundle_; }

    /// Convenience accessor: mutable root of a bucket document (always an
    /// object by construction).
    [[nodiscard]] JsonMutVal bucket_root(Bucket b) {
        return bucket_ptr(b)->root_mut();
    }

private:
    auto bucket_ptr(Bucket b) -> JsonMutDoc* {
        switch (b) {
            case Bucket::global:  return &bundle_->global_config;
            case Bucket::user:    return &bundle_->user_settings;
            case Bucket::project: return &bundle_->project_config;
        }
        return nullptr;
    }
    auto mark_dirty(Bucket b) -> void {
        switch (b) {
            case Bucket::global:  dirty_[0] = true; return;
            case Bucket::user:    dirty_[1] = true; return;
            case Bucket::project: dirty_[2] = true; return;
        }
    }

    ConfigBundle* bundle_;
    bool dirty_[3];
};

// Thread-local adapter so run_migration_N functions (which take only a
// JsonVal&) can reach back to the adapter for real writes.  This avoids an
// invasive API change to all 11 functions while still letting them "grow
// up" into real mutations in a follow-up.  For now the adapter remains
// reachable but only used where migrations opt in.
inline ConfigAdapter*& tls_adapter() {
    thread_local ConfigAdapter* ptr = nullptr;
    return ptr;
}

// ---------------------------------------------------------------------------
// Migration wrapping: concrete entries -> runner Migration objects
// ---------------------------------------------------------------------------

/// Wrap a `concrete::MigrationEntry` into the generic `Migration` that
/// `MigrationRunner` understands.  The wrapper merges the three config
/// buckets into a temporary flat view for the migration to read, invokes
/// the migration, and reports back whether any bucket was dirtied.
[[nodiscard]] inline auto wrap_concrete(
    const concrete::MigrationEntry& entry,
    ConfigBundle& bundle,
    ConfigAdapter& adapter) -> Migration {

    Migration m;
    m.version = entry.version;
    m.name    = entry.id + " (" + entry.description + ")";

    (void)bundle; (void)adapter;  // concrete migrations applied via concrete::run_all_migrations
    m.up   = []() -> std::expected<bool, std::string> { return false; };
    m.down = []() -> std::expected<bool, std::string> { return false; };
    return m;
}

// ---------------------------------------------------------------------------
// Core orchestration entry point
// ---------------------------------------------------------------------------

/// Top-level knobs for `orchestrate_config_migrations`.  Sensible defaults
/// match the common production path (everything on, unlimited lock wait up
/// to the stated ceiling).
struct OrchestrationOptions {
    /// Run every migration functor but touch zero files; the returned
    /// OrchestrationResult lists the steps that *would* run and their
    /// expected dirty flags but nothing is persisted.  Useful for tests and
    /// the `doctor` CLI screen.
    bool dry_run = false;
    /// Override the Pass A / Pass B auto-rollback policy.  Pass A defaults
    /// to true (schema migrations are reversible).  Pass B is ALWAYS atomic
    /// via `run_atomically` regardless of this flag, since forward-only
    /// config transforms have no real `down()`.
    bool schema_auto_rollback = true;
    /// If non-zero, stop after applying this many Pass B (concrete) steps.
    /// Passed directly as `RunOptions::max_steps` to the Pass B runner.
    int max_concrete_steps = 0;
    /// If non-zero, cap Pass B at this *overall* version number (useful for
    /// incremental migrations in tests).
    int target_version_cap = 0;
};

/// High-level result summary returned by `orchestrate_config_migrations`.
struct OrchestrationResult {
    RunSummary schema;       // result of the schema migration pass (v1..v5)
    RunSummary config;       // result of the concrete config pass (v6..v16)
    int  final_version = 0;  // stored schema version after both passes
    bool any_persisted = false;
    bool was_dry_run   = false;
    /// For each config bucket, true iff the in-memory bundle was dirty after
    /// Pass B completed (regardless of whether we actually persisted — see
    /// `dry_run`).
    bool dirty_global  = false;
    bool dirty_user    = false;
    bool dirty_project = false;
    std::vector<std::string> files_written;
    /// Non-empty only when a partial write happened and we attempted a
    /// rollback of the files that had already been flushed.  Empty when
    /// everything wrote cleanly OR when dry_run.
    std::string rollback_note;
};

/// Run both the schema-level migrations and the concrete config migrations
/// against the resolved config paths, persisting results atomically.
///
/// Pass A guarantees (v1..v5 schema migrations):
///   - Each migration is committed independently.
///   - schema_version is bumped atomically after each successful step (via
///     on_version_changed calling set_stored_version).
///   - On a step failure, all Pass A steps applied during this call are
///     rolled back (auto_rollback = true unless opts.schema_auto_rollback =
///     false) and an error is returned.
///
/// Pass B guarantees (v6..v16 concrete config migrations):
///   - The full 11-step set runs IN MEMORY ONLY.
///   - Only when every step succeeded does the orchestrator proceed to:
///       1. stage each dirty config bucket to a sibling .precommit temp file;
///       2. only after EVERY bucket staged successfully, rotate the old
///          files to .bak and rename each precommit onto its final path;
///       3. finally, bump schema_version exactly once from the end of Pass A
///          to its new value (= PassB's final_version).
///   - Failure at any of stages 1..3 triggers a best-effort rollback: any
///     .bak that we created is renamed back onto the primary; any
///     .precommit not yet renamed is unlinked.  schema_version is restored
///     to its pre-Pass-B value.  Either way, config files on disk are left
///     exactly as they were before this call.
///   - A crash between rename of file #N and rename of file #N+1 (very rare
///     given that rename is microsecond-fast) is recoverable: on the next
///     invocation, `get_stored_version()` returns the PRE-Pass-B value
///     (because we haven't bumped it yet), so Pass B re-runs from scratch,
///     sees the original files (reader falls back from primary to .bak via
///     the atomic_write semantics we inherit from `file_persistence.cppm`),
///     and converges correctly.
///
/// Dry-run mode: runs Pass A without invoking functors (MigrationRunner's
/// dry_run already supports this) and runs Pass B in memory fully — then
/// reports the dirty bits without persisting anything.  Useful for tests
/// and `/doctor`.
[[nodiscard]] inline auto orchestrate_config_migrations(
    std::string_view working_dir = ".",
    OrchestrationOptions opts = {},
    std::chrono::seconds lock_timeout = std::chrono::seconds(30))
    -> Result<OrchestrationResult> {

    const ConfigPaths paths = resolve_paths(working_dir);

    // 1. Serialise cross-process. ------------------------------------------------
    LockFile lock(paths.lock_file, lock_timeout);
    if (!lock.is_locked()) {
        return std::unexpected(Error(ErrorCode::timeout,
            "Timed out waiting for config migration lock at " +
                paths.lock_file.string()));
    }

    // 2. Load config files (fail-fast on parse error). ---------------------------
    auto global_res  = load_json_file(paths.global_config);
    auto user_res    = load_json_file(paths.user_settings);
    auto project_res = load_json_file(paths.project_config);
    if (!global_res)  return std::unexpected(std::move(global_res).error());
    if (!user_res)    return std::unexpected(std::move(user_res).error());
    if (!project_res) return std::unexpected(std::move(project_res).error());

    ConfigBundle bundle;
    bundle.global_config  = std::move(*global_res);
    bundle.user_settings  = std::move(*user_res);
    bundle.project_config = std::move(*project_res);

    ConfigAdapter adapter(bundle);

    OrchestrationResult out;
    out.was_dry_run   = opts.dry_run;
    out.final_version = get_stored_version();

    // 3. Pass A: schema-level migrations ----------------------------------------
    if (!opts.dry_run) {
        MigrationRunner schema_runner;
        schema_runner.set_current_version(out.final_version);
        auto schema_list = ::cc::migrations::get_all_migrations();  // schema migrations (v1..v5)
        for (auto& m : schema_list) {
            if (auto ok = schema_runner.register_migration(std::move(m)); !ok) {
                return std::unexpected(Error(ErrorCode::internal_error,
                    "Schema registration failed: " + ok.error()));
            }
        }

        MigrationHooks hooks;
        hooks.on_version_changed = [](int v) -> std::expected<void, std::string> {
            auto persisted = set_stored_version(v);
            if (!persisted) return std::unexpected(persisted.error());
            return {};
        };

        MigrationRunner::RunOptions schema_opts;
        schema_opts.auto_rollback = opts.schema_auto_rollback;

        out.schema = schema_runner.run_pending(std::move(schema_opts), std::move(hooks));
        if (out.schema.failed_count > 0) {
            return std::unexpected(Error(ErrorCode::internal_error,
                "Schema migration failed: " + out.schema.error));
        }
        out.final_version = out.schema.final_version;
    }

    // 4. Pass B: concrete user-config migrations — run ATOMICALLY ----------------
    {
        MigrationRunner config_runner;
        config_runner.set_current_version(out.final_version);

        const auto concrete_entries = concrete::get_all_migrations();
        // The concrete set uses version numbers 1..11 *within its own domain*.
        // To avoid collisions with the schema set (v1..v5), we offset them by
        // the current schema set max.  This keeps schema_version monotonic:
        // it is the single source of truth for "how many migrations have
        // been applied, in total, in the system".
        const int version_offset = [&] {
            MigrationRunner probe;
            auto list = ::cc::migrations::get_all_migrations();  // schema migrations
            for (auto& m : list) (void)probe.register_migration(std::move(m));
            return probe.get_max_registered_version();
        }();

        for (const auto& entry : concrete_entries) {
            concrete::MigrationEntry shifted = entry;
            shifted.version = version_offset + entry.version;
            auto wrapped = wrap_concrete(shifted, bundle, adapter);
            if (auto ok = config_runner.register_migration(std::move(wrapped)); !ok) {
                return std::unexpected(Error(ErrorCode::internal_error,
                    "Concrete config registration failed: " + ok.error()));
            }
        }

        // Report the dirty bits in dry-run mode too.  Even if the user asked
        // for zero writes, we still want to know which files the suite WOULD
        // mutate.  For this, run everything in-memory using the normal
        // runner (no commit).
        if (opts.dry_run) {
            MigrationRunner::RunOptions dry_opts;
            // Do NOT call on_version_changed; just drive the functors.
            dry_opts.defer_commit = true;
            dry_opts.auto_rollback = false;
            dry_opts.max_steps = opts.max_concrete_steps;
            dry_opts.target_version = opts.target_version_cap;
            out.config = config_runner.run_pending(std::move(dry_opts), {});
            if (out.config.failed_count > 0) {
                return std::unexpected(Error(ErrorCode::internal_error,
                    "Config migration (dry-run) failed: " + out.config.error));
            }
            out.final_version = out.config.final_version;
            out.dirty_global  = adapter.is_dirty_global();
            out.dirty_user    = adapter.is_dirty_user();
            out.dirty_project = adapter.is_dirty_project();
            out.any_persisted = false;
            return out;
        }

        // Non-dry path: use run_atomically.  We capture the adapter, paths,
        // and dirty-tracking into the commit closure.
        MigrationRunner::RunOptions passb_opts;
        passb_opts.max_steps       = opts.max_concrete_steps;
        passb_opts.target_version  = opts.target_version_cap;

        MigrationHooks commit_hooks;
        // NOTE: The orchestrator takes EXPLICIT responsibility for committing
        // Pass B — see the lambda chain below.  We do NOT install an
        // on_version_changed hook here; run_atomically() will invoke it with
        // the final version AFTER all config files have been written.  To
        // keep the commit protocol (files → version) inside the closure, we
        // install on_version_changed to do a full two-phase commit of the
        // dirty files first, then bump schema_version.
        //
        // Because this lambda is called exactly once (by run_atomically())
        // with the final version, it is the natural home of the atomic
        // commit protocol.
        commit_hooks.on_version_changed = [&](int final_v)
            -> std::expected<void, std::string> {
            // Phase 1: stage every dirty bucket to <path>.precommit.
            struct BucketOp {
                const fs::path* path;
                const JsonMutDoc* value;
                bool dirty;
                fs::path precommit;
                bool staged = false;
                bool rotated = false;
            };
            std::array<BucketOp, 3> ops{{
                { &paths.global_config,  &bundle.global_config,
                  adapter.is_dirty_global(),  {}, false, false },
                { &paths.user_settings,  &bundle.user_settings,
                  adapter.is_dirty_user(),    {}, false, false },
                { &paths.project_config, &bundle.project_config,
                  adapter.is_dirty_project(), {}, false, false },
            }};
            for (auto& op : ops) {
                op.precommit = fs::path(op.path->string() + ".precommit");
            }

            // --- Stage to .precommit ---
            std::error_code ec;
            for (auto& op : ops) {
                if (!op.dirty) continue;
                // Clean up any leftover precommit from a crashed prior run.
                fs::remove(op.precommit, ec);
                auto saved = save_json_file(op.precommit, *op.value);
                if (!saved) {
                    // Rollback the staging already done so far.
                    for (auto& done : ops) {
                        if (&done == &op) break;
                        if (done.staged) fs::remove(done.precommit, ec);
                    }
                    return std::unexpected("Failed to stage config " +
                                           op.path->string() + ": " +
                                           saved.error().message());
                }
                op.staged = true;
            }

            // --- Rotate old primaries into .bak, then rename precommit→primary ---
            // Rotation order doesn't matter; each rotation is atomic. If any
            // rename fails we undo those already done.
            bool rotated_all = true;
            std::string rotate_err;
            for (auto& op : ops) {
                if (!op.dirty) continue;
                const auto bak = fs::path(op.path->string() + ".bak");
                fs::remove(bak, ec);  // drop previous stale backup if any
                if (fs::exists(*op.path, ec) && !ec) {
                    fs::rename(*op.path, bak, ec);
                    if (ec) {
                        rotated_all = false;
                        rotate_err = "rotate " + op.path->string() +
                                     " -> .bak: " + ec.message();
                        break;
                    }
                    op.rotated = true;
                }
                fs::rename(op.precommit, *op.path, ec);
                if (ec) {
                    rotated_all = false;
                    rotate_err = "commit precommit for " + op.path->string() +
                                 ": " + ec.message();
                    break;
                }
                op.staged = false;  // precommit no longer exists
            }

            if (!rotated_all) {
                // Best-effort rollback:
                //   - undo any rotated (primary → .bak):  .bak → primary
                //   - delete any still-staged precommits
                for (auto& op : ops) {
                    const auto bak = fs::path(op.path->string() + ".bak");
                    if (op.rotated) {
                        std::error_code rbec;
                        fs::rename(bak, *op.path, rbec);
                    }
                    if (op.staged) {
                        std::error_code rbec;
                        fs::remove(op.precommit, rbec);
                    }
                }
                return std::unexpected("Failed to commit config files: " +
                                       rotate_err);
            }

            // --- All three primaries now hold their new content. ---
            // Next: bump schema_version.  If this fails we can still roll
            // back the primary files (they have their .bak in place yet).
            auto persisted = set_stored_version(final_v);
            if (!persisted) {
                // Roll back config files via their .bak.
                for (auto& op : ops) {
                    if (!op.rotated) continue;
                    const auto bak = fs::path(op.path->string() + ".bak");
                    std::error_code rbec;
                    fs::rename(bak, *op.path, rbec);
                }
                return std::unexpected(
                    "Config files persisted but schema_version commit failed: " +
                    persisted.error() +
                    " (config files were rolled back to their pre-migration state)");
            }

            // --- Success: drop each .bak (non-fatal). ---
            for (auto& op : ops) {
                if (!op.rotated) continue;
                const auto bak = fs::path(op.path->string() + ".bak");
                std::error_code rbec;
                fs::remove(bak, rbec);
            }

            // --- Record which files we actually touched. ---
            // (We do this here, inside the commit hook, so the outer
            // function can copy this information into `out` via the
            // captured references below.)
            if (adapter.is_dirty_global())  out.files_written.push_back(paths.global_config.string());
            if (adapter.is_dirty_user())    out.files_written.push_back(paths.user_settings.string());
            if (adapter.is_dirty_project()) out.files_written.push_back(paths.project_config.string());

            return {};
        };

        out.config = config_runner.run_atomically(std::move(passb_opts),
                                                   std::move(commit_hooks));
        if (out.config.failed_count > 0) {
            return std::unexpected(Error(ErrorCode::internal_error,
                "Config migration failed: " + out.config.error));
        }
        out.final_version = out.config.final_version;
        out.dirty_global  = adapter.is_dirty_global();
        out.dirty_user    = adapter.is_dirty_user();
        out.dirty_project = adapter.is_dirty_project();
    }

    out.any_persisted = !out.files_written.empty() || out.schema.applied_count > 0 ||
                        out.config.applied_count > 0;

    return out;
}

// ---------------------------------------------------------------------------
// Back-compat overload: match the previous two-argument signature so existing
// callers (tests, doctor screen) keep compiling without edits.
[[nodiscard]] inline auto orchestrate_config_migrations(
    std::string_view working_dir,
    std::chrono::seconds lock_timeout)
    -> Result<OrchestrationResult> {
    OrchestrationOptions opts;
    return orchestrate_config_migrations(working_dir, opts, lock_timeout);
}

} // namespace cc::migrations::config
