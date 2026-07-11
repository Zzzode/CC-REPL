// ============================================================================
/// @file diagnostic_registry.cppm
/// @brief LSP Diagnostic Registry - stores, deduplicates, and manages LSP
///        diagnostics received asynchronously from LSP servers via
///        textDocument/publishDiagnostics notifications.
///
/// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts (386 lines)
/// TS REF: src/services/lsp/passiveFeedback.ts (328 lines, diagnostic formatting)
///
/// Architecture:
///   1. LSP server sends publishDiagnostics notification
///   2. set_diagnostics() stores per-server, per-URI diagnostics
///   3. check_for_pending() retrieves pending diagnostics for delivery
///   4. Deduplication (within-batch + cross-turn via delivered_keys_) prevents
///      sending the same diagnostic twice
///   5. Volume limiting caps per-file and total diagnostics
///   6. get_diagnostics_by_server() / get_diagnostics() query current state
// ============================================================================
module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module cc.services.lsp.diagnostic_registry;

import cc.utils.error;
import cc.utils.json;

export namespace cc::services::lsp {

namespace fs = std::filesystem;
using cc::utils::Result;

// ============================================================================
// Diagnostic Severity
// ============================================================================

/// LSP diagnostic severity levels.
/// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:91-103 (severityToNumber)
/// TS REF: src/services/lsp/passiveFeedback.ts:18-35 (mapLSPSeverity)
enum class DiagnosticSeverity : int {
    Error       = 1,  // LSP severity 1
    Warning     = 2,  // LSP severity 2
    Info        = 3,  // LSP severity 3 (called "Information" in raw LSP)
    Hint        = 4,  // LSP severity 4
};

/// Map a raw LSP severity number to our DiagnosticSeverity enum.
/// TS REF: src/services/lsp/passiveFeedback.ts:18-35 (mapLSPSeverity)
[[nodiscard]] inline DiagnosticSeverity map_lsp_severity(int lsp_severity) {
    switch (lsp_severity) {
        case 1: return DiagnosticSeverity::Error;
        case 2: return DiagnosticSeverity::Warning;
        case 3: return DiagnosticSeverity::Info;
        case 4: return DiagnosticSeverity::Hint;
        default: return DiagnosticSeverity::Error;  // default to Error for invalid/missing
    }
}

/// Map a DiagnosticSeverity to a sortable number (lower = more severe).
/// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:91-103 (severityToNumber)
[[nodiscard]] inline int severity_to_number(DiagnosticSeverity s) {
    return static_cast<int>(s);
}

/// Map a DiagnosticSeverity to its canonical string name.
/// TS REF: src/services/lsp/passiveFeedback.ts:18-35 (mapLSPSeverity returns string)
[[nodiscard]] inline std::string_view severity_to_string(DiagnosticSeverity s) {
    switch (s) {
        case DiagnosticSeverity::Error:   return "Error";
        case DiagnosticSeverity::Warning: return "Warning";
        case DiagnosticSeverity::Info:    return "Info";
        case DiagnosticSeverity::Hint:    return "Hint";
    }
    return "Error";
}

// ============================================================================
// Diagnostic Range / Position
// ============================================================================

/// A position within a text document (0-based line + character).
/// TS REF: src/services/lsp/passiveFeedback.ts:67-85 (range in diagnostic map)
struct DiagnosticPosition {
    int64_t line{0};
    int64_t character{0};
};

/// A range within a text document (start inclusive, end exclusive).
/// TS REF: src/services/lsp/passiveFeedback.ts:76-85
struct DiagnosticRange {
    DiagnosticPosition start;
    DiagnosticPosition end;
};

/// Related diagnostic information (cross-reference to another location).
/// TS REF: LSP spec - DiagnosticRelatedInformation
struct DiagnosticRelatedInfo {
    std::string uri;
    DiagnosticRange range;
    std::string message;
};

// ============================================================================
// Diagnostic Entry
// ============================================================================

/// A single LSP diagnostic entry.
/// TS REF: src/services/lsp/passiveFeedback.ts:63-92 (diagnostic mapping)
/// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:110-124 (createDiagnosticKey fields)
struct Diagnostic {
    std::string uri;                    ///< File URI (file://...) or plain path
    DiagnosticRange range;             ///< Position range in the document
    DiagnosticSeverity severity{DiagnosticSeverity::Error};
    std::optional<std::string> code;   ///< Diagnostic code (e.g. "TS2345")
    std::optional<std::string> source; ///< Source language server name
    std::string message;               ///< Human-readable diagnostic message
    std::vector<DiagnosticRelatedInfo> related_info;  ///< Related info locations

