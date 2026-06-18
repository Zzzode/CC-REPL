module;
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.migrations.migration_runner;

export namespace cc::migrations {

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

/// Outcome of a single migration's up() or down() invocation.
enum class MigrationStatus : uint8_t {
    pending,       // not yet attempted
    skipped,       // preconditions did not match (migration was a no-op)
    applied,       // up() succeeded and wrote changes
    applied_noop,  // up() succeeded but reported no persistent change
    rolled_back,   // down() succeeded
    failed_up,     // up() returned an error
    failed_down,   // down() returned an error
};

/// Per-migration record produced by the runner after an execute/rollback pass.
struct MigrationStep {
    int version = 0;
    std::string name;
    MigrationStatus status = MigrationStatus::pending;
    /// Total wall time spent inside the migration functor, in microseconds.
    int64_t duration_us = 0;
    /// Populated from the functor's error message on failure.
    std::string error;
};

/// Aggregate result of a run_pending / rollback invocation.
struct RunSummary {
    /// Number of migrations whose status is `applied` or `applied_noop`.
    int applied_count = 0;
    /// Number of migrations whose status is `rolled_back`.
    int rolled_back_count = 0;
    /// Number of migrations that reported a failure.
    int failed_count = 0;
    /// Final schema version after the run.
    int final_version = 0;
    /// Ordered list of attempted steps (useful for structured logging / tests).
    std::vector<MigrationStep> steps;
    /// Combined error message when `failed_count > 0`.
    std::string error;
};

/// A single reversible database/schema migration.
struct Migration {
    int version = 0;
    std::string name;
    /// Returns std::unexpected on hard failure. On success the bool reports
    /// whether any persistent state was actually written:
    ///   - true  -> the migration materially changed state (record as `applied`)
    ///   - false -> the migration's preconditions did not match and no writes
    ///              occurred (record as `applied_noop` but still bump version,
    ///              because logically "this migration has been visited").
    std::function<std::expected<bool, std::string>()> up;
    /// Reverse operation. Same bool semantics as `up`: true = wrote state.
    std::function<std::expected<bool, std::string>()> down;
    /// Optional preflight check. If provided, the runner calls this BEFORE
    /// invoking `up()`.  Returning std::unexpected skips the migration with
    /// status `failed_up`.  Returning false skips the migration with status
    /// `skipped` (does NOT count against max_steps and does NOT bump version).
    /// This allows migrations that need a fresh-state precondition (e.g. "only
    /// run on Pro subscriptions") to abort cleanly without registering as
    /// applied or failing the run.
    std::function<std::expected<bool, std::string>()> preflight_up = nullptr;
    /// Optional post-flight hook invoked after `on_version_changed` has
    /// persisted the new version. Useful for publishing side effects that
    /// should only happen once the migration is durably on disk (e.g. sending
    /// an analytics event, broadcasting a version-change IPC). A failure here
    /// is logged to the step but does NOT roll back the migration.
    std::function<std::expected<void, std::string>()> postflight_up = nullptr;
};

/// User-provided observer callbacks. Invoked synchronously by the runner.  All
/// hooks are optional (default nullptr = no-op).  The `on_version_changed` hook
/// is the natural place to persist the schema version to disk after each step,
/// enabling partial-forward progress on later runs if a subsequent migration
/// fails.
struct MigrationHooks {
    std::function<void(const Migration&)> on_before_up;
    std::function<void(const Migration&, MigrationStatus, std::string_view error, int64_t us)> on_after_up;
    std::function<void(const Migration&)> on_before_down;
    std::function<void(const Migration&, MigrationStatus, std::string_view error, int64_t us)> on_after_down;
    /// Called after each successful migration bump so callers can persist the
    /// version to disk atomically.  Returning an error aborts the run.
    std::function<std::expected<void, std::string>(int new_version)> on_version_changed;
    /// Called after `on_version_changed` + postflight_up succeed, once per
    /// step. Unlike `on_after_up`, this fires only when the step is fully
    /// committed (version persisted + postflight ran). Useful for telemetry.
    std::function<void(const Migration&, MigrationStatus)> on_step_committed;
};

// ---------------------------------------------------------------------------
// MigrationRunner
// ---------------------------------------------------------------------------

class MigrationRunner {
public:
    MigrationRunner() = default;

