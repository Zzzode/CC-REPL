// SSE Stream Parser - Complete Server-Sent Events handling for Anthropic API
module;
#include <chrono>
#include <concepts>
#include <cstdlib>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.services.api.streaming;

import cc.utils.json;
import cc.utils.error;
import cc.services.api.errors;

export namespace cc::services::api {

using cc::services::api::errors::ApiErrorDetails;
using cc::services::api::errors::ErrorFactory;
using cc::utils::Result;

// =========================================================================
// Stream Event Types
// =========================================================================

enum class StreamEventType {
    MessageStart,
    MessageDelta,
    MessageStop,
    ContentBlockStart,
    ContentBlockDelta,
    ContentBlockStop,
    Ping,
    Error,
    Other
};

// =========================================================================
// Content Block Types for Streaming
// =========================================================================

enum class StreamContentBlockType {
    Text,
    ToolUse,
    Thinking,
    Unknown
};

// =========================================================================
// Stream Content Delta
// =========================================================================

struct StreamContentDelta {
    StreamContentBlockType type = StreamContentBlockType::Text;
    std::string text;
    std::string partial_json;  // For tool use inputs
    std::string thinking;
};

// =========================================================================
// Stream Token Usage
// =========================================================================

struct StreamTokenUsage {
    std::optional<int> output_tokens;
};

// =========================================================================
// Stream Message Delta
// =========================================================================

struct StreamMessageDelta {
    std::optional<std::string> stop_reason;
    std::optional<std::string> stop_sequence;
    StreamTokenUsage usage;
};

// =========================================================================
// Stream Event
// =========================================================================

struct StreamEvent {
    StreamEventType type = StreamEventType::Other;
    std::string event_id;

    // MessageStart fields
    std::string message_id;
    std::string model;
    std::string role;

    // ContentBlockStart fields
    int block_index = -1;
    StreamContentBlockType block_type = StreamContentBlockType::Text;
    std::string block_text;
    std::string tool_name;
    std::string tool_use_id;

    // ContentBlockDelta fields
    StreamContentDelta delta;

    // MessageDelta fields
    StreamMessageDelta message_delta;

    // Error fields
    ApiErrorDetails error;
};

// =========================================================================
// SSE Buffer
// =========================================================================

class SseBuffer {
public:
    void append(std::string_view data) {
        buffer_ += data;
    }

    [[nodiscard]] std::optional<std::pair<std::string, std::string>> next_event() {
        std::string event_type;
        std::string event_data;
        bool event_complete = false;

        while (!event_complete) {
            auto newline_pos = buffer_.find('\n');
            if (newline_pos == std::string::npos) {
                break;  // Incomplete line
            }

            auto line = buffer_.substr(0, newline_pos);
            buffer_.erase(0, newline_pos + 1);

            // Remove trailing \r if present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.empty()) {
                // Empty line = end of event
                event_complete = !event_type.empty() || !event_data.empty();
                break;
            }

            // Parse field
            if (line.starts_with("event:")) {
                event_type = strip_prefix(line, "event:");
            } else if (line.starts_with("data:")) {
                if (!event_data.empty()) {
                    event_data += '\n';
                }
                event_data += strip_prefix(line, "data:");
            } else if (line.starts_with("id:")) {
                last_event_id_ = strip_prefix(line, "id:");
            } else if (line.starts_with("retry:")) {
                try {
                    retry_ms_ = std::stoi(std::string(strip_prefix(line, "retry:")));
                } catch (...) {}
            }
            // Ignore comments (lines starting with :)
        }

        if (event_complete) {
            return std::make_pair(std::move(event_type), std::move(event_data));
        }
        return std::nullopt;
    }

    [[nodiscard]] const std::string& last_event_id() const { return last_event_id_; }
    [[nodiscard]] int retry_ms() const { return retry_ms_; }
    void clear() { buffer_.clear(); }

private:
    [[nodiscard]] static std::string_view strip_prefix(std::string_view line, std::string_view prefix) {
        auto value = line.substr(prefix.size());
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
        return value;
    }

