module;
#include <string>
#include <string_view>
#include <map>
#include <iostream>
#include <sstream>
#include <chrono>
#include <mutex>

export module cc.cli.structured_io;

export namespace cc::cli {

// StructuredIO provides JSON/NDJSON output mode for machine-readable output
class StructuredIO {
public:
    StructuredIO() = default;

    // Enable or disable structured output mode
    void set_structured_mode(bool enabled) {
        structured_mode_ = enabled;
    }

    // Emit a typed event with associated data
    void emit_event(std::string_view type, std::map<std::string, std::string> data) {
        if (!structured_mode_) return;

        std::lock_guard lock(mutex_);

        std::ostringstream oss;
        oss << "{\"type\":\"" << escape_json(type) << "\"";
        oss << ",\"timestamp\":" << get_timestamp_ms();
        oss << ",\"data\":{";

        bool first = true;
        for (const auto& [key, value] : data) {
            if (!first) oss << ",";
            oss << "\"" << escape_json(key) << "\":\"" << escape_json(value) << "\"";
            first = false;
        }

        oss << "}}";

        // Write as NDJSON line to stdout
        std::cout << oss.str() << '\n' << std::flush;
    }

    // Emit a result (final output of a command)
    void emit_result(std::string_view content) {
        if (!structured_mode_) {
            std::cout << content << '\n';
            return;
        }

        std::lock_guard lock(mutex_);

        std::ostringstream oss;
        oss << "{\"type\":\"result\"";
        oss << ",\"timestamp\":" << get_timestamp_ms();
        oss << ",\"content\":\"" << escape_json(content) << "\"}";

        std::cout << oss.str() << '\n' << std::flush;
    }

    // Emit an error in structured format
    void emit_error(std::string_view error) {
        if (!structured_mode_) {
            std::cerr << "Error: " << error << '\n';
            return;
        }

        std::lock_guard lock(mutex_);

        std::ostringstream oss;
        oss << "{\"type\":\"error\"";
        oss << ",\"timestamp\":" << get_timestamp_ms();
        oss << ",\"error\":\"" << escape_json(error) << "\"}";

        std::cout << oss.str() << '\n' << std::flush;
    }

    // Check if structured output mode is active
    bool is_structured_mode() const {
        return structured_mode_;
    }

private:
    // Escape special characters for JSON string values
    static std::string escape_json(std::string_view input) {
        std::string result;
        result.reserve(input.size());

        for (char c : input) {
            switch (c) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        // Control character: encode as \u00XX
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        result += buf;
                    } else {
                        result += c;
                    }
                    break;
            }
        }
        return result;
    }

    // Get current timestamp in milliseconds since epoch
    static long long get_timestamp_ms() {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
    }

    bool structured_mode_{false};
    std::mutex mutex_;
};

} // namespace cc::cli