    /// Register a migration. Migrations are kept sorted by version.
    /// Returns an error when a duplicate version is registered (so callers can
    /// fail fast on accidentally duplicated migration numbers rather than
    /// silently overriding).
    [[nodiscard]] auto register_migration(Migration migration)
        -> std::expected<void, std::string> {
        for (const auto& existing : migrations_) {
            if (existing.version == migration.version) {
                return std::unexpected(
                    "Duplicate migration version " +
                    std::to_string(migration.version) +
                    ": already registered as '" + existing.name + "'");
            }
        }
        migrations_.push_back(std::move(migration));
        std::sort(migrations_.begin(), migrations_.end(),
                  [](const Migration& a, const Migration& b) {
                      return a.version < b.version;
                  });
        return {};
    }

    /// Bulk registration helper (preserves the same strict uniqueness check).
    [[nodiscard]] auto register_many(std::vector<Migration> list)
        -> std::expected<void, std::string> {
        for (auto& m : list) {
            if (auto res = register_migration(std::move(m)); !res) {
                return res;
            }
        }
        return {};
    }

    /// Run every migration whose version is strictly greater than
    /// `get_current_version()`, in ascending order.
    ///
    /// Safety rules:
    ///   - On a per-migration failure, attempt to auto-rollback migrations
    ///     that were applied during *this* run in reverse order (so we never
    ///     leave the caller in a half-migrated state).  Each rolled-back step
    ///     is recorded in the summary.
    ///   - If `options.auto_rollback = false`, stop immediately on the first
    ///     failure and leave the runner at the highest *successfully applied*
    ///     version (useful for dry-run/diagnostic modes where the user wants
    ///     to inspect the intermediate state).
    ///   - `options.dry_run = true` skips invocations entirely but still
    ///     reports which migrations *would* run (marked as `skipped`).
    struct RunOptions {
        /// On a per-migration `up()` failure, attempt to undo every migration
        /// that was applied during *this* run, latest-first.
        /// Default: true.
        bool auto_rollback;
        /// Skip calling any migration functors; just report which would have
        /// been invoked (marked as `skipped` in the summary).
        /// Default: false.
        bool dry_run;
        /// If non-zero, stop after applying this many forward migrations
        /// (counts only `applied` + `applied_noop`; failures / skips don't
        /// consume the budget).  Zero means "unlimited".
        /// Default: 0.
        int  max_steps;
        /// If non-zero, stop when the next migration has a version strictly
        /// greater than this value.  0 means "no upper bound".
        /// Default: 0.
        int  target_version;
        /// When true, do not invoke `on_version_changed` or `postflight_up`
        /// after each step — the caller takes responsibility for committing
        /// (see `run_atomically`).
        /// Default: false.
        bool defer_commit;

        /// Sensible defaults: auto_rollback on, dry_run off, unbounded.
        static constexpr RunOptions defaults() noexcept {
            return RunOptions{
                /*auto_rollback=*/true,
                /*dry_run=*/false,
                /*max_steps=*/0,
                /*target_version=*/0,
                /*defer_commit=*/false,
            };
        }
    };

    /// Return the (sorted) list of migrations that still need to run given
    /// the current `current_version_`.  Useful for UIs, logging, and
    /// higher-level orchestrators that want to size progress bars / validate
    /// preconditions up-front.
    [[nodiscard]] auto get_pending() const -> std::vector<Migration> {
        std::vector<Migration> out;
        for (const auto& m : migrations_) {
            if (m.version > current_version_) out.push_back(m);
        }
        return out;
    }

