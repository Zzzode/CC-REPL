// C++23 Module: Voice input/output state management and audio processing pipeline
module;

#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.voice_hooks;


export namespace cc::hooks {

// 语音输入模式
enum class VoiceMode {
    PushToTalk,       // 按住说话模式
    VoiceActivity,    // 语音活动检测（VAD）自动触发
    WakeWord,         // 唤醒词触发
    Disabled          // 关闭语音
};

// 音频状态快照
struct AudioState {
    bool is_recording{false};           // 是否正在录音
    float level_db{-60.0f};             // 当前音量分贝 (dB)
    bool is_speaking{false};            // TTS 是否正在播放语音
    std::chrono::milliseconds recording_duration{0};  // 当前录音时长
};

// STT (语音转文字) 流式结果
struct TranscriptChunk {
    std::string text;
    bool is_final{false};               // 是否为最终确认结果
    float confidence{0.0f};             // 识别置信度 (0.0-1.0)
    std::chrono::milliseconds timestamp{0};
};

// TTS (文字转语音) 队列项
struct SpeechQueueItem {
    std::string text;
    float speed{1.0f};                  // 播放速率
    std::optional<std::string> voice_id; // 指定声音 ID
    bool interruptible{true};           // 是否允许被打断
};

// 语音 hook 内部状态
struct VoiceHookState {
    VoiceMode mode{VoiceMode::Disabled};
    AudioState audio;
    std::string current_transcript;      // 当前累积的转录文本
    std::vector<TranscriptChunk> chunks; // 流式转录片段
    std::queue<SpeechQueueItem> speech_queue;  // TTS 播放队列
    bool vad_active{false};              // VAD 是否检测到语音活动
    std::optional<std::string> wake_word; // 配置的唤醒词
};

// 事件回调类型
using VoiceEventCallback = std::function<void(std::string_view event, std::string_view data)>;

// ─── VoiceHook: 核心语音管理类 ──────────────────────────────────
class VoiceHook {
public:
    explicit VoiceHook(VoiceMode initial_mode = VoiceMode::Disabled)
    {
        state_.mode = initial_mode;
    }

    // 开始录音
    [[nodiscard]] auto start_recording() -> std::expected<void, std::string> {
        if (state_.mode == VoiceMode::Disabled) {
            return std::unexpected("Voice is disabled");
        }
        if (state_.audio.is_recording) {
            return std::unexpected("Already recording");
        }
        state_.audio.is_recording = true;
        state_.audio.recording_duration = std::chrono::milliseconds{0};
        state_.current_transcript.clear();
        state_.chunks.clear();
        recording_start_ = std::chrono::steady_clock::now();
        emit_event("recording_start", "");
        return {};
    }

    // 停止录音
    [[nodiscard]] auto stop_recording() -> std::expected<std::string, std::string> {
        if (!state_.audio.is_recording) {
            return std::unexpected("Not currently recording");
        }
        state_.audio.is_recording = false;
        // 计算录音时长
        auto elapsed = std::chrono::steady_clock::now() - recording_start_;
        state_.audio.recording_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
        emit_event("recording_stop", state_.current_transcript);
        return state_.current_transcript;
    }

    // 获取当前转录文本
    [[nodiscard]] auto get_transcript() const -> std::string {
        return state_.current_transcript;
    }

    // 将文本加入 TTS 播放队列
    auto speak(std::string_view text, float speed = 1.0f) -> void {
        state_.speech_queue.push(SpeechQueueItem{
            .text = std::string(text),
            .speed = speed,
            .voice_id = std::nullopt,
            .interruptible = true
        });
        // 如果当前没有在播放，启动播放
        if (!state_.audio.is_speaking) {
            process_speech_queue();
        }
    }

    // 取消当前 TTS 语音播放并清空队列
    auto cancel_speech() -> void {
        // 清空队列
        while (!state_.speech_queue.empty()) {
            state_.speech_queue.pop();
        }
        state_.audio.is_speaking = false;
        emit_event("speech_cancelled", "");
    }