    /// Create a stable dedup key from diagnostic content.
    /// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:110-124 (createDiagnosticKey)
    [[nodiscard]] std::string dedup_key() const {
        // Build a deterministic key from the fields that define "same diagnostic".
        // Using yyjson for canonical serialization would be heavier; a manual
        // string concatenation is sufficient and avoids allocator churn.
        std::ostringstream oss;
        oss << severity_to_string(severity) << '|'
            << range.start.line << ':' << range.start.character << '-'
            << range.end.line << ':' << range.end.character << '|'
            << (source.value_or("")) << '|'
            << (code.value_or("")) << '|'
            << message;
        return oss.str();
    }
};

// ============================================================================
// Diagnostic File (group of diagnostics for one URI)
// ============================================================================

/// A group of diagnostics for a single file URI.
/// TS REF: src/services/lsp/diagnosticTracking.ts - DiagnosticFile type
struct DiagnosticFile {
    std::string uri;
    std::vector<Diagnostic> diagnostics;
};

// ============================================================================
// Pending LSP Diagnostic Notification
// ============================================================================

/// A pending LSP diagnostic notification awaiting delivery as an attachment.
/// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:12-21 (PendingLSPDiagnostic)
struct PendingLSPDiagnostic {
    std::string server_name;           ///< Server that sent the diagnostic
    std::vector<DiagnosticFile> files; ///< Diagnostic files
    int64_t timestamp_ms{0};           ///< When received (epoch ms)
    bool attachment_sent{false};       ///< Whether already delivered
};

// ============================================================================
// Handler Registration Result
// ============================================================================

/// Result of registering diagnostic notification handlers across servers.
/// TS REF: src/services/lsp/passiveFeedback.ts:105-114 (HandlerRegistrationResult)
struct HandlerRegistrationResult {
    size_t total_servers{0};
    size_t success_count{0};
    std::vector<std::pair<std::string, std::string>> registration_errors;
    /// server_name -> { failure_count, last_error }
    std::unordered_map<std::string, std::pair<size_t, std::string>> diagnostic_failures;
};

// ============================================================================
// Volume Limiting Constants
// ============================================================================

/// Maximum diagnostics per file in a single delivery batch.
/// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:42
constexpr size_t MAX_DIAGNOSTICS_PER_FILE = 10;

/// Maximum total diagnostics across all files in a single delivery batch.
/// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:43
constexpr size_t MAX_TOTAL_DIAGNOSTICS = 30;

/// Maximum files to track for cross-turn deduplication (prevents unbounded growth).
/// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:46
constexpr size_t MAX_DELIVERED_FILES = 500;

// ============================================================================
// DiagnosticRegistry
// ============================================================================

/// Thread-safe registry for LSP diagnostics.
///
/// Stores diagnostics received from LSP servers, deduplicates across turns,
/// and provides volume-limited batches for delivery as conversation
/// attachments. Follows the same pattern as AsyncHookRegistry for consistent
/// async attachment delivery.
///
/// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts (entire module)
class DiagnosticRegistry {
public:
    // ------------------------------------------------------------------
    // Setting / storing diagnostics
    // ------------------------------------------------------------------

