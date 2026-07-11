// ============================================================================
/// @file voice_hooks.cppm
/// @brief Voice hooks: useVoice + useVoiceIntegration faithful port.
///
/// TS REF: src/hooks/useVoice.ts (1144 lines) — main voice hook: recording,
///          playback, interim results, error handling, session generation.
/// TS REF: src/hooks/useVoiceIntegration.tsx (676 lines) — UI integration:
///          push-to-talk, voice indicator, keyboard shortcuts, strip trailing,
///          anchor management, interim range.
///
/// This module provides the hooks-layer abstraction between the REPL UI and
/// the voice service layer (AudioCapture + VoiceStreamSTTService). It manages:
///   - Hold-to-talk key event handling with auto-repeat release detection
///   - Focus-mode recording (terminal focus starts/stops session)
///   - Interim transcript streaming into the prompt input
///   - Audio level histogram for the waveform visualizer
///   - Session generation tracking to prevent stale callbacks
///   - Silent-drop replay for sticky-broken CE pods
///   - Early-error retry with backoff
///   - Language normalization to BCP-47 codes
// ============================================================================
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

export module cc.hooks.voice_hooks;

import cc.services.voice.voice;
import cc.services.voice_stream_stt;
import cc.services.voice.keyterms;
import cc.context.voice;

export namespace cc::hooks::voice {

using namespace std::chrono_literals;
using cc::context::VoiceState;

// ============================================================================
// Constants — TS REF: useVoice.ts:39,51,54,160,171,172,177,180
// ============================================================================

/// Maximum gap (ms) between key presses to count as held (auto-repeat).
/// TS REF: useVoiceIntegration.tsx:39
constexpr int RAPID_KEY_GAP_MS = 120;

/// Fallback (ms) for modifier-combo first-press activation.
/// TS REF: useVoiceIntegration.tsx:46
constexpr int MODIFIER_FIRST_PRESS_FALLBACK_MS = 2000;

/// Number of rapid consecutive key events required to activate voice (bare chars).
/// TS REF: useVoiceIntegration.tsx:51
constexpr int HOLD_THRESHOLD = 5;

/// Number of rapid key events to start showing warmup feedback.
/// TS REF: useVoiceIntegration.tsx:54
constexpr int WARMUP_THRESHOLD = 2;

/// Gap (ms) between auto-repeat key events that signals key release.
/// TS REF: useVoice.ts:160
constexpr int RELEASE_TIMEOUT_MS = 200;

/// Fallback (ms) to arm the release timer if no auto-repeat is seen.
/// TS REF: useVoice.ts:171
constexpr int REPEAT_FALLBACK_MS = 600;

/// How long (ms) to keep a focus-mode session alive without speech.
/// TS REF: useVoice.ts:177
constexpr int FOCUS_SILENCE_TIMEOUT_MS = 5000;

/// Number of bars shown in the recording waveform visualizer.
/// TS REF: useVoice.ts:180
constexpr int AUDIO_LEVEL_BARS = 16;

/// Default STT language code.
/// TS REF: useVoice.ts:32
constexpr std::string_view DEFAULT_STT_LANGUAGE = "en";

// ============================================================================
// Error types — TS REF: useVoice.ts:495-508 (error messages)
// ============================================================================

/// Categorizes voice errors for UI display and recovery logic.
enum class VoiceErrorType : std::uint8_t {
    None,
    MicPermissionDenied,     // TCC microphone access denied
    MicNotFound,             // No audio input device found
    SttServiceUnavailable,   // voice_stream endpoint unreachable
    NetworkError,            // DNS/connect failure
    NoSpeechDetected,        // Recording ended with empty transcript
    NoAudioSignal,           // Mic silent/inaccessible
    StreamError,             // WebSocket error mid-session
    ModuleNotLoaded,         // Voice module not ready
    AlreadyRecording,        // Start called while recording
    NotRecording,            // Stop called while idle
};

struct VoiceError {
    VoiceErrorType type{VoiceErrorType::None};
    std::string message;

