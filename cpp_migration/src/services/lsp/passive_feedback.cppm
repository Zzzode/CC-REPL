// ============================================================================
/// @file passive_feedback.cppm
/// @brief LSP Passive Feedback - collects user acceptance/rejection signals
///        for LSP diagnostic suggestions, computes acceptance rates, and
///        persists feedback to ~/.cc-repl/lsp-passive-feedback.json.
///
/// TS REF: src/services/lsp/passiveFeedback.ts (328 lines)
///   - The TS module primarily handles diagnostic notification handler
///     registration (formatDiagnosticsForAttachment,
///     registerLSPNotificationHandlers). Those formatting helpers live in
///     diagnostic_registry.cppm in the C++ port.
///   - This module adds PassiveFeedbackCollector: tracking whether the user
///     accepted, rejected, or partially-accepted LSP-suggested fixes, with
///     JSON persistence for cross-session learning.
///
/// Architecture:
///   1. record_feedback() stores a PassiveFeedbackItem in memory
///   2. get_acceptance_rate() computes accepted / total per diagnostic code
///   3. export_feedback() / import_feedback() persist to ~/.cc-repl/
///   4. LspHealth metrics (requests/errors/latency) kept from original stub
// ============================================================================
module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module cc.services.lsp.passive_feedback;

import cc.utils.error;
import cc.utils.json;

export namespace cc::services::lsp {

namespace fs = std::filesystem;
using cc::utils::Result;

// ============================================================================
// LSP Health Metrics (kept from original stub API)
// ============================================================================

/// Aggregate LSP health metrics across all servers.
struct LspHealth {
    int requests{0};
    int errors{0};
    std::chrono::milliseconds avg_latency{0};
};

namespace detail {
    inline std::mutex metrics_mutex;
    inline std::atomic<int> total_requests{0};
    inline std::atomic<int> total_errors{0};
    inline std::atomic<long long> total_latency_ms{0};
} // namespace detail

/// Record an LSP event for passive health tracking.
void record_lsp_event(std::string_view event_type,
                      std::map<std::string, std::string> data) {
    detail::total_requests.fetch_add(1);
    if (event_type == "error") {
        detail::total_errors.fetch_add(1);
    }
    if (auto it = data.find("latency_ms"); it != data.end()) {
        detail::total_latency_ms.fetch_add(std::stoll(it->second));
    }
}

/// Get current LSP health metrics.
[[nodiscard]] LspHealth get_lsp_health() {
    int requests = detail::total_requests.load();
    int errors = detail::total_errors.load();
    long long total_ms = detail::total_latency_ms.load();

    auto avg = requests > 0
        ? std::chrono::milliseconds{total_ms / requests}
        : std::chrono::milliseconds{0};

    return LspHealth{requests, errors, avg};
}

/// Reset all LSP health metrics.
void reset_lsp_metrics() {
    detail::total_requests.store(0);
    detail::total_errors.store(0);
    detail::total_latency_ms.store(0);
}

// ============================================================================
// Passive Feedback Types
// ============================================================================

/// Outcome of an LSP diagnostic suggestion being presented to the user.
/// TS REF: task spec (PassiveFeedbackType enum)
enum class PassiveFeedbackType : int {
    Accepted           = 0,  ///< User applied the suggested fix
    Rejected           = 1,  ///< User dismissed the suggestion
    PartiallyAccepted  = 2,  ///< User applied some but not all changes
};

/// Convert PassiveFeedbackType to canonical string name.
[[nodiscard]] inline std::string_view feedback_type_to_string(
    PassiveFeedbackType type
) {
    switch (type) {
        case PassiveFeedbackType::Accepted:          return "Accepted";
        case PassiveFeedbackType::Rejected:          return "Rejected";
        case PassiveFeedbackType::PartiallyAccepted: return "PartiallyAccepted";
    }
    return "Rejected";
}

/// Parse a PassiveFeedbackType from its canonical string name.
[[nodiscard]] inline PassiveFeedbackType feedback_type_from_string(
    std::string_view name
) {
    if (name == "Accepted") return PassiveFeedbackType::Accepted;
    if (name == "PartiallyAccepted") return PassiveFeedbackType::PartiallyAccepted;
    return PassiveFeedbackType::Rejected;  // default
}

// ============================================================================
// Passive Feedback Item
// ============================================================================

/// A single passive feedback record: one user reaction to one diagnostic code.
/// TS REF: task spec (PassiveFeedbackItem struct)
struct PassiveFeedbackItem {
    std::string server_name;       ///< LSP server that produced the diagnostic
    std::string uri;               ///< File URI where the diagnostic appeared
    std::string diagnostic_code;   ///< Diagnostic code (e.g. "TS2345")
    PassiveFeedbackType type{PassiveFeedbackType::Rejected};
    int64_t timestamp_ms{0};       ///< When feedback was recorded (epoch ms)
};

// ============================================================================
// Passive Feedback Collector
// ============================================================================

/// Collects, aggregates, and persists passive feedback from user interactions
/// with LSP diagnostic suggestions. Thread-safe via mutex.
///
/// TS REF: task spec (PassiveFeedbackCollector class)
/// TS REF: src/services/lsp/passiveFeedback.ts - handler registration patterns
class PassiveFeedbackCollector {
public:
    // ------------------------------------------------------------------
    // Recording
    // ------------------------------------------------------------------

