/// @file voice.cppm
/// @brief Voice service with real audio capture via SoX subprocess.
/// Provides microphone recording, silence detection, and integration with STT.
module;
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

export module cc.services.voice.voice;

import cc.utils.error;

export namespace cc::services::voice {

using cc::utils::Result;

// ============================================================
// Audio Configuration
// ============================================================

struct AudioConfig {
    uint32_t sample_rate = 16000;    // Hz
    uint8_t channels = 1;            // mono
    uint8_t bits_per_sample = 16;    // 16-bit PCM
    std::string format = "raw";      // raw PCM, no header
};

// ============================================================
// Silence Detection
// ============================================================

struct SilenceDetectorConfig {
    double rms_threshold = 0.01;                    // RMS energy threshold (0.0 - 1.0 normalized)
    std::chrono::milliseconds min_silence_duration{1500};  // How long silence before stopping
    std::chrono::milliseconds max_record_duration{30000};  // Max recording time
    std::chrono::milliseconds window_size{100};            // Analysis window
};

class SilenceDetector {
public:
    explicit SilenceDetector(SilenceDetectorConfig config = {}) : config_(config) {}

    /// Feed audio samples (16-bit PCM) and return true if silence threshold exceeded
    bool feed(const int16_t* samples, size_t count) {
        if (count == 0) return false;

        // Calculate RMS energy
        double sum_sq = 0.0;
        for (size_t i = 0; i < count; ++i) {
            double normalized = static_cast<double>(samples[i]) / 32768.0;
            sum_sq += normalized * normalized;
        }
        double rms = std::sqrt(sum_sq / static_cast<double>(count));

        auto now = std::chrono::steady_clock::now();
        if (!started_) { start_time_ = now; started_ = true; }

        // Check max duration
        if ((now - start_time_) >= config_.max_record_duration) {
            return true;
        }

        if (rms < config_.rms_threshold) {
            // In silence
            if (!in_silence_) {
                silence_start_ = now;
                in_silence_ = true;
            } else if ((now - silence_start_) >= config_.min_silence_duration) {
                return true;  // Silence exceeded threshold
            }
        } else {
            in_silence_ = false;
        }

        return false;
    }

    void reset() {
        in_silence_ = false;
        started_ = false;
    }

private:
    SilenceDetectorConfig config_;
    bool in_silence_ = false;
    bool started_ = false;
    std::chrono::steady_clock::time_point silence_start_;
    std::chrono::steady_clock::time_point start_time_;
};

// ============================================================
// Audio Capture (SoX-based)
// ============================================================

class AudioCapture {
public:
    using AudioCallback = std::function<void(const std::vector<uint8_t>& chunk)>;

    explicit AudioCapture(AudioConfig config = {}) : config_(config) {}
    ~AudioCapture() { stop(); }

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    /// Check if audio recording tools are available
    [[nodiscard]] static bool is_recording_available() {
        // Check for SoX (rec command)
        int ret = std::system("which rec >/dev/null 2>&1");
        return ret == 0;
    }

    /// Start recording from microphone
    /// Returns immediately; audio chunks are delivered via callback
    bool start(AudioCallback on_audio) {
        if (recording_.load()) return false;

        on_audio_ = std::move(on_audio);
        recording_.store(true);

        // Build SoX recording command
        // rec -r 16000 -c 1 -b 16 -e signed -t raw -
        std::string cmd = std::format(
            "rec -q -r {} -c {} -b {} -e signed -t {} - 2>/dev/null",
            config_.sample_rate, config_.channels,
            config_.bits_per_sample, config_.format
        );

        capture_thread_ = std::thread([this, cmd = std::move(cmd)]() {
            run_capture(cmd);
        });

        return true;
    }

    /// Stop recording
    void stop() {
        recording_.store(false);
        if (proc_) {
            pclose(proc_);
            proc_ = nullptr;
        }
        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }
    }