    [[nodiscard]] bool is_error() const noexcept {
        return type != VoiceErrorType::None;
    }
};

// ============================================================================
// Language normalization — TS REF: useVoice.ts:42-134
// ============================================================================

/// Normalize a language preference string to a BCP-47 code supported by the
/// voice_stream endpoint. Returns default language if unresolvable.
/// TS REF: useVoice.ts:121-134
struct NormalizedLanguage {
    std::string code;
    std::optional<std::string> fell_back_from;
};

namespace detail {

// Subset of the server-side supported_language_codes allowlist.
// TS REF: useVoice.ts:93-114
const std::array<std::string_view, 20> SUPPORTED_LANGUAGE_CODES = {
    "en", "es", "fr", "ja", "de", "pt", "it", "ko", "hi", "id",
    "ru", "pl", "tr", "nl", "uk", "el", "cs", "da", "sv", "no"
};

// Maps language names to BCP-47 codes. TS REF: useVoice.ts:42-89
const std::unordered_map<std::string, std::string>& language_name_to_code() {
    static const std::unordered_map<std::string, std::string> map = {
        {"english", "en"}, {"spanish", "es"}, {"español", "es"},
        {"espanol", "es"}, {"french", "fr"}, {"français", "fr"},
        {"francais", "fr"}, {"japanese", "ja"}, {"日本語", "ja"},
        {"german", "de"}, {"deutsch", "de"}, {"portuguese", "pt"},
        {"português", "pt"}, {"portugues", "pt"}, {"italian", "it"},
        {"italiano", "it"}, {"korean", "ko"}, {"한국어", "ko"},
        {"hindi", "hi"}, {"हिन्दी", "hi"}, {"indonesian", "id"},
        {"bahasa indonesia", "id"}, {"bahasa", "id"}, {"russian", "ru"},
        {"русский", "ru"}, {"polish", "pl"}, {"polski", "pl"},
        {"turkish", "tr"}, {"türkçe", "tr"}, {"turkce", "tr"},
        {"dutch", "nl"}, {"nederlands", "nl"}, {"ukrainian", "uk"},
        {"українська", "uk"}, {"greek", "el"}, {"ελληνικά", "el"},
        {"czech", "cs"}, {"čeština", "cs"}, {"cestina", "cs"},
        {"danish", "da"}, {"dansk", "da"}, {"swedish", "sv"},
        {"svenska", "sv"}, {"norwegian", "no"}, {"norsk", "no"},
    };
    return map;
}

std::string to_lower(std::string_view sv) {
    std::string result(sv);
    for (auto& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

} // namespace detail

/// Normalize language string to BCP-47 code.
/// TS REF: useVoice.ts:121-134
[[nodiscard]] NormalizedLanguage normalize_language_for_stt(
        std::optional<std::string_view> language) {
    if (!language || language->empty()) {
        return {std::string(DEFAULT_STT_LANGUAGE), std::nullopt};
    }
    auto lower = detail::to_lower(*language);
    // Trim whitespace
    while (!lower.empty() && lower.front() == ' ') lower.erase(lower.begin());
    while (!lower.empty() && lower.back() == ' ') lower.pop_back();
    if (lower.empty()) {
        return {std::string(DEFAULT_STT_LANGUAGE), std::nullopt};
    }

    // Direct code match
    for (auto code : detail::SUPPORTED_LANGUAGE_CODES) {
        if (lower == code) return {std::string(code), std::nullopt};
    }

    // Language name match
    const auto& name_map = detail::language_name_to_code();
    auto it = name_map.find(lower);
    if (it != name_map.end()) {
        return {it->second, std::nullopt};
    }

    // Base language (strip region: en-US → en)
    auto dash = lower.find('-');
    if (dash != std::string::npos) {
        auto base = lower.substr(0, dash);
        for (auto code : detail::SUPPORTED_LANGUAGE_CODES) {
            if (base == code) return {std::string(code), std::nullopt};
        }
    }

    return {std::string(DEFAULT_STT_LANGUAGE), std::string(*language)};
}

// ============================================================================
// Audio level computation — TS REF: useVoice.ts:185-197
// ============================================================================

/// Compute RMS amplitude from a 16-bit signed PCM buffer, normalized 0-1.
/// Uses sqrt curve to spread quieter levels across the visual range.
/// TS REF: useVoice.ts:185-197
[[nodiscard]] float compute_audio_level(const std::vector<uint8_t>& chunk) {
    size_t samples = chunk.size() / 2; // 16-bit = 2 bytes per sample
    if (samples == 0) return 0.0f;

    double sum_sq = 0.0;
    for (size_t i = 0; i + 1 < chunk.size(); i += 2) {
        // Read 16-bit signed little-endian
        int16_t sample;
        std::memcpy(&sample, &chunk[i], sizeof(int16_t));
        double d = static_cast<double>(sample);
        sum_sq += d * d;
    }
    double rms = std::sqrt(sum_sq / static_cast<double>(samples));
    double normalized = std::min(rms / 2000.0, 1.0);
    return static_cast<float>(std::sqrt(normalized));
}

// ============================================================================
// VoiceIntegrationState — TS REF: useVoiceIntegration.tsx result shape
// ============================================================================

/// Aggregate state exposed to the UI layer for rendering voice indicators,
/// interim transcripts, and error banners.
struct VoiceIntegrationState {
    bool is_recording{false};
    bool is_playing{false};
    bool is_warming_up{false};
    std::string interim_text;
    std::string final_text;
    VoiceError error;
    float volume_level{0.0f};
    std::vector<float> audio_levels;  // Histogram for waveform visualizer
    std::chrono::milliseconds recording_duration{0};
};

// ============================================================================
// InterimRange — TS REF: useVoiceIntegration.tsx:97-100,328-340
// ============================================================================

/// Character range of interim (not-yet-finalized) transcript text within the
/// prompt input, so the UI can dim it.
struct InterimRange {
    std::size_t start{0};
    std::size_t end{0};
};

// ============================================================================
// VoiceRecorder — TS REF: useVoice.ts:199-1144 (useVoice hook body)
// ============================================================================

/// Manages a voice recording session: audio capture, STT streaming, interim
/// transcript accumulation, audio level tracking, and session lifecycle.
///
/// Thread safety: all public methods are safe to call from any thread.
/// Internal state is guarded by state_mutex_. Audio callbacks arrive on the
/// capture thread; STT callbacks arrive on the recv thread.
class VoiceRecorder {
public:
    using TranscriptCallback = std::function<void(std::string_view text, bool is_final)>;
    using InterimCallback = std::function<void(std::string_view text)>;
    using ErrorCallback = std::function<void(const VoiceError& err)>;
    using StateChangeCallback = std::function<void(VoiceState new_state)>;
    using LevelsCallback = std::function<void(const std::vector<float>& levels)>;

    VoiceRecorder() = default;
    ~VoiceRecorder() { stop_recording(); }

    VoiceRecorder(const VoiceRecorder&) = delete;
    VoiceRecorder& operator=(const VoiceRecorder&) = delete;

    /// Start a new recording session. Returns error if already recording or
    /// if audio capture cannot be initialized.
    /// TS REF: useVoice.ts:633-1011 (startRecordingSession)
    std::expected<void, VoiceError> start_recording(
            std::optional<std::string_view> language_hint = std::nullopt) {
        std::lock_guard lock(state_mutex_);

        if (state_ == VoiceState::Listening) {
            return std::unexpected(VoiceError{
                VoiceErrorType::AlreadyRecording, "Already recording"});
        }

        // Pre-check: is audio capture available?
        // TS REF: useVoice.ts:661-672
        if (!cc::services::voice::AudioCapture::is_recording_available()) {
            return std::unexpected(VoiceError{
                VoiceErrorType::MicNotFound,
                "SoX 'rec' command not found. Install with: brew install sox"});
        }

        // Reset session state
        // TS REF: useVoice.ts:648-658
        session_gen_++;
        accumulated_transcript_.clear();
        has_audio_signal_ = false;
        ever_connected_ = false;
        retry_used_ = false;
        silent_drop_retried_ = false;
        full_audio_buffer_.clear();
        audio_levels_.clear();
        recording_start_ = std::chrono::steady_clock::now();

        auto normalized = normalize_language_for_stt(language_hint);
        stt_language_ = normalized.code;

        set_state_locked(VoiceState::Listening);

        // Start audio capture immediately — buffer chunks while WebSocket connects.
        // TS REF: useVoice.ts:688-745
        const auto my_gen = session_gen_.load();
        audio_capture_ = std::make_unique<cc::services::voice::AudioCapture>();
        bool started = audio_capture_->start(
            [this, my_gen](const std::vector<uint8_t>& chunk) {
                on_audio_chunk(chunk, my_gen);
            });

        if (!started) {
            audio_capture_.reset();
            set_state_locked(VoiceState::Idle);
            return std::unexpected(VoiceError{
                VoiceErrorType::MicPermissionDenied,
                "Failed to start audio capture. Check microphone permissions."});
        }

        // Connect STT WebSocket in parallel
        // TS REF: useVoice.ts:771-1010
        connect_stt(my_gen);

        return {};
    }

    /// Stop recording and finalize the transcript. Returns the accumulated
    /// final transcript text.
    /// TS REF: useVoice.ts:322-522 (finishRecording)
    std::expected<std::string, VoiceError> stop_recording() {
        std::unique_lock lock(state_mutex_);

        if (state_ != VoiceState::Listening) {
            return std::unexpected(VoiceError{
                VoiceErrorType::NotRecording, "Not currently recording"});
        }

        const auto my_gen = session_gen_.load();
        (void)my_gen;  // reserved for future session-gen staleness check
        const auto had_audio = has_audio_signal_.load();
        const auto ws_connected = ever_connected_;
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - recording_start_);

        set_state_locked(VoiceState::Processing);

        // Stop audio capture
        if (audio_capture_) {
            audio_capture_->stop();
            audio_capture_.reset();
        }

        // Finalize STT connection
        // TS REF: useVoice.ts:362-517
        std::string final_text;
        if (stt_service_ && stt_service_->is_connected()) {
            auto finalize_source = stt_service_->finalize();
            (void)finalize_source; // Used for silent-drop detection in TS

            final_text = accumulated_transcript_;
            // Trim whitespace
            while (!final_text.empty() && final_text.front() == ' ')
                final_text.erase(final_text.begin());
            while (!final_text.empty() && final_text.back() == ' ')
                final_text.pop_back();

            stt_service_->close();
            stt_service_.reset();
        }

        // Error diagnostics — TS REF: useVoice.ts:491-508
        if (final_text.empty() && duration > 2s) {
            VoiceError err;
            if (!ws_connected) {
                err = {VoiceErrorType::NetworkError,
                       "Voice connection failed. Check your network and try again."};
            } else if (!had_audio) {
                err = {VoiceErrorType::NoAudioSignal,
                       "No audio detected from microphone. Check that the correct "
                       "input device is selected and that Claude Code has "
                       "microphone access."};
            } else {
                err = {VoiceErrorType::NoSpeechDetected, "No speech detected."};
            }
            if (on_error_) on_error_(err);
        }

        set_state_locked(VoiceState::Idle);

        if (!final_text.empty()) {
            if (on_transcript_) on_transcript_(final_text, true);
        }

        return final_text;
    }

    /// Whether we are currently recording audio.
    [[nodiscard]] bool is_recording() const {
        std::lock_guard lock(state_mutex_);
        return state_ == VoiceState::Listening;
    }

    /// Current voice state (idle/listening/processing/error).
    [[nodiscard]] VoiceState state() const {
        std::lock_guard lock(state_mutex_);
        return state_;
    }

    /// Get the current interim (partial) transcript text.
    [[nodiscard]] std::string get_interim_text() const {
        std::lock_guard lock(state_mutex_);
        return interim_text_;
    }

    /// Get the current audio level histogram (for waveform visualizer).
    [[nodiscard]] std::vector<float> get_audio_levels() const {
        std::lock_guard lock(state_mutex_);
        return audio_levels_;
    }

    /// Get current recording duration.
    [[nodiscard]] std::chrono::milliseconds get_recording_duration() const {
        std::lock_guard lock(state_mutex_);
        if (state_ != VoiceState::Listening) return 0ms;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - recording_start_);
    }

