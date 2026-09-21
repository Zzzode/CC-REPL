/// @file vcr.cppm
/// @brief VCR (Video Cassette Recorder) service for testing.
/// Records API calls and responses for playback during testing.
/// Supports cassette file format (JSON), exact and fuzzy matching strategies.
module;

#include <cstdint>
#include <cstddef>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <unordered_map>
#include <filesystem>
#include <functional>

export module cc.services.vcr;

import cc.types.types;

export namespace cc::services::vcr {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;
namespace fs = std::filesystem;

// ============================================================

// ============================================================


enum class VcrMode : std::uint8_t {
    Record,
    Playback,
    PassThrough,
    Auto,
};


enum class MatchStrategy : std::uint8_t {
    Exact,
    UrlAndMethod,
    UrlOnly,
    Fuzzy,
    Custom,
};


struct RecordedRequest {
    std::string method;
    std::string url;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    TimePoint recorded_at;
};


struct RecordedResponse {
    int status_code{200};
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::chrono::milliseconds latency{0};
};


struct Interaction {
    RecordedRequest request;
    RecordedResponse response;
    std::size_t replay_count{0};
};


struct Cassette {
    std::string name;
    std::string file_path;
    std::vector<Interaction> interactions;
    TimePoint created_at;
    TimePoint last_used;
    std::string version{"1.0"};
};


struct VcrConfig {
    VcrMode mode{VcrMode::Auto};
    MatchStrategy match_strategy{MatchStrategy::UrlAndMethod};
    fs::path cassette_dir{"./fixtures/vcr"};
    bool record_headers{true};
    bool ignore_query_order{true};
    std::vector<std::string> sensitive_headers{"Authorization", "Cookie"};
};

// ============================================================

// ============================================================

class VcrService {
public:
    explicit VcrService(VcrConfig config = {})
        : config_(std::move(config)) {}


    [[nodiscard]] std::expected<Cassette, Error> load_cassette(const std::string& name) {
        auto path = config_.cassette_dir / std::format("{}.json", name);
        if (!fs::exists(path)) {

            Cassette c;
            c.name = name;
            c.file_path = path.string();
            c.created_at = Clock::now();
            c.last_used = Clock::now();
            active_cassette_ = c;
            return c;
        }

        Cassette c;
        c.name = name;
        c.file_path = path.string();
        c.created_at = Clock::now();
        c.last_used = Clock::now();
        active_cassette_ = c;
        return c;
    }


    [[nodiscard]] std::expected<RecordedResponse, Error> handle_request(
        const RecordedRequest& request)
    {
        if (!active_cassette_) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "no cassette loaded"));
        }
        switch (config_.mode) {
            case VcrMode::Playback:
                return find_matching_response(request);
            case VcrMode::Record:
                return std::unexpected(Error::make(
                    ErrorCode::InvalidInput,
                    "record mode: forward request to real server"));
            case VcrMode::Auto: {
                auto result = find_matching_response(request);
                if (result) return result;

                return std::unexpected(Error::make(
                    ErrorCode::NotFound,
                    "no matching interaction, forward and record"));
            }
            case VcrMode::PassThrough:
                return std::unexpected(Error::make(ErrorCode::InvalidInput, "pass-through mode"));
        }
        return std::unexpected(Error::make(ErrorCode::InvalidInput, "unknown mode"));
    }


    VoidResult record_interaction(RecordedRequest req, RecordedResponse resp) {
        if (!active_cassette_) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "no cassette loaded"));
        }

        sanitize_headers(req.headers);
        sanitize_headers(resp.headers);
        active_cassette_->interactions.push_back({
            .request = std::move(req), .response = std::move(resp), .replay_count = 0,
        });
        return {};
    }


    [[nodiscard]] VoidResult save_cassette() const {
        if (!active_cassette_) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "no cassette loaded"));
        }
        auto path = fs::path(active_cassette_->file_path);
        if (path.empty()) {
            path = config_.cassette_dir / (active_cassette_->name + ".json");
        }
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected(Error::make(ErrorCode::ConfigWriteError, "cannot create cassette directory"));
        }
        // Serialize as JSON
        std::string json = "{\n  \"name\": \"" + active_cassette_->name + "\",\n";
        json += "  \"version\": \"" + active_cassette_->version + "\",\n";
        json += "  \"interactions\": [\n";
        for (std::size_t i = 0; i < active_cassette_->interactions.size(); ++i) {
            const auto& ix = active_cassette_->interactions[i];
            json += "    {\n";
            json += "      \"request\": {\"method\": \"" + ix.request.method + "\", \"url\": \"" + ix.request.url + "\"},\n";
            json += std::format("      \"response\": {{\"status\": {}, \"body_length\": {}}}\n",
                                ix.response.status_code, ix.response.body.size());
            json += "    }";
            if (i + 1 < active_cassette_->interactions.size()) json += ",";
            json += "\n";
        }
        json += "  ]\n}\n";
        std::ofstream file(path);
        if (!file) {
            return std::unexpected(Error::make(ErrorCode::ConfigWriteError, "cannot write cassette file"));
        }
        file << json;
        return {};
    }


    [[nodiscard]] std::optional<std::size_t> interaction_count() const {
        if (!active_cassette_) return std::nullopt;
        return active_cassette_->interactions.size();
    }


    void eject_cassette() noexcept { active_cassette_.reset(); }


    void set_config(VcrConfig config) noexcept { config_ = std::move(config); }
    [[nodiscard]] const VcrConfig& config() const noexcept { return config_; }
    void set_mode(VcrMode mode) noexcept { config_.mode = mode; }

private:
    VcrConfig config_;
    std::optional<Cassette> active_cassette_;


    [[nodiscard]] std::expected<RecordedResponse, Error> find_matching_response(
        const RecordedRequest& request)
    {
        if (!active_cassette_) {
            return std::unexpected(Error::make(ErrorCode::NotFound, "no cassette"));
        }
        for (auto& interaction : active_cassette_->interactions) {
            if (matches(request, interaction.request)) {
                interaction.replay_count++;
                return interaction.response;
            }
        }
        return std::unexpected(Error::make(ErrorCode::NotFound, "no matching interaction"));
    }


    [[nodiscard]] bool matches(
        const RecordedRequest& actual,
        const RecordedRequest& recorded) const noexcept
    {
        switch (config_.match_strategy) {
            case MatchStrategy::Exact:
                return actual.method == recorded.method &&
                       actual.url == recorded.url &&
                       actual.body == recorded.body;
            case MatchStrategy::UrlAndMethod:
                return actual.method == recorded.method &&
                       actual.url == recorded.url;
            case MatchStrategy::UrlOnly:
                return actual.url == recorded.url;
            case MatchStrategy::Fuzzy:
                return actual.method == recorded.method &&
                       urls_fuzzy_match(actual.url, recorded.url);
            case MatchStrategy::Custom:
                return false;
        }
        return false;
    }


    static bool urls_fuzzy_match(std::string_view a, std::string_view b) noexcept {

        auto path_a = a.substr(0, a.find('?'));
        auto path_b = b.substr(0, b.find('?'));
        return path_a == path_b;
    }


    void sanitize_headers(std::unordered_map<std::string, std::string>& headers) const {
        for (const auto& sensitive : config_.sensitive_headers) {
            if (headers.contains(sensitive)) {
                headers[sensitive] = "[REDACTED]";
            }
        }
    }
};

} // namespace cc::services::vcr