    /// Return the (sorted) list of migrations whose version is <=
    /// current_version_.  Useful for audit / reporting.
    [[nodiscard]] auto get_applied() const -> std::vector<Migration> {
        std::vector<Migration> out;
        for (const auto& m : migrations_) {
            if (m.version <= current_version_) out.push_back(m);
        }
        return out;
    }

    /// Convenience wrapper: run pending migrations up to and including
    /// `target_version`.  Specifying 0 for `target_version` is equivalent to
    /// calling `run_pending()` with no upper bound.
    [[nodiscard]] auto run_to_version(int target_version,
                                      RunOptions options = RunOptions::defaults(),
                                      MigrationHooks hooks = {}) -> RunSummary {
        if (target_version > 0) {
            // Do not override a stricter user-provided target_version.
            if (options.target_version == 0 || target_version < options.target_version) {
                options.target_version = target_version;
            }
        }
        return run_pending(std::move(options), std::move(hooks));
    }

    /// Run pending migrations as a *single atomic unit*: either ALL of them
    /// succeed (then `on_version_changed` fires exactly once for the final
    /// version + postflights run in order), or NONE are observable (the
    /// runner auto-rolls back every in-memory side-effect and resets
    /// current_version to its starting value).
    ///
    /// Use this instead of `run_pending()` when the downstream persistence
    /// layer can only commit the state *and* the version bump together (e.g.
    /// config_orchestrator rewriting multiple JSON files in lockstep with
    /// schema_version).
    ///
    /// Semantics:
    ///   - Internally sets `options.defer_commit = true`.
    ///   - On success, invokes `hooks.on_version_changed(final)` exactly once,
    ///     then runs `postflight_up` for each applied migration in order.
    ///   - `on_step_committed` fires once per applied migration, after the
    ///     postflight, so observers see a consistent ordering.
    ///   - On ANY failure (up / persist / postflight), rolls back every
    ///     migration that was applied during this call and returns a summary
    ///     with `final_version == starting_version`.
    [[nodiscard]] auto run_atomically(RunOptions options = RunOptions::defaults(),
                                      MigrationHooks hooks = {}) -> RunSummary {
        const int starting_version = current_version_;
        options.defer_commit = true;
        // Always auto-rollback in atomic mode — the whole point is "all or
        // nothing" at the runner level.  A caller that asked for false would
        // not get atomic semantics, so we override defensively.
        options.auto_rollback = true;

        RunSummary summary = run_pending(std::move(options), hooks);
        if (summary.failed_count > 0) {
            // run_pending's internal auto-rollback should have already
            // restored current_version_ as close as possible, but guarantee
            // it anyway for the "nothing observable" contract.
            current_version_ = starting_version;
            summary.final_version = starting_version;
            return summary;
        }

        // All in-memory work succeeded. Now do the durable commit:
        //  1. Call on_version_changed exactly once with the final version.
        //  2. Run postflights in ascending order.
        //  3. Fire on_step_committed for each step that applied.
        if (hooks.on_version_changed) {
            auto persist = hooks.on_version_changed(summary.final_version);
            if (!persist.has_value()) {
                summary.failed_count++;
                summary.error =
                    "run_atomically: failed to commit final version v" +
                    std::to_string(summary.final_version) + ": " +
                    persist.error();
                // Rollback every applied migration so the commit failure is
                // observable as a complete no-op.
                std::vector<size_t> indices;
                for (size_t i = 0; i < migrations_.size(); ++i) {
                    if (migrations_[i].version > starting_version &&
                        migrations_[i].version <= summary.final_version) {
                        indices.push_back(i);
                    }
                }
                auto rb_err = rollback_in_place(indices, hooks, summary);
                if (!rb_err.empty()) {
                    summary.error +=
                        "\nAdditionally, rollback of applied steps failed: " +
                        rb_err;
                }
                current_version_ = starting_version;
                summary.final_version = starting_version;
                return summary;
            }
        }

        // Postflights in migration order (ascending).
        for (auto& step : summary.steps) {
            if (step.status != MigrationStatus::applied &&
                step.status != MigrationStatus::applied_noop) {
                continue;
            }
            const Migration* m = find_migration(step.version);
            if (!m) continue;
            if (m->postflight_up) {
                auto pf = m->postflight_up();
                if (!pf.has_value()) {
                    step.error  = "postflight: " + pf.error();
                    // Postflight is best-effort-once-durable: do not roll
                    // back, but surface it so the caller can alert.
                    if (summary.error.empty()) summary.error = step.error;
                }
            }
            if (hooks.on_step_committed) {
                hooks.on_step_committed(*m, step.status);
            }
        }
        return summary;
    }