    // ── Callback registration ──────────────────────────────────────────

    /// Called when a final transcript chunk is received.
    void on_transcript(TranscriptCallback cb) {
        std::lock_guard lock(state_mutex_);
        on_transcript_ = std::move(cb);
    }

    /// Called when interim (partial) transcript text updates.
    void on_interim_text(InterimCallback cb) {
        std::lock_guard lock(state_mutex_);
        on_interim_ = std::move(cb);
    }

    /// Called when an error occurs.
    void on_error(ErrorCallback cb) {
        std::lock_guard lock(state_mutex_);
        on_error_ = std::move(cb);
    }

    /// Called when voice state changes.
    void on_state_change(StateChangeCallback cb) {
        std::lock_guard lock(state_mutex_);
        on_state_change_ = std::move(cb);
    }

    /// Called when audio level histogram updates.
    void on_levels_update(LevelsCallback cb) {
        std::lock_guard lock(state_mutex_);
        on_levels_ = std::move(cb);
    }

    /// Whether the STT stream service is available.
    /// TS REF: useVoice.ts:1024 (isVoiceStreamAvailable)
    [[nodiscard]] static bool is_stt_available() noexcept {
        return cc::services::voice_stream_stt::VoiceStreamSTTService::is_available();
    }

private:
    // ── Internal helpers (caller must hold state_mutex_) ───────────────