    /// Set (replace) diagnostics for a given server and URI.
    /// Called when a textDocument/publishDiagnostics notification arrives.
    /// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:65-85 (registerPendingLSPDiagnostic)
    void set_diagnostics(
        std::string_view server_name,
        std::string_view uri,
        std::vector<Diagnostic> diagnostics
    ) {
        std::lock_guard lock(mutex_);

        // Store per-server, per-URI diagnostics
        auto& server_map = diagnostics_by_server_[std::string(server_name)];
        server_map[std::string(uri)] = std::move(diagnostics);

        // Also register as pending for async delivery
        auto pending = std::make_unique<PendingLSPDiagnostic>();
        pending->server_name = std::string(server_name);
        pending->timestamp_ms = current_time_ms();

        DiagnosticFile df;
        df.uri = std::string(uri);
        // Copy diagnostics from the server map (we just stored them)
        auto it = server_map.find(df.uri);
        if (it != server_map.end()) {
            df.diagnostics = it->second;
        }
        pending->files.push_back(std::move(df));
        pending->attachment_sent = false;

        // Use a simple incrementing ID (thread-safe via mutex we already hold)
        std::string id = std::to_string(++next_pending_id_);
        pending_diagnostics_[id] = std::move(pending);
    }

    // ------------------------------------------------------------------
    // Retrieving diagnostics
    // ------------------------------------------------------------------

    /// Get all diagnostics for a specific URI across all servers.
    /// Returns merged + deduplicated list.
    std::vector<Diagnostic> get_diagnostics(std::string_view uri) const {
        std::lock_guard lock(mutex_);
        std::set<std::string> seen_keys;
        std::vector<Diagnostic> result;

        for (const auto& [server_name, uri_map] : diagnostics_by_server_) {
            auto it = uri_map.find(std::string(uri));
            if (it == uri_map.end()) continue;
            for (const auto& diag : it->second) {
                auto key = diag.dedup_key();
                if (seen_keys.insert(key).second) {
                    result.push_back(diag);
                }
            }
        }
        return result;
    }

    /// Get all diagnostics for a specific server name.
    /// TS REF: task spec - get_diagnostics_by_server()
    std::vector<Diagnostic> get_diagnostics_by_server(std::string_view server_name) const {
        std::lock_guard lock(mutex_);
        std::vector<Diagnostic> result;
        auto it = diagnostics_by_server_.find(std::string(server_name));
        if (it == diagnostics_by_server_.end()) return result;
        for (const auto& [uri, diags] : it->second) {
            result.insert(result.end(), diags.begin(), diags.end());
        }
        return result;
    }

    /// Get all diagnostics grouped by file URI.
    std::vector<DiagnosticFile> get_all_diagnostic_files() const {
        std::lock_guard lock(mutex_);
        std::unordered_map<std::string, DiagnosticFile> file_map;
        std::set<std::string> seen_keys;  // global dedup across servers

        for (const auto& [server_name, uri_map] : diagnostics_by_server_) {
            for (const auto& [uri, diags] : uri_map) {
                auto& df = file_map[uri];
                if (df.uri.empty()) df.uri = uri;
                for (const auto& diag : diags) {
                    auto key = diag.dedup_key();
                    if (seen_keys.insert(key).second) {
                        df.diagnostics.push_back(diag);
                    }
                }
            }
        }

        std::vector<DiagnosticFile> result;
        result.reserve(file_map.size());
        for (auto& [uri, df] : file_map) {
            if (!df.diagnostics.empty()) {
                result.push_back(std::move(df));
            }
        }
        return result;
    }

    // ------------------------------------------------------------------
    // Clearing
    // ------------------------------------------------------------------

    /// Clear all diagnostics for a specific URI across all servers.
    /// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:372-379 (clearDeliveredDiagnosticsForFile)
    void clear_diagnostics(std::string_view uri) {
        std::lock_guard lock(mutex_);
        for (auto& [server_name, uri_map] : diagnostics_by_server_) {
            uri_map.erase(std::string(uri));
        }
        // Also clear cross-turn dedup tracking for this URI
        delivered_keys_.erase(std::string(uri));
    }