    [[nodiscard]] auto run_pending(RunOptions options = RunOptions::defaults(),
                                   MigrationHooks hooks = {}) -> RunSummary {
        RunSummary summary;
        summary.final_version = current_version_;

        int steps_taken = 0;
        // Track the indices of migrations applied during *this* run so we can
        // reverse them if something later fails.
        std::vector<size_t> applied_this_run;

        for (size_t i = 0; i < migrations_.size(); ++i) {
            const auto& m = migrations_[i];

            MigrationStep step;
            step.version = m.version;
            step.name    = m.name;

            if (m.version <= current_version_) {
                step.status = MigrationStatus::skipped;
                summary.steps.push_back(std::move(step));
                continue;
            }
            if (options.target_version > 0 && m.version > options.target_version) {
                step.status = MigrationStatus::skipped;
                step.error  = "(target_version reached)";
                summary.steps.push_back(std::move(step));
                continue;
            }
            if (options.max_steps > 0 && steps_taken >= options.max_steps) {
                step.status = MigrationStatus::skipped;
                step.error  = "(max_steps reached)";
                summary.steps.push_back(std::move(step));
                continue;
            }

            if (options.dry_run) {
                step.status   = MigrationStatus::skipped;
                step.error    = "(dry-run)";
                summary.steps.push_back(std::move(step));
                continue;
            }

            // ---- preflight: optional "should this migration run?" gate ----
            if (m.preflight_up) {
                auto pf = m.preflight_up();
                if (!pf.has_value()) {
                    step.status = MigrationStatus::failed_up;
                    step.error  = "preflight: " + pf.error();
                    summary.failed_count++;
                    summary.error =
                        "Migration v" + std::to_string(m.version) +
                        " (" + m.name + ") preflight failed: " + pf.error();
                    summary.steps.push_back(std::move(step));
                    if (options.auto_rollback) {
                        auto rb_err = rollback_in_place(applied_this_run, hooks, summary);
                        if (!rb_err.empty()) {
                            summary.error +=
                                "\nAdditionally, auto-rollback failed: " + rb_err;
                        }
                    }
                    summary.final_version = current_version_;
                    return summary;
                }
                if (!*pf) {
                    step.status = MigrationStatus::skipped;
                    step.error  = "(precondition not met)";
                    summary.steps.push_back(std::move(step));
                    continue;
                }
            }

            if (hooks.on_before_up) hooks.on_before_up(m);

            const auto t0 = std::chrono::steady_clock::now();
            auto result   = m.up();
            const auto t1 = std::chrono::steady_clock::now();
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            step.duration_us = us;

            if (!result.has_value()) {
                step.status = MigrationStatus::failed_up;
                step.error  = result.error();
                summary.failed_count++;
                if (hooks.on_after_up) {
                    hooks.on_after_up(m, step.status, step.error, us);
                }
                summary.steps.push_back(std::move(step));

                // Build the combined error string *before* possibly rolling
                // back (the rollback may add more diagnostics).
                summary.error =
                    "Migration v" + std::to_string(m.version) +
                    " (" + m.name + ") failed: " + result.error();

                if (options.auto_rollback) {
                    auto rollback_error = rollback_in_place(applied_this_run, hooks, summary);
                    if (!rollback_error.empty()) {
                        summary.error +=
                            "\nAdditionally, auto-rollback failed: " + rollback_error;
                    }
                }
                summary.final_version = current_version_;
                return summary;
            }

            step.status = *result ? MigrationStatus::applied
                                  : MigrationStatus::applied_noop;
            applied_this_run.push_back(i);
            ++summary.applied_count;
            ++steps_taken;
            current_version_ = m.version;
            summary.final_version = current_version_;

            if (hooks.on_after_up) {
                hooks.on_after_up(m, step.status, "", us);
            }
            // Persist after each step UNLESS the caller asked us to defer the
            // commit (run_atomically uses this).  If persistence fails, treat
            // it like any other failure: roll back the logical step too.
            if (!options.defer_commit && hooks.on_version_changed) {
                auto persist = hooks.on_version_changed(current_version_);
                if (!persist.has_value()) {
                    step.status = MigrationStatus::failed_up;
                    step.error  = "persist version: " + persist.error();
                    summary.failed_count++;
                    summary.error =
                        "After applying v" + std::to_string(m.version) +
                        " (" + m.name + "), could not persist version: " +
                        persist.error();
                    if (hooks.on_after_up) {
                        hooks.on_after_up(m, step.status, step.error, us);
                    }
                    summary.steps.push_back(std::move(step));
                    if (options.auto_rollback) {
                        // Undo the step we just appended too.
                        applied_this_run.pop_back();
                        current_version_ = m.version - 1;
                        summary.applied_count--;
                        steps_taken--;
                        auto rb_err = rollback_in_place(applied_this_run, hooks, summary);
                        if (!rb_err.empty()) {
                            summary.error +=
                                "\nAdditionally, auto-rollback failed: " + rb_err;
                        }
                    }
                    summary.final_version = current_version_;
                    return summary;
                }

                // Non-deferred commit path: run postflight + on_step_committed
                // synchronously here, once the version is durably on disk.
                if (m.postflight_up) {
                    auto pf = m.postflight_up();
                    if (!pf.has_value()) {
                        step.error = "postflight: " + pf.error();
                        if (summary.error.empty()) summary.error = step.error;
                    }
                }
                if (hooks.on_step_committed) {
                    hooks.on_step_committed(m, step.status);
                }
            }

            summary.steps.push_back(std::move(step));
        }

        return summary;
    }

