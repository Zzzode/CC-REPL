// Secure credential storage abstraction — macOS Keychain, fallback plain text,
// keychain prefetch. Migrated from src/utils/secureStorage/*.ts
module;

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.secure_storage;

import cc.utils.json;

export namespace cc::utils::secure_storage {

using JsonVal = cc::utils::json::JsonVal;
using namespace std::chrono;

// --- Types ---

/// Opaque credential data stored as JSON key-value pairs.
/// Mirrors TS `SecureStorageData = Record<string, unknown>`.
using SecureStorageData = cc::utils::json::JsonMutDoc;

/// Result returned by update operations.
struct UpdateResult {
    bool success;
    std::optional<std::string> warning;
};

/// Abstract interface for credential backends (keychain, plaintext).
struct SecureStorage {
    virtual ~SecureStorage() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Synchronous read — may spawn a subprocess (macOS security CLI).
    [[nodiscard]] virtual std::expected<std::string, std::string> read() = 0;

    /// Asynchronous read (fire-and-coalesce pattern).
    [[nodiscard]] virtual std::expected<std::string, std::string> read_async() = 0;

    /// Write credential data.
    [[nodiscard]] virtual UpdateResult update(std::string_view json_data) = 0;

    /// Delete stored credentials.
    [[nodiscard]] virtual bool remove() = 0;
};

// --- macOS Keychain Cache ---

/// TTL for keychain read cache. Cross-process staleness up to 30s is acceptable:
/// OAuth tokens expire in hours; only /login or refresh writes across processes.
inline constexpr auto kKeychainCacheTtl = seconds{30};

/// Suffix distinguishing the OAuth credentials keychain entry from the legacy
/// API key entry. DO NOT change — part of the keychain lookup key.
inline constexpr std::string_view kCredentialsServiceSuffix = "-credentials";

/// Stdin line limit for macOS `security -i` (4096 - 64 headroom).
inline constexpr std::size_t kSecurityStdinLineLimit = 4096 - 64;

/// Keychain prefetch timeout.
inline constexpr auto kKeychainPrefetchTimeout = seconds{10};

// --- Keychain cache state (internal, shared between prefetch and read) ---

struct KeychainCacheEntry {
    std::optional<std::string> data;  // nullopt = no cached value
    steady_clock::time_point cached_at;
};

struct KeychainCacheState {
    KeychainCacheEntry cache{std::nullopt, steady_clock::time_point{}};
    int generation{0};
};

/// Get the macOS Keychain service name.
/// @param service_suffix Additional suffix (e.g. "-credentials").
[[nodiscard]] std::string get_macos_keychain_service_name(
    std::string_view service_suffix = "");

/// Get the current username for keychain lookups.
[[nodiscard]] std::string get_username();

/// Clear the keychain cache (invalidation on write/delete).
void clear_keychain_cache();

/// Prime the keychain cache from a prefetch result.
/// Only writes if cache hasn't been touched yet.
void prime_keychain_cache_from_prefetch(std::optional<std::string_view> stdout_data);

// --- macOS Keychain Storage ---

/// Create the macOS Keychain secure storage backend.
[[nodiscard]] std::unique_ptr<SecureStorage> create_macos_keychain_storage();

/// Check if the macOS Keychain is locked (e.g. in SSH sessions).
/// Cached for process lifetime.
[[nodiscard]] bool is_macos_keychain_locked();

// --- Plain Text Storage ---

/// Create a plain-text file storage backend (~/.claude/.credentials.json).
[[nodiscard]] std::unique_ptr<SecureStorage> create_plain_text_storage();

// --- Fallback Storage ---

/// Create a fallback storage: tries primary first, falls back to secondary.
/// On macOS: primary = keychain, secondary = plaintext.
[[nodiscard]] std::unique_ptr<SecureStorage> create_fallback_storage(
    std::unique_ptr<SecureStorage> primary,
    std::unique_ptr<SecureStorage> secondary);

// --- Factory ---

/// Get the appropriate secure storage for the current platform.
/// macOS: keychain-with-plaintext-fallback; other: plaintext.
[[nodiscard]] std::unique_ptr<SecureStorage> get_secure_storage();

// --- Keychain Prefetch ---

/// Fire both keychain reads in parallel (OAuth + legacy API key).
/// Non-darwin is a no-op. Called at startup before heavy imports.
void start_keychain_prefetch();

/// Await prefetch completion. Nearly free if subprocesses finished during init.
void ensure_keychain_prefetch_completed();

/// Get the legacy API key prefetch result. Returns nullopt if not completed.
[[nodiscard]] std::optional<std::optional<std::string>> get_legacy_api_key_prefetch_result();

/// Clear legacy API key prefetch result (cache invalidation).
void clear_legacy_api_key_prefetch();

} // namespace cc::utils::secure_storage