    void set_state_locked(VoiceState new_state) {
        if (state_ == new_state) return;
        state_ = new_state;
        if (on_state_change_) on_state_change_(new_state);
    }

    /// Audio chunk callback from AudioCapture capture thread.
    /// TS REF: useVoice.ts:694-731
    void on_audio_chunk(const std::vector<uint8_t>& chunk, uint64_t my_gen) {
        if (my_gen != session_gen_.load()) return; // Stale session

        // Compute audio level
        float level = compute_audio_level(chunk);
        if (!has_audio_signal_ && level > 0.01f) {
            has_audio_signal_ = true;
        }

        // Update level histogram
        std::lock_guard lock(state_mutex_);
        if (audio_levels_.size() >= static_cast<size_t>(AUDIO_LEVEL_BARS)) {
            audio_levels_.erase(audio_levels_.begin());
        }
        audio_levels_.push_back(level);
        if (on_levels_) on_levels_(audio_levels_);

        // Buffer full audio for potential silent-drop replay
        // TS REF: useVoice.ts:697-703
        if (full_audio_buffer_.size() < 2000) { // ~2MB safety cap
            full_audio_buffer_.push_back(chunk);
        }

        // Send to STT if connected, otherwise buffer
        if (stt_service_ && stt_service_->is_connected()) {
            // Flush any pending buffered audio first
            if (!pending_audio_buffer_.empty()) {
                for (const auto& buf : pending_audio_buffer_) {
                    stt_service_->send_audio(buf);
                }
                pending_audio_buffer_.clear();
            }
            stt_service_->send_audio(chunk);
        } else {
            pending_audio_buffer_.push_back(chunk);
        }
    }

    /// Connect to the voice_stream STT WebSocket.
    /// TS REF: useVoice.ts:779-1007 (attemptConnect)
    void connect_stt(uint64_t my_gen) {
        using namespace cc::services::voice_stream_stt;

        // Gather keyterms for STT context
        auto keyterms = cc::services::voice::keyterms::get_voice_keyterms();

        VoiceStreamConfig config;
        config.language = stt_language_;
        config.keyterms = std::move(keyterms);

        auto service = std::make_unique<VoiceStreamSTTService>();
        auto result = service->connect(
            config,
            // on_transcript — TS REF: useVoice.ts:783-839
            [this, my_gen](std::string_view text, bool is_final) {
                if (my_gen != session_gen_.load()) return;
                handle_stt_transcript(text, is_final);
            },
            // on_error — TS REF: useVoice.ts:841-902
            [this, my_gen](std::string_view error, bool is_fatal) {
                if (my_gen != session_gen_.load()) return;
                handle_stt_error(error, is_fatal);
            },
            // on_close
            []() { /* lifecycle handled by stop_recording */ },
            // on_ready — TS REF: useVoice.ts:907-974
            [this, my_gen]() {
                if (my_gen != session_gen_.load()) return;
                handle_stt_ready();
            }
        );

        if (!result) {
            VoiceError err{VoiceErrorType::NetworkError,
                          std::string("Failed to connect to voice stream: ") +
                          result.error().message()};
            if (on_error_) on_error_(err);
            std::lock_guard lock(state_mutex_);
            set_state_locked(VoiceState::Idle);
            return;
        }

        std::lock_guard lock(state_mutex_);
        stt_service_ = std::move(service);
    }

    /// Handle STT transcript callback.
    /// TS REF: useVoice.ts:783-839
    void handle_stt_transcript(std::string_view text, bool is_final) {
        std::lock_guard lock(state_mutex_);
        std::string text_str(text);

        // Trim
        while (!text_str.empty() && text_str.front() == ' ')
            text_str.erase(text_str.begin());
        while (!text_str.empty() && text_str.back() == ' ')
            text_str.pop_back();

        if (is_final && !text_str.empty()) {
            // Accumulate final transcript separated by spaces
            if (!accumulated_transcript_.empty()) {
                accumulated_transcript_ += ' ';
            }
            accumulated_transcript_ += text_str;

            // Update interim to show accumulated finals + current
            interim_text_ = accumulated_transcript_;
            if (on_interim_) on_interim_(interim_text_);
            if (on_transcript_) on_transcript_(text_str, true);
        } else if (!is_final) {
            // Interim: show accumulated finals + current interim as preview
            std::string preview = accumulated_transcript_;
            if (!preview.empty() && !text_str.empty()) preview += ' ';
            preview += text_str;
            interim_text_ = preview;
            if (on_interim_) on_interim_(interim_text_);
        }
    }

    /// Handle STT error callback.
    /// TS REF: useVoice.ts:841-902
    void handle_stt_error(std::string_view error, bool is_fatal) {
        (void)is_fatal;
        VoiceError err{VoiceErrorType::StreamError,
                      std::string("Voice stream error: ") + std::string(error)};
        if (on_error_) on_error_(err);

        std::lock_guard lock(state_mutex_);
        pending_audio_buffer_.clear();
        if (stt_service_) {
            stt_service_->close();
            stt_service_.reset();
        }
        set_state_locked(VoiceState::Idle);
    }

