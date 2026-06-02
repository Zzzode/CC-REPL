module;

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.services.telemetry;


export namespace cc::services {

// 遥测事件类型
enum class TelemetryEventType {
    session_start, session_end, tool_use, command_use, error_occurred,
    model_switch, compact_triggered, api_call, feature_flag_check,
    performance_metric, user_feedback
};

// 遥测事件
struct TelemetryEvent {
    std::string id;
    TelemetryEventType type;
    std::string name;
    std::unordered_map<std::string, std::string> properties;
    std::unordered_map<std::string, double> metrics;
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> session_id;
    std::optional<std::string> trace_id;  // 分布式追踪
};

// Span (性能追踪单元)
struct Span {
    std::string id;
    std::string name;
    std::string trace_id;
    std::optional<std::string> parent_span_id;
    std::chrono::steady_clock::time_point start_time;
    std::optional<std::chrono::steady_clock::time_point> end_time;
    std::unordered_map<std::string, std::string> attributes;
    enum class Status { ok, error, cancelled } status{Status::ok};
    
    [[nodiscard]] auto duration_ms() const -> double {
        auto end = end_time.value_or(std::chrono::steady_clock::now());
        return std::chrono::duration<double, std::milli>(end - start_time).count();
    }
};

// 遥测配置
struct TelemetryConfig {
    bool enabled{true};
    bool send_to_server{false};     // 是否上报到远程
    std::string endpoint;            // 上报端点
    std::chrono::seconds flush_interval{60};
    size_t max_buffer_size{1000};
    bool include_system_info{false};
    bool include_performance{true};
    std::vector<std::string> blocked_events;  // 不上报的事件类型
};

// RAII Span 守卫
class SpanGuard {
    Span span_;
    std::function<void(Span)> on_end_;
public:
    SpanGuard(std::string name, std::string trace_id, std::function<void(Span)> on_end)
        : on_end_(std::move(on_end)) {
        span_.name = std::move(name);
        span_.trace_id = std::move(trace_id);
        span_.id = generate_id();
        span_.start_time = std::chrono::steady_clock::now();
    }
    ~SpanGuard() {
        span_.end_time = std::chrono::steady_clock::now();
        if (on_end_) on_end_(span_);
    }
    SpanGuard(const SpanGuard&) = delete;
    SpanGuard& operator=(const SpanGuard&) = delete;
    
    void set_attribute(std::string key, std::string value) { span_.attributes[std::move(key)] = std::move(value); }
    void set_error() { span_.status = Span::Status::error; }
    [[nodiscard]] auto id() const -> std::string_view { return span_.id; }
    
private:
    static auto generate_id() -> std::string {
        static std::atomic<uint64_t> counter{0};
        return "span_" + std::to_string(counter.fetch_add(1));
    }
};

// ─── 遥测管理器 ────────────────────────────────────────────

class TelemetryManager {
    TelemetryConfig config_;
    std::vector<TelemetryEvent> buffer_;
    std::vector<Span> spans_;
    std::string current_session_id_;
    std::atomic<uint64_t> event_counter_{0};

public:
    explicit TelemetryManager(TelemetryConfig config = {}) : config_(std::move(config)) {}

    // 记录事件
    void track(TelemetryEventType type, std::string name,
               std::unordered_map<std::string, std::string> props = {},
               std::unordered_map<std::string, double> metrics = {}) {
        if (!config_.enabled) return;
        
        TelemetryEvent event{
            .id = "evt_" + std::to_string(event_counter_.fetch_add(1)),
            .type = type, .name = std::move(name),
            .properties = std::move(props), .metrics = std::move(metrics),
            .timestamp = std::chrono::system_clock::now(),
            .session_id = current_session_id_
        };
        buffer_.push_back(std::move(event));
        
        if (buffer_.size() >= config_.max_buffer_size) flush();
    }

    // 快捷方法
    void track_tool_use(std::string_view tool, double duration_ms) {
        track(TelemetryEventType::tool_use, std::string(tool), {}, {{"duration_ms", duration_ms}});
    }
    void track_command(std::string_view cmd) {
        track(TelemetryEventType::command_use, std::string(cmd));
    }
    void track_error(std::string_view error, std::string_view context = "") {
        track(TelemetryEventType::error_occurred, std::string(error),
              {{"context", std::string(context)}});
    }
    void track_api_call(std::string_view model, double latency_ms, size_t tokens) {
        track(TelemetryEventType::api_call, std::string(model),
              {}, {{"latency_ms", latency_ms}, {"tokens", static_cast<double>(tokens)}});
    }

    // 开始 Span (返回 RAII 守卫)
    [[nodiscard]] auto start_span(std::string name, std::string trace_id = "") -> SpanGuard {
        if (trace_id.empty()) trace_id = "trace_" + std::to_string(event_counter_.fetch_add(1));
        return SpanGuard(std::move(name), std::move(trace_id),
            [this](Span s) { spans_.push_back(std::move(s)); });
    }

    // 刷新缓冲区到存储/远程
    void flush() {
        if (buffer_.empty()) return;
        if (config_.send_to_server) {
            last_flush_endpoint_ = config_.endpoint;
            last_flush_count_ = buffer_.size();
        }
        // 本地: 写入日志文件
        buffer_.clear();
    }

    // 会话管理
    void set_session(std::string id) { current_session_id_ = std::move(id); }
    
    // 获取统计
    [[nodiscard]] auto get_event_count() const -> size_t { return event_counter_.load(); }
    [[nodiscard]] auto get_buffer_size() const -> size_t { return buffer_.size(); }
    [[nodiscard]] auto get_spans() const -> const std::vector<Span>& { return spans_; }
    [[nodiscard]] auto get_last_flush_endpoint() const -> const std::string& { return last_flush_endpoint_; }
    [[nodiscard]] auto get_last_flush_count() const -> size_t { return last_flush_count_; }
    
    // 配置
    void set_config(TelemetryConfig config) { config_ = std::move(config); }
    void enable(bool on = true) { config_.enabled = on; }
    [[nodiscard]] auto is_enabled() const -> bool { return config_.enabled; }

private:
    std::string last_flush_endpoint_;
    size_t last_flush_count_{0};
};

} // namespace cc::services