    /// Record a feedback item. Timestamp defaults to now.
    void record_feedback(PassiveFeedbackItem item) {
        std::lock_guard lock(mutex_);
        if (item.timestamp_ms == 0) {
            item.timestamp_ms = current_time_ms();
        }
        items_.push_back(std::move(item));
        dirty_ = true;
    }

    /// Convenience: record feedback with explicit fields.
    void record_feedback(
        std::string_view server_name,
        std::string_view uri,
        std::string_view diagnostic_code,
        PassiveFeedbackType type
    ) {
        PassiveFeedbackItem item;
        item.server_name = std::string(server_name);
        item.uri = std::string(uri);
        item.diagnostic_code = std::string(diagnostic_code);
        item.type = type;
        record_feedback(std::move(item));
    }

    // ------------------------------------------------------------------
    // Retrieval
    // ------------------------------------------------------------------

    /// Get all recorded feedback items (copy).
    std::vector<PassiveFeedbackItem> get_feedback() const {
        std::lock_guard lock(mutex_);
        return items_;
    }

    /// Get feedback filtered by diagnostic code.
    std::vector<PassiveFeedbackItem> get_feedback_by_code(
        std::string_view diagnostic_code
    ) const {
        std::lock_guard lock(mutex_);
        std::vector<PassiveFeedbackItem> result;
        for (const auto& item : items_) {
            if (item.diagnostic_code == diagnostic_code) {
                result.push_back(item);
            }
        }
        return result;
    }

    /// Get feedback filtered by server name.
    std::vector<PassiveFeedbackItem> get_feedback_by_server(
        std::string_view server_name
    ) const {
        std::lock_guard lock(mutex_);
        std::vector<PassiveFeedbackItem> result;
        for (const auto& item : items_) {
            if (item.server_name == server_name) {
                result.push_back(item);
            }
        }
        return result;
    }

    // ------------------------------------------------------------------
    // Aggregation
    // ------------------------------------------------------------------

    /// Compute acceptance rate (0.0 - 1.0) for a specific diagnostic code.
    /// Accepted counts as 1.0, PartiallyAccepted as 0.5, Rejected as 0.0.
    /// Returns 0.0 if there are no feedback items for this code.
    double get_acceptance_rate(std::string_view diagnostic_code) const {
        std::lock_guard lock(mutex_);
        double total_weight = 0.0;
        size_t count = 0;
        for (const auto& item : items_) {
            if (item.diagnostic_code != diagnostic_code) continue;
            switch (item.type) {
                case PassiveFeedbackType::Accepted:
                    total_weight += 1.0; break;
                case PassiveFeedbackType::PartiallyAccepted:
                    total_weight += 0.5; break;
                case PassiveFeedbackType::Rejected:
                    break;
            }
            ++count;
        }
        return count > 0 ? total_weight / static_cast<double>(count) : 0.0;
    }

    /// Get overall acceptance rate across all diagnostic codes.
    double get_overall_acceptance_rate() const {
        std::lock_guard lock(mutex_);
        if (items_.empty()) return 0.0;
        double total_weight = 0.0;
        for (const auto& item : items_) {
            switch (item.type) {
                case PassiveFeedbackType::Accepted:
                    total_weight += 1.0; break;
                case PassiveFeedbackType::PartiallyAccepted:
                    total_weight += 0.5; break;
                case PassiveFeedbackType::Rejected:
                    break;
            }
        }
        return total_weight / static_cast<double>(items_.size());
    }

    /// Get total number of feedback items recorded.
    size_t get_feedback_count() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    // ------------------------------------------------------------------
    // Clearing
    // ------------------------------------------------------------------

    /// Clear all feedback (in-memory only; does not delete disk file).
    void clear_feedback() {
        std::lock_guard lock(mutex_);
        items_.clear();
        dirty_ = true;
    }

    // ------------------------------------------------------------------
    // Persistence
    // ------------------------------------------------------------------