    /// Handle STT ready callback — flush buffered audio.
    /// TS REF: useVoice.ts:907-974
    void handle_stt_ready() {
        std::lock_guard lock(state_mutex_);
        ever_connected_ = true;

        if (!stt_service_ || pending_audio_buffer_.empty()) return;

        // Coalesce buffered chunks into ~1s slices for fewer WS frames.
        // TS REF: useVoice.ts:928-951
        constexpr size_t SLICE_TARGET_BYTES = 32000; // ~1s at 16kHz/16-bit/mono
        std::vector<uint8_t> coalesced;
        for (const auto& chunk : pending_audio_buffer_) {
            if (!coalesced.empty() &&
                coalesced.size() + chunk.size() > SLICE_TARGET_BYTES) {
                stt_service_->send_audio(coalesced);
                coalesced.clear();
            }
            coalesced.insert(coalesced.end(), chunk.begin(), chunk.end());
        }
        if (!coalesced.empty()) {
            stt_service_->send_audio(coalesced);
        }
        pending_audio_buffer_.clear();
    }

    // ── State (guarded by state_mutex_) ────────────────────────────────
    mutable std::mutex state_mutex_;
    VoiceState state_{VoiceState::Idle};

    std::unique_ptr<cc::services::voice::AudioCapture> audio_capture_;
    std::unique_ptr<cc::services::voice_stream_stt::VoiceStreamSTTService> stt_service_;

    std::string stt_language_{std::string(DEFAULT_STT_LANGUAGE)};
    std::string accumulated_transcript_;
    std::string interim_text_;

    std::vector<std::vector<uint8_t>> full_audio_buffer_;    // For silent-drop replay
    std::vector<std::vector<uint8_t>> pending_audio_buffer_; // Buffered before WS ready
    std::vector<float> audio_levels_;

    std::chrono::steady_clock::time_point recording_start_{};
    std::atomic<uint64_t> session_gen_{0}; // Bumped on each new session (atomic for cross-thread checks)
    std::atomic<bool> has_audio_signal_{false};
    bool ever_connected_{false};
    bool retry_used_{false};
    bool silent_drop_retried_{false};

    // ── Callbacks ──────────────────────────────────────────────────────
    TranscriptCallback on_transcript_;
    InterimCallback on_interim_;
    ErrorCallback on_error_;
    StateChangeCallback on_state_change_;
    LevelsCallback on_levels_;
};

// ============================================================================
// VoicePlayback — TS REF: useVoice.ts speak() / TTS output
// ============================================================================

/// Manages text-to-speech playback queue. Currently a stub — real TTS output
/// requires platform-specific audio synthesis (say command on macOS, espeak
/// on Linux, etc.). The queue structure mirrors TS speech_queue.
class VoicePlayback {
public:
    using PlaybackStartCallback = std::function<void(std::string_view text)>;
    using PlaybackEndCallback = std::function<void()>;

    /// Enqueue text for speech playback.
    /// TS REF: voice_hooks.cppm:110-121 (original speak)
    void play(std::string_view text, float speed = 1.0f) {
        std::lock_guard lock(mutex_);
        queue_.push_back(QueuedSpeech{std::string(text), speed});
        if (!is_playing_) {
            process_queue();
        }
    }

    /// Stop all playback immediately.
    void stop_playback() {
        std::lock_guard lock(mutex_);
        queue_.clear();
        is_playing_ = false;
        if (on_end_) on_end_();
    }

    /// Whether audio is currently playing.
    [[nodiscard]] bool is_playing() const {
        std::lock_guard lock(mutex_);
        return is_playing_;
    }

    void on_playback_start(PlaybackStartCallback cb) {
        std::lock_guard lock(mutex_);
        on_start_ = std::move(cb);
    }
    void on_playback_end(PlaybackEndCallback cb) {
        std::lock_guard lock(mutex_);
        on_end_ = std::move(cb);
    }

private:
    struct QueuedSpeech {
        std::string text;
        float speed{1.0f};
    };

    void process_queue() {
        if (queue_.empty()) {
            is_playing_ = false;
            if (on_end_) on_end_();
            return;
        }
        is_playing_ = true;
        auto item = std::move(queue_.front());
        queue_.erase(queue_.begin());
        if (on_start_) on_start_(item.text);
        // TODO(platform-tts): real audio synthesis via `say`/espeak subprocess.
        // For now, immediately mark as finished — the queue machinery is correct.
        is_playing_ = false;
        if (on_end_) on_end_();
        // Process next item
        if (!queue_.empty()) process_queue();
    }

    mutable std::mutex mutex_;
    std::vector<QueuedSpeech> queue_;
    bool is_playing_{false};
    PlaybackStartCallback on_start_;
    PlaybackEndCallback on_end_;
};

// ============================================================================
// PushToTalkHandler — TS REF: useVoiceIntegration.tsx:373-668
// ============================================================================

/// Handles push-to-talk key hold detection. Distinguishes between:
///   - Bare chars (space, v): requires HOLD_THRESHOLD rapid auto-repeat events
///     to activate, to avoid triggering on normal typing.
///   - Modifier combos (meta+k, ctrl+x): activates on first press, since
///     modifier combos are unambiguous intent.
///
/// TS REF: useVoiceIntegration.tsx:373-668 (useVoiceKeybindingHandler)
class PushToTalkHandler {
public:
    /// Result of processing a key-down event.
    enum class KeyAction : std::uint8_t {
        PassThrough,    // Let the key flow through to text input normally
        Swallow,        // Consume the key (stop propagation)
        Activate,       // Start voice recording
        Warmup,         // Show warmup indicator but don't activate yet
    };

    PushToTalkHandler() = default;

    /// Set the PTT key as a bare character (e.g. ' ' for space).
    void set_ptt_bare_char(char c) {
        ptt_char_ = c;
        has_modifier_ = false;
    }