    // 设置语音模式
    auto set_mode(VoiceMode mode) -> void {
        if (state_.mode == mode) return;
        // 从非 Disabled 切到 Disabled 时，停止录音
        if (mode == VoiceMode::Disabled && state_.audio.is_recording) {
            [[maybe_unused]] auto _ = stop_recording();
        }
        state_.mode = mode;
        emit_event("mode_changed", format_mode(mode));
    }

    // 获取当前音频电平 (dB)
    [[nodiscard]] auto get_audio_level() const -> float {
        return state_.audio.level_db;
    }

    // 语音活动检测是否激活
    [[nodiscard]] auto is_voice_active() const -> bool {
        return state_.vad_active;
    }

    // 获取当前模式
    [[nodiscard]] auto mode() const -> VoiceMode { return state_.mode; }

    // 获取完整状态快照
    [[nodiscard]] auto state() const -> const VoiceHookState& { return state_; }

    // 获取音频状态
    [[nodiscard]] auto audio_state() const -> const AudioState& { return state_.audio; }

    // 设置唤醒词
    auto set_wake_word(std::string_view word) -> void {
        state_.wake_word = std::string(word);
    }

    // 注册事件回调
    auto on_event(VoiceEventCallback cb) -> void {
        event_callback_ = std::move(cb);
    }

    // ─── 以下为音频子系统回调接口（由底层音频引擎调用）─────────

    // 接收音频电平更新
    auto feed_audio_level(float db) -> void {
        state_.audio.level_db = db;
        // VAD 简易实现：超过阈值视为有语音活动
        constexpr float vad_threshold = -30.0f;
        bool was_active = state_.vad_active;
        state_.vad_active = (db > vad_threshold);

        // VAD 模式下自动开始/停止录音
        if (state_.mode == VoiceMode::VoiceActivity) {
            if (state_.vad_active && !state_.audio.is_recording) {
                [[maybe_unused]] auto _ = start_recording();
            } else if (!state_.vad_active && was_active && state_.audio.is_recording) {
                [[maybe_unused]] auto _ = stop_recording();
            }
        }
    }

    // 接收 STT 流式结果
    auto feed_transcript_chunk(TranscriptChunk chunk) -> void {
        state_.chunks.push_back(chunk);
        if (chunk.is_final) {
            // 最终结果：追加到累积文本
            if (!state_.current_transcript.empty()) {
                state_.current_transcript += ' ';
            }
            state_.current_transcript += chunk.text;
            emit_event("transcript_final", chunk.text);
        } else {
            emit_event("transcript_partial", chunk.text);
        }
    }

    // TTS 播放完毕通知
    auto on_speech_finished() -> void {
        state_.audio.is_speaking = false;
        emit_event("speech_finished", "");
        // 继续处理队列中的下一项
        process_speech_queue();
    }

private:
    VoiceHookState state_;
    std::chrono::steady_clock::time_point recording_start_;
    VoiceEventCallback event_callback_;

    // 处理 TTS 队列
    auto process_speech_queue() -> void {
        if (state_.speech_queue.empty()) return;
        state_.audio.is_speaking = true;
        auto& item = state_.speech_queue.front();
        emit_event("speech_start", item.text);
        state_.speech_queue.pop();
        on_speech_finished();
    }

    // 发射事件
    auto emit_event(std::string_view event, std::string_view data) -> void {
        if (event_callback_) {
            event_callback_(event, data);
        }
    }

    // 格式化模式字符串
    [[nodiscard]] static auto format_mode(VoiceMode m) -> std::string_view {
        switch (m) {
            case VoiceMode::PushToTalk:     return "push_to_talk";
            case VoiceMode::VoiceActivity:  return "voice_activity";
            case VoiceMode::WakeWord:       return "wake_word";
            case VoiceMode::Disabled:       return "disabled";
        }
        return "unknown";
    }
};

} // namespace cc::hooks