    /// Export all feedback to a JSON file.
    /// Default path: ~/.cc-repl/lsp-passive-feedback.json
    /// TS REF: task spec (export_feedback -> persist to ~/.cc-repl/)
    Result<void> export_feedback() const {
        return export_feedback(default_feedback_path());
    }

    /// Export feedback to a specific path.
    Result<void> export_feedback(const fs::path& path) const {
        std::lock_guard lock(mutex_);

        // Ensure parent directory exists
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        // Ignore directory creation errors; write may still succeed

        // Build JSON document
        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();

        auto items_array = doc.array();
        for (const auto& item : items_) {
            auto item_obj = doc.object();
            item_obj.add("serverName", doc.string(item.server_name));
            item_obj.add("uri", doc.string(item.uri));
            item_obj.add("diagnosticCode", doc.string(item.diagnostic_code));
            item_obj.add("type", doc.string(feedback_type_to_string(item.type)));
            item_obj.add("timestampMs", doc.number(item.timestamp_ms));
            items_array.append(item_obj);
        }
        root.add("items", items_array);
        root.add("version", doc.number(static_cast<int64_t>(1)));
        root.add("exportedAtMs", doc.number(current_time_ms()));

        doc.set_root(root);

        // Write to file
        std::ofstream ofs(path);
        if (!ofs) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::io_error,
                "Failed to open feedback file for writing: " + path.string()
            ));
        }
        ofs << doc.to_pretty_string();
        if (!ofs) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::io_error,
                "Failed to write feedback file: " + path.string()
            ));
        }

        dirty_ = false;
        return {};
    }

    /// Import feedback from the default JSON file.
    /// Merges with existing items (does not clear).
    /// TS REF: task spec (import_feedback)
    Result<void> import_feedback() {
        return import_feedback(default_feedback_path());
    }

    /// Import feedback from a specific path.
    Result<void> import_feedback(const fs::path& path) {
        auto parsed = cc::utils::json::parse_file(path);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }

        auto root = parsed->root();
        auto items_node = root.get("items");
        if (!items_node.is_arr()) return {};  // empty or missing = no-op

        std::lock_guard lock(mutex_);
        items_node.iter([this](cc::utils::json::JsonVal item_node) {
            PassiveFeedbackItem item;

            auto server = item_node.get("serverName");
            if (server.is_str()) item.server_name = std::string(server.as_str());

            auto uri = item_node.get("uri");
            if (uri.is_str()) item.uri = std::string(uri.as_str());

            auto code = item_node.get("diagnosticCode");
            if (code.is_str()) item.diagnostic_code = std::string(code.as_str());

            auto type_node = item_node.get("type");
            if (type_node.is_str()) {
                item.type = feedback_type_from_string(type_node.as_str());
            }

            auto ts = item_node.get("timestampMs");
            if (ts.is_num()) item.timestamp_ms = ts.as_int();

            // Skip empty/invalid items
            if (!item.diagnostic_code.empty() || !item.server_name.empty()) {
                items_.push_back(std::move(item));
            }
        });

        dirty_ = true;
        return {};
    }

    // ------------------------------------------------------------------
    // Path helpers
    // ------------------------------------------------------------------

    /// Get the default feedback persistence path.
    /// ~/.cc-repl/lsp-passive-feedback.json
    [[nodiscard]] static fs::path default_feedback_path() {
        if (const char* home = std::getenv("HOME")) {
            return fs::path{home} / ".cc-repl" / "lsp-passive-feedback.json";
        }
        return fs::path{".cc-repl"} / "lsp-passive-feedback.json";
    }

    // ------------------------------------------------------------------
    // Dirty tracking
    // ------------------------------------------------------------------

    /// Whether in-memory state differs from last export/import.
    [[nodiscard]] bool is_dirty() const {
        std::lock_guard lock(mutex_);
        return dirty_;
    }

private:
    /// Get current time in milliseconds since epoch.
    static int64_t current_time_ms() {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
    }

    mutable std::mutex mutex_;
    std::vector<PassiveFeedbackItem> items_;
    mutable bool dirty_{false};
};

// ============================================================================
// Handler Registration Result (matching TS passiveFeedback.ts)
// ============================================================================

/// Result of registering diagnostic notification handlers across all servers.
/// TS REF: src/services/lsp/passiveFeedback.ts:105-114 (HandlerRegistrationResult)
struct DiagnosticHandlerRegistrationResult {
    size_t total_servers{0};
    size_t success_count{0};
    std::vector<std::pair<std::string, std::string>> registration_errors;
    /// server_name -> { consecutive_failure_count, last_error_message }
    std::unordered_map<std::string, std::pair<size_t, std::string>> diagnostic_failures;
};

} // namespace cc::services::lsp