    /// Roll back the most recent `steps` applied migrations, in reverse
    /// order.  Uses the same hooks/persistence behaviour as `run_pending`.
    [[nodiscard]] auto rollback(int steps = 1, MigrationHooks hooks = {})
        -> RunSummary {
        RunSummary summary;
        summary.final_version = current_version_;
        if (steps <= 0) return summary;

        int rolled = 0;
        // Walk the registry in reverse, invoking down() for each migration
        // whose version we are currently >=.
        for (auto it = migrations_.rbegin();
             it != migrations_.rend() && rolled < steps;
             ++it) {
            const auto& m = *it;
            MigrationStep step;
            step.version = m.version;
            step.name    = m.name;

            if (m.version > current_version_) {
                step.status = MigrationStatus::skipped;
                summary.steps.push_back(std::move(step));
                continue;
            }

            if (hooks.on_before_down) hooks.on_before_down(m);

            const auto t0 = std::chrono::steady_clock::now();
            auto result   = m.down();
            const auto t1 = std::chrono::steady_clock::now();
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            step.duration_us = us;

            if (!result.has_value()) {
                step.status = MigrationStatus::failed_down;
                step.error  = result.error();
                summary.failed_count++;
                summary.error =
                    "Rollback of v" + std::to_string(m.version) +
                    " (" + m.name + ") failed: " + result.error();
                if (hooks.on_after_down) {
                    hooks.on_after_down(m, step.status, step.error, us);
                }
                summary.steps.push_back(std::move(step));
                summary.final_version = current_version_;
                return summary;
            }

            step.status = MigrationStatus::rolled_back;
            ++rolled;
            ++summary.rolled_back_count;
            current_version_ = m.version - 1;
            summary.final_version = current_version_;

            if (hooks.on_after_down) {
                hooks.on_after_down(m, step.status, "", us);
            }
            if (hooks.on_version_changed) {
                auto persist = hooks.on_version_changed(current_version_);
                if (!persist.has_value()) {
                    step.status = MigrationStatus::failed_down;
                    step.error  = "persist version: " + persist.error();
                    summary.failed_count++;
                    summary.error =
                        "After rolling back v" + std::to_string(m.version) +
                        ", could not persist version: " + persist.error();
                    if (hooks.on_after_down) {
                        hooks.on_after_down(m, step.status, step.error, us);
                    }
                    summary.steps.push_back(std::move(step));
                    summary.final_version = current_version_;
                    return summary;
                }
            }
            if (hooks.on_step_committed) {
                hooks.on_step_committed(m, step.status);
            }

            summary.steps.push_back(std::move(step));
        }

        if (current_version_ < 0) current_version_ = 0;
        summary.final_version = current_version_;
        return summary;
    }