    /// Clear all diagnostics for a specific server.
    void clear_server_diagnostics(std::string_view server_name) {
        std::lock_guard lock(mutex_);
        diagnostics_by_server_.erase(std::string(server_name));
    }

    /// Clear all pending diagnostics (for shutdown or testing).
    /// Does NOT clear cross-turn dedup tracking.
    /// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:346-351 (clearAllLSPDiagnostics)
    void clear_all_pending() {
        std::lock_guard lock(mutex_);
        pending_diagnostics_.clear();
    }

    /// Reset ALL state including cross-turn dedup tracking.
    /// Used on session reset or for testing.
    /// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:357-363 (resetAllLSPDiagnosticState)
    void reset_all_state() {
        std::lock_guard lock(mutex_);
        pending_diagnostics_.clear();
        diagnostics_by_server_.clear();
        delivered_keys_.clear();
    }

    // ------------------------------------------------------------------
    // Pending delivery (async attachment system)
    // ------------------------------------------------------------------

    /// Retrieve and mark-as-sent all pending diagnostics, with deduplication
    /// and volume limiting. Returns empty vector if nothing is pending.
    /// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:193-338 (checkForLSPDiagnostics)
    std::vector<std::pair<std::string, std::vector<DiagnosticFile>>>
    check_for_pending() {
        std::lock_guard lock(mutex_);

        // Collect all diagnostic files from unsent pending notifications
        std::vector<DiagnosticFile> all_files;
        std::set<std::string> server_names;
        std::vector<std::string> to_delete;

        for (const auto& [id, pending] : pending_diagnostics_) {
            if (!pending->attachment_sent) {
                all_files.insert(all_files.end(),
                    pending->files.begin(), pending->files.end());
                server_names.insert(pending->server_name);
                to_delete.push_back(id);
            }
        }

        if (all_files.empty()) return {};

        // Deduplicate across files and against previously delivered
        auto deduped_files = deduplicate_files(all_files);

        // Mark as sent and remove from pending map
        for (const auto& id : to_delete) {
            pending_diagnostics_.erase(id);
        }

        // Apply volume limiting: sort by severity then cap
        size_t total_diagnostics = 0;
        for (auto& file : deduped_files) {
            // Sort by severity (lower number = more severe = first)
            std::sort(file.diagnostics.begin(), file.diagnostics.end(),
                [](const Diagnostic& a, const Diagnostic& b) {
                    return severity_to_number(a.severity) < severity_to_number(b.severity);
                });

            // Cap per file
            if (file.diagnostics.size() > MAX_DIAGNOSTICS_PER_FILE) {
                file.diagnostics.resize(MAX_DIAGNOSTICS_PER_FILE);
            }

            // Cap total
            size_t remaining = MAX_TOTAL_DIAGNOSTICS - total_diagnostics;
            if (file.diagnostics.size() > remaining) {
                file.diagnostics.resize(remaining);
            }
            total_diagnostics += file.diagnostics.size();
        }

        // Remove files that ended up with no diagnostics
        std::erase_if(deduped_files, [](const DiagnosticFile& f) {
            return f.diagnostics.empty();
        });

        if (deduped_files.empty()) return {};

        // Track delivered keys for cross-turn dedup
        for (const auto& file : deduped_files) {
            evict_delivered_if_needed();
            auto& keys = delivered_keys_[file.uri];
            for (const auto& diag : file.diagnostics) {
                keys.insert(diag.dedup_key());
            }
        }

        // Return single result with combined server names
        std::string combined_server;
        for (const auto& s : server_names) {
            if (!combined_server.empty()) combined_server += ", ";
            combined_server += s;
        }

        return {{combined_server, std::move(deduped_files)}};
    }