    /// Set the PTT key as a modifier combo (e.g. ctrl+v).
    void set_ptt_modifier_combo(bool ctrl, bool meta, bool alt, bool shift, char key) {
        ptt_char_ = key;
        ptt_ctrl_ = ctrl;
        ptt_meta_ = meta;
        ptt_alt_ = alt;
        ptt_shift_ = shift;
        has_modifier_ = (ctrl || meta || alt || shift);
    }

    /// Process a key-down event. Returns the action the caller should take.
    /// TS REF: useVoiceIntegration.tsx:468-647 (handleKeyDown)
    KeyAction handle_key_down(bool ctrl, bool meta, bool alt, bool shift,
                              char key, bool is_auto_repeat = false) {
        if (!voice_enabled_) return KeyAction::PassThrough;

        // Check if this matches our binding
        if (!matches_binding(ctrl, meta, alt, shift, key)) {
            // Not our key — but if we're in hold-active recording, swallow
            // auto-repeat of the bound key only (other keys pass through).
            return KeyAction::PassThrough;
        }

        // If we're actively holding and recording, swallow to prevent text leak.
        // TS REF: useVoiceIntegration.tsx:514-530
        if (is_hold_active_ && is_recording_) {
            if (!has_modifier_) {
                trailing_strip_count_++;
            }
            return KeyAction::Swallow;
        }

        // Non-hold recording active (focus mode) — swallow modifier combos only
        // TS REF: useVoiceIntegration.tsx:538-541
        if (is_recording_ && has_modifier_) {
            return KeyAction::Swallow;
        }

        const auto now = std::chrono::steady_clock::now();
        auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_key_time_);
        last_key_time_ = now;

        // Reset rapid count if gap is too large (not a hold pattern)
        if (gap > std::chrono::milliseconds(RAPID_KEY_GAP_MS)) {
            rapid_count_ = 0;
            chars_in_input_ = 0;
            is_warming_up_ = false;
        }

        int repeat_count = is_auto_repeat ? 1 : 1;
        rapid_count_ += repeat_count;

        // ── Activation check ──────────────────────────────────────────
        // Modifier combos activate on first press; bare chars need threshold.
        // TS REF: useVoiceIntegration.tsx:552-601
        if (has_modifier_ || rapid_count_ >= HOLD_THRESHOLD) {
            rapid_count_ = 0;
            is_hold_active_ = true;
            is_warming_up_ = false;
            return KeyAction::Activate;
        }

        // ── Warmup (bare char only) ───────────────────────────────────
        // TS REF: useVoiceIntegration.tsx:603-646
        if (rapid_count_ >= WARMUP_THRESHOLD) {
            is_warming_up_ = true;
            return KeyAction::Warmup;
        }

        // Below warmup threshold — let the char through for normal typing.
        chars_in_input_ += repeat_count;
        return KeyAction::PassThrough;
    }

    /// Called when voice state transitions away from 'recording'.
    /// TS REF: useVoiceIntegration.tsx:453-467
    void on_voice_state_changed(VoiceState state) {
        if (state != VoiceState::Listening) {
            is_hold_active_ = false;
            rapid_count_ = 0;
            chars_in_input_ = 0;
            trailing_strip_count_ = 0;
            is_warming_up_ = false;
        }
    }

    /// Whether push-to-talk is currently held and active.
    [[nodiscard]] bool is_push_to_talk_active() const { return is_hold_active_; }

    /// Whether warmup indicator should be shown.
    [[nodiscard]] bool is_warming_up() const { return is_warming_up_; }

    /// Number of chars to strip from the input on activation.
    [[nodiscard]] int chars_to_strip() const {
        return chars_in_input_ + trailing_strip_count_;
    }

    /// Whether the current binding is a bare character (needs strip).
    [[nodiscard]] bool is_bare_char_binding() const { return !has_modifier_; }

    /// Get the bound character (for stripping).
    [[nodiscard]] char ptt_char() const { return ptt_char_; }

    void set_voice_enabled(bool enabled) { voice_enabled_ = enabled; }
    void set_recording(bool recording) { is_recording_ = recording; }

    /// Get the recommended fallback timeout for modifier-combo first-press.
    /// TS REF: useVoiceIntegration.tsx:588
    [[nodiscard]] int modifier_first_press_fallback_ms() const {
        return MODIFIER_FIRST_PRESS_FALLBACK_MS;
    }

private:
    bool matches_binding(bool ctrl, bool meta, bool alt, bool shift, char key) const {
        if (std::tolower(static_cast<unsigned char>(key)) !=
            std::tolower(static_cast<unsigned char>(ptt_char_))) {
            return false;
        }
        if (has_modifier_) {
            return (ctrl == ptt_ctrl_) && (meta == ptt_meta_) &&
                   (alt == ptt_alt_) && (shift == ptt_shift_);
        }
        // Bare char: reject if any modifier is pressed
        return !(ctrl || meta || alt || shift);
    }

    char ptt_char_{' '};
    bool ptt_ctrl_{false};
    bool ptt_meta_{false};
    bool ptt_alt_{false};
    bool ptt_shift_{false};
    bool has_modifier_{false};

    bool voice_enabled_{true};
    bool is_recording_{false};
    bool is_hold_active_{false};
    bool is_warming_up_{false};

    int rapid_count_{0};
    int chars_in_input_{0};
    int trailing_strip_count_{0};

    std::chrono::steady_clock::time_point last_key_time_{};
};

// ============================================================================
// StripTrailing helpers — TS REF: useVoiceIntegration.tsx:152-198
// ============================================================================

/// Options for strip_trailing_hold_chars.
struct StripOpts {
    char ch{' '};              // Character to strip (the configured hold key)
    bool anchor{false};        // Capture voice prefix/suffix anchor at stripped position
    int floor{0};              // Minimum trailing count to leave behind
};

