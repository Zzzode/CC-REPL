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


enum class VoiceMode {
    PushToTalk,
    VoiceActivity,
    WakeWord,
    Disabled
};


struct AudioState {
    bool is_recording{false};
    float level_db{-60.0f};
    bool is_speaking{false};
    std::chrono::milliseconds recording_duration{0};
};


struct TranscriptChunk {
    std::string text;
    bool is_final{false};
    float confidence{0.0f};
    std::chrono::milliseconds timestamp{0};
};


struct SpeechQueueItem {
    std::string text;
    float speed{1.0f};
    std::optional<std::string> voice_id;
    bool interruptible{true};
};


struct VoiceHookState {
    VoiceMode mode{VoiceMode::Disabled};
    AudioState audio;
    std::string current_transcript;
    std::vector<TranscriptChunk> chunks;
    std::queue<SpeechQueueItem> speech_queue;
    bool vad_active{false};
    std::optional<std::string> wake_word;
};


using VoiceEventCallback = std::function<void(std::string_view event, std::string_view data)>;


class VoiceHook {
public:
    explicit VoiceHook(VoiceMode initial_mode = VoiceMode::Disabled)
    {
        state_.mode = initial_mode;
    }


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


    [[nodiscard]] auto stop_recording() -> std::expected<std::string, std::string> {
        if (!state_.audio.is_recording) {
            return std::unexpected("Not currently recording");
        }
        state_.audio.is_recording = false;

        auto elapsed = std::chrono::steady_clock::now() - recording_start_;
        state_.audio.recording_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
        emit_event("recording_stop", state_.current_transcript);
        return state_.current_transcript;
    }


    [[nodiscard]] auto get_transcript() const -> std::string {
        return state_.current_transcript;
    }


    auto speak(std::string_view text, float speed = 1.0f) -> void {
        state_.speech_queue.push(SpeechQueueItem{
            .text = std::string(text),
            .speed = speed,
            .voice_id = std::nullopt,
            .interruptible = true
        });

        if (!state_.audio.is_speaking) {
            process_speech_queue();
        }
    }


    auto cancel_speech() -> void {

        while (!state_.speech_queue.empty()) {
            state_.speech_queue.pop();
        }
        state_.audio.is_speaking = false;
        emit_event("speech_cancelled", "");
    }


    auto set_mode(VoiceMode mode) -> void {
        if (state_.mode == mode) return;

        if (mode == VoiceMode::Disabled && state_.audio.is_recording) {
            [[maybe_unused]] auto _ = stop_recording();
        }
        state_.mode = mode;
        emit_event("mode_changed", format_mode(mode));
    }


    [[nodiscard]] auto get_audio_level() const -> float {
        return state_.audio.level_db;
    }


    [[nodiscard]] auto is_voice_active() const -> bool {
        return state_.vad_active;
    }


    [[nodiscard]] auto mode() const -> VoiceMode { return state_.mode; }


    [[nodiscard]] auto state() const -> const VoiceHookState& { return state_; }


    [[nodiscard]] auto audio_state() const -> const AudioState& { return state_.audio; }


    auto set_wake_word(std::string_view word) -> void {
        state_.wake_word = std::string(word);
    }


    auto on_event(VoiceEventCallback cb) -> void {
        event_callback_ = std::move(cb);
    }




    auto feed_audio_level(float db) -> void {
        state_.audio.level_db = db;

        constexpr float vad_threshold = -30.0f;
        bool was_active = state_.vad_active;
        state_.vad_active = (db > vad_threshold);


        if (state_.mode == VoiceMode::VoiceActivity) {
            if (state_.vad_active && !state_.audio.is_recording) {
                [[maybe_unused]] auto _ = start_recording();
            } else if (!state_.vad_active && was_active && state_.audio.is_recording) {
                [[maybe_unused]] auto _ = stop_recording();
            }
        }
    }


    auto feed_transcript_chunk(TranscriptChunk chunk) -> void {
        state_.chunks.push_back(chunk);
        if (chunk.is_final) {

            if (!state_.current_transcript.empty()) {
                state_.current_transcript += ' ';
            }
            state_.current_transcript += chunk.text;
            emit_event("transcript_final", chunk.text);
        } else {
            emit_event("transcript_partial", chunk.text);
        }
    }


    auto on_speech_finished() -> void {
        state_.audio.is_speaking = false;
        emit_event("speech_finished", "");

        process_speech_queue();
    }

private:
    VoiceHookState state_;
    std::chrono::steady_clock::time_point recording_start_;
    VoiceEventCallback event_callback_;


    auto process_speech_queue() -> void {
        if (state_.speech_queue.empty()) return;
        state_.audio.is_speaking = true;
        auto& item = state_.speech_queue.front();
        emit_event("speech_start", item.text);
        state_.speech_queue.pop();
        on_speech_finished();
    }


    auto emit_event(std::string_view event, std::string_view data) -> void {
        if (event_callback_) {
            event_callback_(event, data);
        }
    }


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