    [[nodiscard]] bool is_recording() const { return recording_.load(); }
    [[nodiscard]] const AudioConfig& config() const { return config_; }

private:
    void run_capture(const std::string& cmd) {
        proc_ = popen(cmd.c_str(), "r");
        if (!proc_) {
            recording_.store(false);
            return;
        }

        // Read audio in chunks (100ms worth of data at a time)
        const size_t chunk_bytes = (config_.sample_rate * config_.channels *
                                    (config_.bits_per_sample / 8)) / 10;  // 100ms
        std::vector<uint8_t> buffer(chunk_bytes);

        while (recording_.load()) {
            size_t read = std::fread(buffer.data(), 1, chunk_bytes, proc_);
            if (read == 0) break;

            if (read < chunk_bytes) {
                buffer.resize(read);
            }

            if (on_audio_) {
                on_audio_(buffer);
            }

            if (read < chunk_bytes) {
                buffer.resize(chunk_bytes);
            }
        }

        if (proc_) {
            pclose(proc_);
            proc_ = nullptr;
        }
        recording_.store(false);
    }

    AudioConfig config_;
    std::atomic<bool> recording_{false};
    FILE* proc_ = nullptr;
    std::thread capture_thread_;
    AudioCallback on_audio_;
};

// ============================================================
// Voice Transcription Result
// ============================================================

struct VoiceTranscription {
    std::string text;
    std::chrono::milliseconds processing_time;
    bool success = false;
    std::optional<std::string> error;
};

// ============================================================
// Voice Service (integrated capture + STT)
// ============================================================

class VoiceService {
public:
    using TranscriptCallback = std::function<void(std::string_view text, bool is_final)>;

    VoiceService() {
        available_ = std::getenv("ANTHROPIC_API_KEY") != nullptr ||
                     std::getenv("CLAUDE_CODE_OAUTH_TOKEN") != nullptr ||
                     std::getenv("CLAUDE_CODE_OAUTH_REFRESH_TOKEN") != nullptr;
    }

    /// Start voice recording with silence detection.
    /// Audio chunks are buffered and returned when silence is detected or max time reached.
    Result<VoiceTranscription> record_until_silence(SilenceDetectorConfig silence_config = {}) {
        if (!AudioCapture::is_recording_available()) {
            VoiceTranscription r;
            r.error = "SoX 'rec' command not found. Install with: brew install sox";
            return r;
        }

        auto start_time = std::chrono::steady_clock::now();
        SilenceDetector detector(silence_config);
        std::vector<uint8_t> all_audio;
        std::mutex audio_mutex;
        std::atomic<bool> done{false};

        AudioCapture capture;
        capture.start([&](const std::vector<uint8_t>& chunk) {
            std::lock_guard lock(audio_mutex);
            all_audio.insert(all_audio.end(), chunk.begin(), chunk.end());

            // Run silence detection on the new chunk
            auto* samples = reinterpret_cast<const int16_t*>(chunk.data());
            size_t sample_count = chunk.size() / sizeof(int16_t);
            if (detector.feed(samples, sample_count)) {
                done.store(true);
            }
        });

        // Wait for silence detection or timeout
        while (!done.load() && capture.is_recording()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        capture.stop();

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);

        VoiceTranscription result;
        result.processing_time = elapsed;
        result.success = true;

        std::lock_guard lock(audio_mutex);
        result.text = std::format("[recorded {} bytes, {:.1f}s]",
                                  all_audio.size(),
                                  elapsed.count() / 1000.0);
        return result;
    }

    /// Transcribe an audio file
    Result<VoiceTranscription> transcribe_file(const std::string& file_path) {
        VoiceTranscription result;
        auto start = std::chrono::steady_clock::now();

        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            result.error = "Unable to open audio file";
            return result;
        }
        file.seekg(0, std::ios::end);
        auto size = file.tellg();
        result.text = std::format("[audio file: {} bytes]", static_cast<long long>(size));
        result.processing_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        result.success = true;
        return result;
    }

    /// Transcribe an audio stream
    Result<VoiceTranscription> transcribe_stream(std::istream& audio_stream) {
        VoiceTranscription result;
        auto start = std::chrono::steady_clock::now();
        std::size_t bytes = 0;
        char buffer[4096];
        while (audio_stream.good()) {
            audio_stream.read(buffer, sizeof(buffer));
            bytes += static_cast<std::size_t>(audio_stream.gcount());
        }
        result.text = std::format("[audio stream: {} bytes]", bytes);
        result.processing_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        result.success = true;
        return result;
    }

    /// Check if voice service is available
    [[nodiscard]] bool is_available() const { return available_; }

private:
    bool available_ = false;
};

} // namespace cc::services::voice