/// Strip trailing hold-key characters from the input text.
/// Returns the number of trailing chars remaining after stripping.
/// When anchor is true, captures prefix/suffix around the cursor for interim
/// transcript placement.
/// TS REF: useVoiceIntegration.tsx:152-198
struct StripResult {
    std::string new_text;
    std::size_t cursor_pos;
    int remaining;
};

[[nodiscard]] StripResult strip_trailing_hold_chars(
        std::string_view input_text,
        std::size_t cursor_offset,
        int max_strip,
        const StripOpts& opts = {}) {
    auto before_cursor = std::string(input_text.substr(0, cursor_offset));
    auto after_cursor = std::string(input_text.substr(cursor_offset));

    // Count trailing instances of opts.ch in before_cursor
    int trailing = 0;
    while (trailing < static_cast<int>(before_cursor.size()) &&
           before_cursor[before_cursor.size() - 1 - trailing] == opts.ch) {
        trailing++;
    }

    int strip_count = std::max(0, std::min(trailing - opts.floor, max_strip));
    int remaining = trailing - strip_count;
    auto stripped = before_cursor.substr(0, before_cursor.size() - strip_count);

    // When anchoring with a non-space suffix, insert a gap space so the
    // waveform cursor sits on the gap.
    // TS REF: useVoiceIntegration.tsx:176-188
    std::string gap;
    if (opts.anchor && !after_cursor.empty() &&
        after_cursor.front() != ' ' && after_cursor.front() != '\t') {
        gap = " ";
    }

    std::string new_text = stripped + gap + after_cursor;
    std::size_t cursor_pos = stripped.size() + gap.size();

    return {new_text, cursor_pos, remaining};
}

// ============================================================================
// InterimRange computation — TS REF: useVoiceIntegration.tsx:328-340
// ============================================================================

/// Compute the character range of interim transcript text within the full
/// input value (prefix + leading_space + interim + trailing_space + suffix).
/// Returns nullopt if there is no interim text.
/// TS REF: useVoiceIntegration.tsx:328-340
[[nodiscard]] std::optional<InterimRange> compute_interim_range(
        std::string_view prefix,
        std::string_view interim_transcript) {
    if (interim_transcript.empty()) return std::nullopt;

    bool needs_space = !prefix.empty() &&
                       !prefix.empty() && prefix.back() != ' ' &&
                       !interim_transcript.empty();
    std::size_t start = prefix.size() + (needs_space ? 1 : 0);
    std::size_t end = start + interim_transcript.size();
    return InterimRange{start, end};
}

// ============================================================================
// VoiceSessionManager — TS REF: useVoice.ts full hook orchestration
// ============================================================================

/// High-level voice session manager that coordinates VoiceRecorder,
/// PushToTalkHandler, and VoicePlayback with the REPL UI integration.
///
/// This is the C++ equivalent of the combined useVoice + useVoiceIntegration
/// hooks, providing a single integration point for the REPL screen.
///
/// TS REF: useVoice.ts:199-1144 (useVoice)
/// TS REF: useVoiceIntegration.tsx:118-347 (useVoiceIntegration)
class VoiceSessionManager {
public:
    VoiceSessionManager() {
        // Wire up internal callbacks
        recorder_.on_transcript(
            [this](std::string_view text, bool is_final) {
                if (is_final && on_final_transcript_) {
                    on_final_transcript_(text);
                }
            });
        recorder_.on_interim_text(
            [this](std::string_view text) {
                if (on_interim_update_) on_interim_update_(text);
            });
        recorder_.on_error(
            [this](const VoiceError& err) {
                if (on_error_) on_error_(err);
            });
        recorder_.on_state_change(
            [this](VoiceState new_state) {
                ptt_.on_voice_state_changed(new_state);
                if (on_state_change_) on_state_change_(new_state);
            });
        recorder_.on_levels_update(
            [this](const std::vector<float>& levels) {
                if (on_levels_update_) on_levels_update_(levels);
            });
    }

    /// ── Hold-to-talk key event ────────────────────────────────────────
    /// Called on every keypress (including terminal auto-repeats).
    /// TS REF: useVoice.ts:1022-1127 (handleKeyEvent)
    void handle_key_event(bool ctrl, bool meta, bool alt, bool shift,
                          char key, bool is_auto_repeat) {
        if (!voice_enabled_) return;
        if (!VoiceRecorder::is_stt_available()) return;

        auto action = ptt_.handle_key_down(ctrl, meta, alt, shift, key, is_auto_repeat);

        switch (action) {
            case PushToTalkHandler::KeyAction::Activate: {
                auto result = recorder_.start_recording(language_hint_);
                if (!result) {
                    ptt_.on_voice_state_changed(VoiceState::Idle);
                    if (on_error_) on_error_(result.error());
                } else {
                    ptt_.set_recording(true);
                }
                break;
            }
            case PushToTalkHandler::KeyAction::Swallow:
                // Forward to release detection
                check_key_release();
                break;
            case PushToTalkHandler::KeyAction::PassThrough:
            case PushToTalkHandler::KeyAction::Warmup:
                break;
        }
    }

    /// Called externally to signal that the key was released (gap > RELEASE_TIMEOUT).
    /// TS REF: useVoice.ts:1094-1124 (release timer arming logic)
    void on_key_timeout() {
        if (recorder_.is_recording()) {
            auto result = recorder_.stop_recording();
            ptt_.set_recording(false);
            if (result && on_final_transcript_) {
                on_final_transcript_(*result);
            }
        }
    }

    /// ── Focus mode ────────────────────────────────────────────────────
    /// Start recording when terminal gains focus (focus mode).
    /// TS REF: useVoice.ts:576-630
    void on_terminal_focus_gained() {
        if (!focus_mode_ || !voice_enabled_) return;
        if (recorder_.is_recording()) return;
        auto result = recorder_.start_recording(language_hint_);
        if (!result && on_error_) on_error_(result.error());
    }