    // -- accessors ---------------------------------------------------------

    [[nodiscard]] auto get_current_version() const -> int {
        return current_version_;
    }
    auto set_current_version(int version) -> void { current_version_ = version; }

    [[nodiscard]] auto get_migrations() const -> const std::vector<Migration>& {
        return migrations_;
    }
    /// Return the highest registered version, or 0 if none are registered.
    [[nodiscard]] auto get_max_registered_version() const -> int {
        if (migrations_.empty()) return 0;
        return migrations_.back().version;
    }

    /// Look up a migration by version (linear scan; registry is small).
    [[nodiscard]] auto find_migration(int version) const -> const Migration* {
        for (const auto& m : migrations_) {
            if (m.version == version) return &m;
        }
        return nullptr;
    }

private:
    // Helper used by run_pending to roll back a list of migrations that were
    // applied during the current execution (by registry index, in reverse).
    // Appends additional Step entries to summary and updates
    // rolled_back_count / current_version_.  Returns the first non-empty
    // error message on failure (further rollback is aborted).
    [[nodiscard]] auto rollback_in_place(
        const std::vector<size_t>& indices,
        MigrationHooks& hooks,
        RunSummary& summary) -> std::string {
        // Iterate in reverse so we undo latest-first.
        for (auto idx_it = indices.rbegin(); idx_it != indices.rend(); ++idx_it) {
            const size_t i = *idx_it;
            if (i >= migrations_.size()) continue;
            const auto& m = migrations_[i];

            MigrationStep step;
            step.version = m.version;
            step.name    = m.name;

            if (hooks.on_before_down) hooks.on_before_down(m);

            const auto t0 = std::chrono::steady_clock::now();
            auto result   = m.down();
            const auto t1 = std::chrono::steady_clock::now();
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            step.duration_us = us;

            if (!result.has_value()) {
                step.status = MigrationStatus::failed_down;
                step.error  = result.error();
                summary.failed_count++;
                summary.steps.push_back(std::move(step));
                if (hooks.on_after_down) {
                    hooks.on_after_down(m, step.status, step.error, us);
                }
                return "v" + std::to_string(m.version) + " (" + m.name + "): " +
                       result.error();
            }

            step.status = MigrationStatus::rolled_back;
            ++summary.rolled_back_count;
            --summary.applied_count;
            current_version_ = m.version - 1;
            if (current_version_ < 0) current_version_ = 0;
            summary.final_version = current_version_;

            if (hooks.on_after_down) {
                hooks.on_after_down(m, step.status, "", us);
            }
            if (hooks.on_version_changed) {
                auto persist = hooks.on_version_changed(current_version_);
                if (!persist.has_value()) {
                    step.status = MigrationStatus::failed_down;
                    step.error  = "persist version: " + persist.error();
                    summary.failed_count++;
                    summary.steps.push_back(std::move(step));
                    return step.error;
                }
            }
            if (hooks.on_step_committed) {
                hooks.on_step_committed(m, step.status);
            }
            summary.steps.push_back(std::move(step));
        }
        return {};
    }

    std::vector<Migration> migrations_;
    int current_version_ = 0;
};

} // namespace cc::migrations
