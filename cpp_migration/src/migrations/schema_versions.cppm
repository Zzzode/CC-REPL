module;
#include <cstdlib>
#include <expected>
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

export module cc.migrations.schema_versions;

import cc.utils.file_persistence;
import cc.utils.lockfile;

export namespace cc::migrations {

namespace fs = std::filesystem;
using cc::utils::LockFile;

// The target schema version for the current application build.
//
// Breakdown:
//   - Versions 1..5  are schema-level migrations  (see migration_registry.cppm)
//   - Versions 6..16 are concrete config migrations (offset 5 from the
//     internal 1..11 numbering in concrete_migrations.cppm).
// The `config_orchestrator` applies both passes and bumps the single,
// monotonic `schema_version` marker across them.
inline constexpr int CURRENT_SCHEMA_VERSION = 16;

// Suffix used for the schema-version backup (kept across one successful write
// so we can recover from a torn rename / crash mid-persist).
inline constexpr std::string_view kVersionBackupSuffix = ".bak";
// Suffix used for the lock file (advisory cross-process lock around writes).
inline constexpr std::string_view kVersionLockSuffix = ".lock";

namespace detail {

// Resolve the root config directory, honouring $HOME with a safe fallback.
[[nodiscard]] inline auto get_config_dir() -> fs::path {
    const char* home = std::getenv("HOME");
    fs::path base = (home && *home) ? fs::path(home) : fs::path("/tmp");
    return base / ".config" / "cc-repl";
}

// Path to the on-disk schema version marker (plain integer text file).
[[nodiscard]] inline auto get_version_file_path() -> fs::path {
    return get_config_dir() / "schema_version";
}

// Path to the backup marker used for atomic write recovery.
[[nodiscard]] inline auto get_version_backup_path() -> fs::path {
    return fs::path(get_version_file_path().string() + std::string(kVersionBackupSuffix));
}

// Path to the cross-process advisory lock that gates version updates.
[[nodiscard]] inline auto get_version_lock_path() -> fs::path {
    return fs::path(get_version_file_path().string() + std::string(kVersionLockSuffix));
}

// Ensure the config directory exists. Returns false if creation fails.
inline auto ensure_config_dir() -> bool {
    std::error_code ec;
    fs::create_directories(get_config_dir(), ec);
    return !ec;
}

// Parse the integer stored inside a version file. Returns -1 on malformed
// content so the caller can distinguish "missing file" (0) from "garbage" (-1).
[[nodiscard]] inline auto parse_version_file(const fs::path& path) -> int {
    if (!fs::exists(path)) {
        return 0;
    }
    std::ifstream f(path);
    if (!f.is_open()) {
        return -1;
    }
    int version = 0;
    f >> version;
    if (f.fail() && !f.eof()) {
        return -1;
    }
    return version;
}

// On startup, try to recover from a torn write:
//   - If primary exists and is parseable -> use it, drop any stale backup.
//   - If primary is missing/broken but backup exists and is valid -> restore
//     backup to primary and return the backup's version.
//   - Otherwise -> return 0 (fresh install).
[[nodiscard]] inline auto recover_and_read() -> int {
    const auto primary = get_version_file_path();
    const auto backup  = get_version_backup_path();

    const int primary_version = parse_version_file(primary);
    if (primary_version >= 0) {
        // Primary is authoritative; drop any leftover backup.
        if (fs::exists(backup)) {
            std::error_code ec;
            fs::remove(backup, ec);
        }
        return primary_version;
    }

    // Primary is malformed or unreadable. Try the backup.
    const int backup_version = parse_version_file(backup);
    if (backup_version > 0) {
        std::error_code ec;
        fs::rename(backup, primary, ec);
        if (!ec) {
            return backup_version;
        }
        // Rename failed; fall through to copy-on-read.
        fs::copy_file(backup, primary, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            return backup_version;
        }
    }
    return 0;
}

} // namespace detail

// Read the stored schema version from disk, transparently recovering from
// a torn write if the backup marker is present.
[[nodiscard]] inline auto get_stored_version() -> int {
    return detail::recover_and_read();
}

// Atomically write the schema version to disk.
//
// Guarantees (on POSIX filesystems):
//   1. Parent directory exists.
//   2. Acquire an advisory cross-process lock so concurrent callers do not
//      interleave the backup/temp/rename dance.
//   3. Write the new payload to a random temp file in the same directory,
//      fsync it (cc::utils::atomic_write handles this), then rotate:
//        a. If a primary exists, copy (rename) it INTO the .bak slot *only after
//           the temp is already on disk (so a torn rename cannot destroy the
//           only good copy).
//        b. rename the temp file over the primary (atomic within a
//           filesystem).
//   4. Unlink the backup once the rename succeeds.
//
// The critical difference from the naive "primary→backup, then write" order is
// that the new bytes are durable *before* the old primary is touched, so a
// crash between the two renames leaves either [primary_old] or
// [primary_old + backup_old] on disk, both of which recover_and_read
// understands.
//
// Returns std::unexpected with a diagnostic message on failure.
[[nodiscard]] inline auto set_stored_version(int version)
    -> std::expected<void, std::string> {
    if (!detail::ensure_config_dir()) {
        return std::unexpected("Unable to create config directory: " +
                               detail::get_config_dir().string());
    }

    // ---- cross-process serialisation --------------------------------------
    LockFile lock(detail::get_version_lock_path());
    if (!lock.is_locked()) {
        return std::unexpected(
            "Timed out waiting for schema_version lock: " +
            detail::get_version_lock_path().string());
    }

    const auto primary = detail::get_version_file_path();
    const auto backup  = detail::get_version_backup_path();

    // ---- write new payload to a temp, make durable ---------------------
    // `atomic_write` internally writes to a sibling temp + fsync + rename over
    // its own temp. We want an extra safety layer *and* to keep a .bak of the
    // previous primary for crash recovery. To avoid opening a window where we
    // have neither primary nor backup, we:
    //
    //   a. write temp_v  (durable)
    //   b. if primary exists → primary.bak  (rotate)
    //   c. temp_v → primary  (atomic rename)
    //
    // To do (b) without losing the ability to recover, we rename PRIMARY into the
    // existing backup slot only AFTER (a) is durable.
    const std::string payload = std::to_string(version) + "\n";

    // Step (a): use a distinct temp so we can later issue (c) without relying on
    // the internal temp of atomic_write (which gets replaced atomically over its
    // target... which IS the target). To honour the (a)(b)(c) ordering we use
    // atomic_write to write to an intermediate `precommit` path first.
    std::error_code ec;
    fs::path precommit = primary;
    precommit += ".precommit";
    // Remove any leftover precommit from a crashed previous run. We know we
    // hold the lock so it's ours.
    fs::remove(precommit, ec);  // ignore

    auto wrote = cc::utils::atomic_write(precommit, payload);
    if (!wrote.has_value()) {
        return std::unexpected("Failed to stage schema_version precommit: " +
                               wrote.error());
    }

    // Step (b): rotate previous primary into the backup slot (only if primary
    // actually exists).
    bool had_primary = fs::exists(primary, ec) && !ec;
    if (had_primary) {
        fs::remove(backup, ec);  // ignore errors (may not exist)
        fs::rename(primary, backup, ec);
        if (ec) {
            // Failed to rotate — best-effort undo: try to restore primary back
            // from backup if possible; otherwise leave precommit in place
            // (nothing durable was committed yet).
            std::error_code ec2;
            if (fs::exists(backup, ec2) && !ec2) {
                fs::rename(backup, primary, ec2);
            }
            fs::remove(precommit, ec2);
            return std::unexpected("Failed to rotate schema_version to backup: " +
                                   ec.message());
        }
    }

    // Step (c): commit the precommit temp over primary. Use fs::rename which
    // is atomic on POSIX.
    fs::rename(precommit, primary, ec);
    if (ec) {
        // Failed to commit. Rollback: restore the backup we just rotated (if any)
        // back to primary so we don't leave a primary-less state behind.
        std::error_code ec2;
        if (had_primary && fs::exists(backup, ec2) && !ec2) {
            fs::rename(backup, primary, ec2);
        }
        fs::remove(precommit, ec2);
        return std::unexpected("Failed to commit schema_version precommit: " +
                               ec.message());
    }

    // Step (d): parent-dir fsync so the rename is durable against power loss.
    // atomic_write already fsynced the precommit + its parent; re-fsync the
    // parent so the final rename→primary is durable.
    const auto parent = primary.parent_path();
    if (!parent.empty()) {
        int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
        if (dir_fd >= 0) {
#if defined(__APPLE__)
            (void)fcntl(dir_fd, F_FULLFSYNC);
#endif
            (void)::fsync(dir_fd);
            ::close(dir_fd);
        }
    }

    // ---- clean up backup (best-effort, non-fatal) -------------------------
    fs::remove(backup, ec);
    return {};
}

// Convenience: read once, compare to CURRENT_SCHEMA_VERSION.
[[nodiscard]] inline auto needs_migration() -> bool {
    return get_stored_version() < CURRENT_SCHEMA_VERSION;
}

// Acquire the schema-version lock and run `fn`, passing in the currently
// stored version.  This lets higher-level orchestrators perform a
// read-modify-write sequence on the schema version safely even when multiple
// processes race.
template <typename Fn>
[[nodiscard]] inline auto with_version_lock(Fn&& fn)
    -> std::expected<decltype(std::declval<Fn>()(0)), std::string> {
    if (!detail::ensure_config_dir()) {
        return std::unexpected("Unable to create config directory: " +
                               detail::get_config_dir().string());
    }
    LockFile lock(detail::get_version_lock_path());
    if (!lock.is_locked()) {
        return std::unexpected(
            "Timed out waiting for schema_version lock: " +
            detail::get_version_lock_path().string());
    }
    return std::forward<Fn>(fn)(detail::recover_and_read());
}

} // namespace cc::migrations