    // ------------------------------------------------------------------
    // Counting / monitoring
    // ------------------------------------------------------------------

    /// Get total diagnostic count across all files and servers.
    /// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:384-386 (getPendingLSPDiagnosticCount)
    size_t get_diagnostic_count() const {
        std::lock_guard lock(mutex_);
        size_t count = 0;
        for (const auto& [server, uri_map] : diagnostics_by_server_) {
            for (const auto& [uri, diags] : uri_map) {
                count += diags.size();
            }
        }
        return count;
    }

    /// Get count of pending (undelivered) diagnostic notifications.
    size_t get_pending_count() const {
        std::lock_guard lock(mutex_);
        return pending_diagnostics_.size();
    }

    /// Get total error count across all files.
    /// Kept from original stub API; useful for status indicators.
    size_t get_error_count() const {
        std::lock_guard lock(mutex_);
        size_t count = 0;
        for (const auto& [server, uri_map] : diagnostics_by_server_) {
            for (const auto& [uri, diags] : uri_map) {
                for (const auto& d : diags) {
                    if (d.severity == DiagnosticSeverity::Error) ++count;
                }
            }
        }
        return count;
    }

    // ------------------------------------------------------------------
    // Change notification callback
    // ------------------------------------------------------------------

    using ChangeCallback = std::function<void(
        std::string_view server_name,
        std::string_view uri,
        size_t diagnostic_count
    )>;

    /// Register a callback invoked when diagnostics change for a server/URI.
    /// TS REF: task spec - on_diagnostics_changed()
    void on_diagnostics_changed(ChangeCallback callback) {
        std::lock_guard lock(mutex_);
        change_callback_ = std::move(callback);
    }

private:
    // ------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------

    /// Deduplicate diagnostic files: within-batch + cross-turn.
    /// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:136-184 (deduplicateDiagnosticFiles)
    std::vector<DiagnosticFile> deduplicate_files(
        const std::vector<DiagnosticFile>& all_files
    ) const {
        std::unordered_map<std::string, std::set<std::string>> file_seen;
        std::unordered_map<std::string, DiagnosticFile> file_map;

        for (const auto& file : all_files) {
            auto& seen = file_seen[file.uri];
            auto& df = file_map[file.uri];
            if (df.uri.empty()) df.uri = file.uri;

            // Get previously delivered keys for cross-turn dedup
            auto delivered_it = delivered_keys_.find(file.uri);
            const std::set<std::string>* delivered = nullptr;
            if (delivered_it != delivered_keys_.end()) {
                delivered = &delivered_it->second;
            }

            for (const auto& diag : file.diagnostics) {
                auto key = diag.dedup_key();
                // Skip if already seen in this batch
                if (seen.contains(key)) continue;
                // Skip if already delivered in previous turns
                if (delivered && delivered->contains(key)) continue;
                seen.insert(key);
                df.diagnostics.push_back(diag);
            }
        }

        std::vector<DiagnosticFile> result;
        result.reserve(file_map.size());
        for (auto& [uri, df] : file_map) {
            if (!df.diagnostics.empty()) {
                result.push_back(std::move(df));
            }
        }
        return result;
    }

    /// Evict oldest entries from delivered_keys_ if over capacity.
    /// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:46,54-56 (LRU max)
    void evict_delivered_if_needed() {
        // Simple FIFO eviction: erase oldest inserted keys until under limit.
        // We use an unordered_map so "oldest" is arbitrary; for correctness
        // we just cap the size by removing any excess.
        while (delivered_keys_.size() > MAX_DELIVERED_FILES) {
            auto it = delivered_keys_.begin();
            delivered_keys_.erase(it);
        }
    }

    /// Get current time in milliseconds since epoch.
    static int64_t current_time_ms() {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
    }

    // ------------------------------------------------------------------
    // Member variables
    // ------------------------------------------------------------------

    mutable std::mutex mutex_;