    std::string buffer_;
    std::string last_event_id_;
    int retry_ms_ = 3000;
};

// =========================================================================
// Stream Event Parser
// =========================================================================

class StreamEventParser {
public:
    [[nodiscard]] static Result<StreamEvent> parse(const std::string& event_type,
                                                    const std::string& event_data) {
        StreamEvent event;
        event.event_id = "";  // Would be set from SSE buffer

        if (event_type == "message_start") {
            return parse_message_start(event_data, event);
        } else if (event_type == "content_block_start") {
            return parse_content_block_start(event_data, event);
        } else if (event_type == "content_block_delta") {
            return parse_content_block_delta(event_data, event);
        } else if (event_type == "content_block_stop") {
            return parse_content_block_stop(event_data, event);
        } else if (event_type == "message_delta") {
            return parse_message_delta(event_data, event);
        } else if (event_type == "message_stop") {
            event.type = StreamEventType::MessageStop;
            return event;
        } else if (event_type == "ping") {
            event.type = StreamEventType::Ping;
            return event;
        } else if (event_type == "error") {
            return parse_error(event_data, event);
        }

        event.type = StreamEventType::Other;
        return event;
    }

private:
    [[nodiscard]] static Result<StreamEvent> parse_message_start(const std::string& data,
                                                                 StreamEvent& event) {
        event.type = StreamEventType::MessageStart;
        auto doc_result = cc::utils::json::parse(data);
        if (!doc_result) {
            return std::unexpected(doc_result.error());
        }

        auto root = doc_result->root();
        auto message = root.get("message");

        event.message_id = std::string(message.get("id").as_str());
        event.model = std::string(message.get("model").as_str());
        event.role = std::string(message.get("role").as_str());

        return event;
    }

    [[nodiscard]] static Result<StreamEvent> parse_content_block_start(const std::string& data,
                                                                       StreamEvent& event) {
        event.type = StreamEventType::ContentBlockStart;
        auto doc_result = cc::utils::json::parse(data);
        if (!doc_result) {
            return std::unexpected(doc_result.error());
        }

        auto root = doc_result->root();
        event.block_index = static_cast<int>(root.get("index").as_int());

        auto block = root.get("content_block");
        auto type_str = block.get("type").as_str();

        if (type_str == "text") {
            event.block_type = StreamContentBlockType::Text;
            event.block_text = std::string(block.get("text").as_str());
        } else if (type_str == "tool_use") {
            event.block_type = StreamContentBlockType::ToolUse;
            event.tool_use_id = std::string(block.get("id").as_str());
            event.tool_name = std::string(block.get("name").as_str());
        } else if (type_str == "thinking") {
            event.block_type = StreamContentBlockType::Thinking;
            // thinking fields
        }

        return event;
    }

    [[nodiscard]] static Result<StreamEvent> parse_content_block_delta(const std::string& data,
                                                                       StreamEvent& event) {
        event.type = StreamEventType::ContentBlockDelta;
        auto doc_result = cc::utils::json::parse(data);
        if (!doc_result) {
            return std::unexpected(doc_result.error());
        }

        auto root = doc_result->root();
        event.block_index = static_cast<int>(root.get("index").as_int());

        auto delta = root.get("delta");
        auto delta_type = delta.get("type").as_str();

        if (delta_type == "text_delta") {
            event.delta.type = StreamContentBlockType::Text;
            event.delta.text = std::string(delta.get("text").as_str());
        } else if (delta_type == "input_json_delta") {
            event.delta.type = StreamContentBlockType::ToolUse;
            event.delta.partial_json = std::string(delta.get("partial_json").as_str());
        } else if (delta_type == "thinking_delta") {
            event.delta.type = StreamContentBlockType::Thinking;
            event.delta.thinking = std::string(delta.get("thinking").as_str());
        }

        return event;
    }