    /// Stop recording when terminal loses focus (focus mode).
    void on_terminal_focus_lost() {
        if (!focus_mode_) return;
        if (recorder_.is_recording()) {
            auto result = recorder_.stop_recording();
            if (result && on_final_transcript_) on_final_transcript_(*result);
        }
    }

    /// ── Configuration ─────────────────────────────────────────────────
    void set_voice_enabled(bool enabled) {
        voice_enabled_ = enabled;
        if (!enabled && recorder_.is_recording()) {
            auto result = recorder_.stop_recording();
            ptt_.set_recording(false);
        }
    }

    void set_focus_mode(bool enabled) { focus_mode_ = enabled; }

    void set_language_hint(std::string_view lang) { language_hint_ = lang; }

    void set_ptt_key(char c) { ptt_.set_ptt_bare_char(c); }

    void set_ptt_modifier_combo(bool ctrl, bool meta, bool alt, bool shift, char key) {
        ptt_.set_ptt_modifier_combo(ctrl, meta, alt, shift, key);
    }

    /// ── State queries ─────────────────────────────────────────────────
    [[nodiscard]] VoiceIntegrationState get_integration_state() const {
        VoiceIntegrationState s;
        s.is_recording = recorder_.is_recording();
        s.is_playing = playback_.is_playing();
        s.is_warming_up = ptt_.is_warming_up();
        s.interim_text = recorder_.get_interim_text();
        s.volume_level = !recorder_.get_audio_levels().empty()
                             ? recorder_.get_audio_levels().back()
                             : 0.0f;
        s.audio_levels = recorder_.get_audio_levels();
        s.recording_duration = recorder_.get_recording_duration();
        return s;
    }

    [[nodiscard]] bool is_voice_enabled() const { return voice_enabled_; }
    [[nodiscard]] bool is_focus_mode() const { return focus_mode_; }

    /// ── Playback ──────────────────────────────────────────────────────
    void speak(std::string_view text, float speed = 1.0f) {
        playback_.play(text, speed);
    }

    void stop_speaking() { playback_.stop_playback(); }

    /// ── Callbacks ─────────────────────────────────────────────────────
    using FinalTranscriptCallback = std::function<void(std::string_view text)>;
    using InterimUpdateCallback = std::function<void(std::string_view text)>;
    using ErrorCallback = std::function<void(const VoiceError& err)>;
    using StateChangeCallback = std::function<void(VoiceState new_state)>;
    using LevelsCallback = std::function<void(const std::vector<float>& levels)>;

    void on_final_transcript(FinalTranscriptCallback cb) {
        on_final_transcript_ = std::move(cb);
    }
    void on_interim_update(InterimUpdateCallback cb) {
        on_interim_update_ = std::move(cb);
    }
    void on_error(ErrorCallback cb) { on_error_ = std::move(cb); }
    void on_state_change(StateChangeCallback cb) { on_state_change_ = std::move(cb); }
    void on_levels_update(LevelsCallback cb) { on_levels_update_ = std::move(cb); }

    /// Direct access to the recorder (for advanced use cases).
    VoiceRecorder& recorder() { return recorder_; }
    const VoiceRecorder& recorder() const { return recorder_; }

    /// Direct access to the PTT handler (for key event integration).
    PushToTalkHandler& ptt() { return ptt_; }

    /// Direct access to playback (for TTS queue management).
    VoicePlayback& playback() { return playback_; }

    /// Release timeout interval for the caller's timer (milliseconds).
    /// TS REF: useVoice.ts:160
    static constexpr int release_timeout_ms() { return RELEASE_TIMEOUT_MS; }

    /// Repeat fallback interval for the caller's timer (milliseconds).
    /// TS REF: useVoice.ts:171
    static constexpr int repeat_fallback_ms() { return REPEAT_FALLBACK_MS; }

private:
    void check_key_release() {
        // The caller (REPL screen event loop) is responsible for tracking
        // key gaps and calling on_key_timeout(). This method provides a
        // hook for future inline release detection if needed.
    }

    VoiceRecorder recorder_;
    VoicePlayback playback_;
    PushToTalkHandler ptt_;

    bool voice_enabled_{true};
    bool focus_mode_{false};
    std::string language_hint_;

    FinalTranscriptCallback on_final_transcript_;
    InterimUpdateCallback on_interim_update_;
    ErrorCallback on_error_;
    StateChangeCallback on_state_change_;
    LevelsCallback on_levels_update_;
};

// ============================================================================
// Convenience: format voice error for display
// ============================================================================

/// Format a VoiceError into a human-readable string suitable for notification.
[[nodiscard]] std::string format_voice_error(const VoiceError& err) {
    if (!err.is_error()) return {};
    return err.message;
}

/// Get the error category string for analytics/logging.
[[nodiscard]] std::string_view voice_error_category(VoiceErrorType type) {
    switch (type) {
        case VoiceErrorType::None:               return "none";
        case VoiceErrorType::MicPermissionDenied: return "mic_permission_denied";
        case VoiceErrorType::MicNotFound:        return "mic_not_found";
        case VoiceErrorType::SttServiceUnavailable: return "stt_service_unavailable";
        case VoiceErrorType::NetworkError:       return "network_error";
        case VoiceErrorType::NoSpeechDetected:   return "no_speech_detected";
        case VoiceErrorType::NoAudioSignal:      return "no_audio_signal";
        case VoiceErrorType::StreamError:        return "stream_error";
        case VoiceErrorType::ModuleNotLoaded:    return "module_not_loaded";
        case VoiceErrorType::AlreadyRecording:   return "already_recording";
        case VoiceErrorType::NotRecording:       return "not_recording";
    }
    return "unknown";
}

} // namespace cc::hooks::voice