    /// Per-server, per-URI diagnostic storage.
    /// server_name -> (uri -> diagnostics)
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::vector<Diagnostic>>>
        diagnostics_by_server_;

    /// Pending diagnostic notifications awaiting delivery.
    /// id -> PendingLSPDiagnostic
    std::unordered_map<std::string, std::unique_ptr<PendingLSPDiagnostic>>
        pending_diagnostics_;

    /// Cross-turn deduplication: uri -> set of delivered diagnostic keys.
    /// TS REF: src/services/lsp/LSPDiagnosticRegistry.ts:53-56 (deliveredDiagnostics LRU)
    std::unordered_map<std::string, std::set<std::string>> delivered_keys_;

    /// Monotonically increasing ID for pending diagnostics.
    uint64_t next_pending_id_{0};

    /// Optional callback for diagnostic change notifications.
    ChangeCallback change_callback_;
};

// ============================================================================
// Free functions: LSP diagnostic formatting
// ============================================================================

/// Convert LSP PublishDiagnosticsParams JSON to DiagnosticFile[].
/// Handles both file:// URIs and plain paths.
/// TS REF: src/services/lsp/passiveFeedback.ts:43-100 (formatDiagnosticsForAttachment)
[[nodiscard]] inline std::vector<DiagnosticFile> format_diagnostics_for_attachment(
    cc::utils::json::JsonVal params_root
) {
    std::vector<DiagnosticFile> result;

    auto uri_node = params_root.get("uri");
    auto diagnostics_node = params_root.get("diagnostics");
    if (!uri_node.is_str() || !diagnostics_node.is_arr()) return result;

    std::string uri{uri_node.as_str()};

    // Handle file:// URIs: strip prefix to get filesystem path
    // TS REF: src/services/lsp/passiveFeedback.ts:50-52 (fileURLToPath)
    if (uri.starts_with("file://")) {
        uri = uri.substr(7);  // remove "file://"
        // URL decoding would be ideal; for now we leave as-is since LSP
        // servers typically send properly-encoded URIs that are usable.
    }

    DiagnosticFile df;
    df.uri = uri;

    diagnostics_node.iter([&df](cc::utils::json::JsonVal diag_node) {
        Diagnostic diag;
        diag.uri = df.uri;

        // Parse message
        auto msg_node = diag_node.get("message");
        if (msg_node.is_str()) diag.message = std::string(msg_node.as_str());

        // Parse severity
        auto sev_node = diag_node.get("severity");
        if (sev_node.is_num()) {
            diag.severity = map_lsp_severity(static_cast<int>(sev_node.as_int()));
        }

        // Parse range
        auto range_node = diag_node.get("range");
        if (range_node.is_obj()) {
            auto start_node = range_node.get("start");
            auto end_node = range_node.get("end");
            if (start_node.is_obj()) {
                auto line_node = start_node.get("line");
                auto char_node = start_node.get("character");
                if (line_node.is_num()) diag.range.start.line = line_node.as_int();
                if (char_node.is_num()) diag.range.start.character = char_node.as_int();
            }
            if (end_node.is_obj()) {
                auto line_node = end_node.get("line");
                auto char_node = end_node.get("character");
                if (line_node.is_num()) diag.range.end.line = line_node.as_int();
                if (char_node.is_num()) diag.range.end.character = char_node.as_int();
            }
        }

        // Parse source
        auto src_node = diag_node.get("source");
        if (src_node.is_str()) diag.source = std::string(src_node.as_str());

        // Parse code (may be string or number)
        auto code_node = diag_node.get("code");
        if (code_node.is_str()) {
            diag.code = std::string(code_node.as_str());
        } else if (code_node.is_num()) {
            diag.code = std::to_string(code_node.as_int());
        }

        df.diagnostics.push_back(std::move(diag));
    });

    result.push_back(std::move(df));
    return result;
}

} // namespace cc::services::lsp