    [[nodiscard]] static Result<StreamEvent> parse_content_block_stop(const std::string& data,
                                                                      StreamEvent& event) {
        event.type = StreamEventType::ContentBlockStop;
        auto doc_result = cc::utils::json::parse(data);
        if (!doc_result) {
            return std::unexpected(doc_result.error());
        }

        auto root = doc_result->root();
        event.block_index = static_cast<int>(root.get("index").as_int());

        return event;
    }

    [[nodiscard]] static Result<StreamEvent> parse_message_delta(const std::string& data,
                                                                 StreamEvent& event) {
        event.type = StreamEventType::MessageDelta;
        auto doc_result = cc::utils::json::parse(data);
        if (!doc_result) {
            return std::unexpected(doc_result.error());
        }

        auto root = doc_result->root();
        auto delta = root.get("delta");

        auto stop_reason = delta.get("stop_reason");
        if (stop_reason.valid()) {
            event.message_delta.stop_reason = std::string(stop_reason.as_str());
        }

        auto stop_seq = delta.get("stop_sequence");
        if (stop_seq.valid()) {
            event.message_delta.stop_sequence = std::string(stop_seq.as_str());
        }

        auto usage = root.get("usage");
        if (usage.valid()) {
            event.message_delta.usage.output_tokens =
                static_cast<int>(usage.get("output_tokens").as_int());
        }

        return event;
    }

    [[nodiscard]] static Result<StreamEvent> parse_error(const std::string& data,
                                                         StreamEvent& event) {
        event.type = StreamEventType::Error;
        auto doc_result = cc::utils::json::parse(data);
        if (!doc_result) {
            return std::unexpected(doc_result.error());
        }

        auto root = doc_result->root();
        auto error = root.get("error");

        event.error = ApiErrorDetails{};
        event.error.error_type = std::string(error.get("type").as_str());
        event.error.error_message = std::string(error.get("message").as_str());

        return event;
    }
};

// =========================================================================
// Connection State
// =========================================================================

enum class ConnectionState {
    Idle,
    Connecting,
    Connected,
    Reconnecting,
    Disconnected,
    Finished,
    Error
};

// =========================================================================
// Stream Configuration
// =========================================================================

struct StreamConfig {
    std::chrono::milliseconds connect_timeout{30000};
    std::chrono::milliseconds read_timeout{120000};
    std::chrono::milliseconds heartbeat_interval{30000};
    int max_reconnect_attempts = 5;
    std::chrono::milliseconds base_reconnect_delay{1000};
    std::chrono::milliseconds max_reconnect_delay{30000};
    bool enable_reconnect = true;
};

// =========================================================================
// Stream Statistics
// =========================================================================

struct StreamStatistics {
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
    int total_events = 0;
    int text_blocks = 0;
    int tool_use_blocks = 0;
    int reconnects = 0;
    int input_tokens = 0;
    int output_tokens = 0;
    std::chrono::milliseconds time_to_first_token{0};

    [[nodiscard]] std::chrono::milliseconds total_duration() const {
        if (end_time == std::chrono::steady_clock::time_point{}) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time);
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    }
};

// =========================================================================
// Accumulated Stream State
// =========================================================================

struct StreamState {
    std::string message_id;
    std::string model;
    std::string full_text;
    std::vector<std::pair<std::string, std::string>> tool_uses;  // id -> json
    std::string stop_reason;
    int input_tokens = 0;
    int output_tokens = 0;
    bool is_complete = false;
    bool has_error = false;
};

// =========================================================================
// Main Stream Parser
// =========================================================================

class StreamParser {
    struct SharedState {
        explicit SharedState(StreamConfig cfg) : config(std::move(cfg)) {}

        StreamConfig config;
        ConnectionState connection = ConnectionState::Idle;
        SseBuffer buffer;
        StreamStatistics statistics;
        StreamState stream_state;
        std::optional<ApiErrorDetails> error_details;
        int reconnect_attempts = 0;
        mutable std::mutex mutex;
    };

public:
    explicit StreamParser(StreamConfig config = {})
        : state_(std::make_shared<SharedState>(std::move(config))) {}

