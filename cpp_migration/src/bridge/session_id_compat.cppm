module;
#include <string>
#include <string_view>
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cctype>

export module cc.bridge.session_id_compat;

export namespace cc::bridge {

// Check if a session ID uses the legacy UUID format.
bool is_legacy_session_id(std::string_view id);

// Convert a legacy UUID-format session ID to the new format.
std::string convert_legacy_id(std::string_view legacy);

// Normalize a session ID to the current format (lowercase, no dashes variant)
std::string normalize_session_id(std::string_view id) {
    std::string normalized(id);

    // Remove any surrounding whitespace
    while (!normalized.empty() && (normalized.front() == ' ' || normalized.front() == '\t')) {
        normalized.erase(normalized.begin());
    }
    while (!normalized.empty() && (normalized.back() == ' ' || normalized.back() == '\t')) {
        normalized.pop_back();
    }

    // Convert to lowercase
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // If it's a legacy format, convert it
    if (is_legacy_session_id(normalized)) {
        return convert_legacy_id(normalized);
    }

    return normalized;
}

// Check if a session ID uses the legacy format
// Legacy format: 8-4-4-4-12 UUID with uppercase (e.g., "A1B2C3D4-E5F6-7890-ABCD-EF1234567890")
bool is_legacy_session_id(std::string_view id) {
    if (id.size() != 36) return false;

    // Check UUID format (8-4-4-4-12 with dashes)
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else {
            if (!std::isxdigit(static_cast<unsigned char>(id[i]))) return false;
        }
    }

    // Legacy IDs had uppercase hex — but since we already lowercased, check prefix
    // Actually legacy detection: if it has dashes, it's legacy format
    return true;
}

// Convert a legacy UUID-format session ID to the new format (no dashes, with prefix)
std::string convert_legacy_id(std::string_view legacy) {
    std::string result = "ses_";

    // Remove dashes and append
    for (char c : legacy) {
        if (c != '-') {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    return result;
}

// Generate a new session ID in the current format
std::string generate_session_id() {
    // Current format: "ses_" + 32 random hex characters
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    std::ostringstream oss;
    oss << "ses_" << std::hex << std::setfill('0');
    oss << std::setw(16) << dist(gen);
    oss << std::setw(16) << dist(gen);

    return oss.str();
}

} // namespace cc::bridge