    // Factory method for parser setup; AnthropicClient owns the HTTP transfer.
    static std::optional<StreamParser> create(
        const std::string& url,
        const std::string& body,
        const std::vector<std::string>& headers,
        std::chrono::milliseconds timeout) {
        StreamParser parser;
        {
            std::scoped_lock lock(parser.state_->mutex);
            parser.state_->config.connect_timeout = timeout;
        }
        parser.feed({});
        return parser;
    }

    // Feed raw data
    void feed(std::string_view data) {
        std::scoped_lock lock(state_->mutex);
        state_->buffer.append(data);
    }

    // Get next parsed event
    [[nodiscard]] Result<std::optional<StreamEvent>> next_event() {
        std::scoped_lock lock(state_->mutex);
        while (true) {
            auto raw_event = state_->buffer.next_event();
            if (!raw_event) {
                return std::nullopt;
            }

            auto& [event_type, event_data] = *raw_event;
            auto parse_result = StreamEventParser::parse(event_type, event_data);

            if (!parse_result) {
                return std::unexpected(parse_result.error());
            }

            update_statistics_locked(*state_, *parse_result);
            update_stream_state_locked(*state_, *parse_result);

            return std::move(*parse_result);
        }
    }

    // Process all available events with callback
    template<typename Callback>
    requires std::invocable<Callback, const StreamEvent&>
    void process_events(Callback&& callback) {
        while (auto event = next_event()) {
            if (!event->has_value()) break;
            callback(**event);
            if ((*event)->type == StreamEventType::MessageStop ||
                (*event)->type == StreamEventType::Error) {
                break;
            }
        }
    }

    // State access
    [[nodiscard]] ConnectionState connection_state() const {
        std::scoped_lock lock(state_->mutex);
        return state_->connection;
    }
    [[nodiscard]] bool is_connected() const { return connection_state() == ConnectionState::Connected; }
    [[nodiscard]] bool is_finished() const {
        const auto state = connection_state();
        return state == ConnectionState::Finished || state == ConnectionState::Error;
    }
    [[nodiscard]] bool has_error() const {
        std::scoped_lock lock(state_->mutex);
        return state_->stream_state.has_error;
    }
    [[nodiscard]] std::optional<ApiErrorDetails> error_details() const {
        std::scoped_lock lock(state_->mutex);
        return state_->error_details;
    }

    // Statistics
    [[nodiscard]] StreamStatistics statistics() const {
        std::scoped_lock lock(state_->mutex);
        return state_->statistics;
    }

    // Accumulated state
    [[nodiscard]] StreamState stream_state() const {
        std::scoped_lock lock(state_->mutex);
        return state_->stream_state;
    }
    [[nodiscard]] std::string full_text() const {
        std::scoped_lock lock(state_->mutex);
        return state_->stream_state.full_text;
    }

    // Reconnection
    [[nodiscard]] bool can_reconnect() const {
        std::scoped_lock lock(state_->mutex);
        return state_->config.enable_reconnect &&
               state_->reconnect_attempts < state_->config.max_reconnect_attempts;
    }

    [[nodiscard]] std::chrono::milliseconds next_reconnect_delay() const {
        std::scoped_lock lock(state_->mutex);
        // Exponential backoff with jitter
        auto delay = state_->config.base_reconnect_delay * (1 << state_->reconnect_attempts);
        if (delay > state_->config.max_reconnect_delay) {
            delay = state_->config.max_reconnect_delay;
        }
        // Add jitter (±20%)
        auto jitter = static_cast<double>(std::rand()) / RAND_MAX * 0.4 - 0.2;
        return std::chrono::milliseconds(static_cast<long long>(delay.count() * (1 + jitter)));
    }

    void reset_reconnect_attempts() {
        std::scoped_lock lock(state_->mutex);
        state_->reconnect_attempts = 0;
    }

    void mark_reconnect() {
        std::scoped_lock lock(state_->mutex);
        ++state_->reconnect_attempts;
        ++state_->statistics.reconnects;
    }

    // Lifecycle
    void start() {
        std::scoped_lock lock(state_->mutex);
        state_->connection = ConnectionState::Connected;
        state_->statistics.start_time = std::chrono::steady_clock::now();
    }

    void finish() {
        std::scoped_lock lock(state_->mutex);
        state_->connection = ConnectionState::Finished;
        state_->statistics.end_time = std::chrono::steady_clock::now();
    }

    void set_error(const ApiErrorDetails& error) {
        std::scoped_lock lock(state_->mutex);
        state_->connection = ConnectionState::Error;
        state_->stream_state.has_error = true;
        state_->error_details = error;
        state_->statistics.end_time = std::chrono::steady_clock::now();
    }

    void clear() {
        std::scoped_lock lock(state_->mutex);
        state_->buffer.clear();
        state_->stream_state = StreamState{};
        state_->error_details = std::nullopt;
        state_->statistics = StreamStatistics{};
        state_->reconnect_attempts = 0;
        state_->connection = ConnectionState::Idle;
    }

    [[nodiscard]] std::string last_event_id() const {
        std::scoped_lock lock(state_->mutex);
        return state_->buffer.last_event_id();
    }

private:
    static void update_statistics_locked(SharedState& state, const StreamEvent& event) {
        ++state.statistics.total_events;

        if (state.statistics.total_events == 1) {
            state.statistics.time_to_first_token =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - state.statistics.start_time);
        }

        switch (event.type) {
            case StreamEventType::ContentBlockStart:
                if (event.block_type == StreamContentBlockType::Text) {
                    ++state.statistics.text_blocks;
                } else if (event.block_type == StreamContentBlockType::ToolUse) {
                    ++state.statistics.tool_use_blocks;
                }
                break;

            case StreamEventType::MessageDelta:
                if (event.message_delta.usage.output_tokens) {
                    state.statistics.output_tokens = *event.message_delta.usage.output_tokens;
                }
                break;

            default:
                break;
        }
    }

    static void update_stream_state_locked(SharedState& state, const StreamEvent& event) {
        switch (event.type) {
            case StreamEventType::MessageStart:
                state.stream_state.message_id = event.message_id;
                state.stream_state.model = event.model;
                break;

            case StreamEventType::ContentBlockDelta:
                if (event.delta.type == StreamContentBlockType::Text) {
                    state.stream_state.full_text += event.delta.text;
                }
                break;

            case StreamEventType::MessageDelta:
                if (event.message_delta.stop_reason) {
                    state.stream_state.stop_reason = *event.message_delta.stop_reason;
                }
                if (event.message_delta.usage.output_tokens) {
                    state.stream_state.output_tokens = *event.message_delta.usage.output_tokens;
                }
                break;

            case StreamEventType::MessageStop:
                state.stream_state.is_complete = true;
                break;

            case StreamEventType::Error:
                state.stream_state.has_error = true;
                break;

            default:
                break;
        }
    }

    std::shared_ptr<SharedState> state_;
};

// =========================================================================
// Stream Event Type to String
// =========================================================================

[[nodiscard]] inline std::string_view stream_event_type_to_string(StreamEventType type) {
    switch (type) {
        case StreamEventType::MessageStart: return "message_start";
        case StreamEventType::MessageDelta: return "message_delta";
        case StreamEventType::MessageStop: return "message_stop";
        case StreamEventType::ContentBlockStart: return "content_block_start";
        case StreamEventType::ContentBlockDelta: return "content_block_delta";
        case StreamEventType::ContentBlockStop: return "content_block_stop";
        case StreamEventType::Ping: return "ping";
        case StreamEventType::Error: return "error";
        case StreamEventType::Other: return "other";
    }
    return "unknown";
}

} // namespace cc::services::api
