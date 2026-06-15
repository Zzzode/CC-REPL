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
#include <expected>
#include <format>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

export module cc.services.voice.voice;

import cc.utils.error;
import cc.utils.bash_execution;

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
            cc::utils::bash::pclose_spawn(proc_);
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
        proc_ = cc::utils::bash::popen_spawn(cmd.c_str());
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
            cc::utils::bash::pclose_spawn(proc_);
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
    using TranscriptionProvider = std::function<Result<std::string>(std::span<const uint8_t> audio)>;

    VoiceService()
        : VoiceService(make_env_command_transcriber()) {}

    explicit VoiceService(TranscriptionProvider transcriber)
        : transcriber_(std::move(transcriber)) {
        available_ = transcriber_ ||
                     std::getenv("ANTHROPIC_API_KEY") != nullptr ||
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

        std::lock_guard lock(audio_mutex);
        if (all_audio.empty()) {
            VoiceTranscription result;
            result.processing_time = elapsed;
            result.error = "No audio data captured";
            return result;
        }
        return transcribe_audio_bytes(std::move(all_audio), start_time);
    }

    /// Transcribe an audio file
    Result<VoiceTranscription> transcribe_file(const std::string& file_path) {
        auto start = std::chrono::steady_clock::now();

        std::ifstream file(file_path, std::ios::binary);
        VoiceTranscription result;
        if (!file) {
            result.error = "Unable to open audio file";
            return result;
        }
        std::vector<uint8_t> audio;
        char byte = 0;
        while (file.get(byte)) {
            audio.push_back(static_cast<uint8_t>(byte));
        }
        return transcribe_audio_bytes(std::move(audio), start);
    }

    /// Transcribe an audio stream
    Result<VoiceTranscription> transcribe_stream(std::istream& audio_stream) {
        auto start = std::chrono::steady_clock::now();
        std::vector<uint8_t> audio;
        char buffer[4096];
        while (audio_stream.good()) {
            audio_stream.read(buffer, sizeof(buffer));
            auto read = audio_stream.gcount();
            if (read > 0) {
                audio.insert(audio.end(), buffer, buffer + read);
            }
        }
        return transcribe_audio_bytes(std::move(audio), start);
    }

    /// Check if voice service is available
    [[nodiscard]] bool is_available() const { return available_; }

private:
    TranscriptionProvider transcriber_;
    bool available_ = false;

    [[nodiscard]] static std::string shell_quote(std::string_view value) {
        std::string out = "'";
        for (char c : value) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    static void replace_all(std::string& text,
                            std::string_view needle,
                            std::string_view replacement) {
        std::size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos) {
            text.replace(pos, needle.size(), replacement);
            pos += replacement.size();
        }
    }

    [[nodiscard]] static std::string read_command_stdout(const std::string& command) {
        std::unique_ptr<FILE, decltype(&pclose)> pipe(cc::utils::bash::popen_spawn(command.c_str()), pclose);
        if (!pipe) return {};
        std::string output;
        char buffer[4096];
        while (auto n = std::fread(buffer, 1, sizeof(buffer), pipe.get())) {
            output.append(buffer, buffer + n);
        }
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
            output.pop_back();
        }
        return output;
    }

    [[nodiscard]] static TranscriptionProvider make_env_command_transcriber() {
        auto* command_env = std::getenv("CC_REPL_VOICE_TRANSCRIBE_CMD");
        if (!command_env || std::string_view(command_env).empty()) {
            return {};
        }
        std::string command_template = command_env;
        return [command_template](std::span<const uint8_t> audio) -> Result<std::string> {
            auto path = std::filesystem::temp_directory_path() /
                std::format("cc-repl-voice-{}.raw",
                    std::chrono::steady_clock::now().time_since_epoch().count());
            {
                std::ofstream out(path, std::ios::binary);
                if (!out) {
                    return std::unexpected(cc::utils::make_error(
                        cc::utils::ErrorCode::io_error,
                        "Unable to create temporary audio file"));
                }
                out.write(reinterpret_cast<const char*>(audio.data()),
                          static_cast<std::streamsize>(audio.size()));
            }
            std::string command = command_template;
            auto quoted_path = shell_quote(path.string());
            if (command.find("{file}") != std::string::npos) {
                replace_all(command, "{file}", quoted_path);
            } else {
                command += " ";
                command += quoted_path;
            }
            auto transcript = read_command_stdout(command);
            std::error_code ec;
            std::filesystem::remove(path, ec);
            if (transcript.empty()) {
                return std::unexpected(cc::utils::make_error(
                    cc::utils::ErrorCode::unavailable,
                    "Voice transcription command returned no transcript"));
            }
            return transcript;
        };
    }

    [[nodiscard]] Result<VoiceTranscription> transcribe_audio_bytes(
        std::vector<uint8_t> audio,
        std::chrono::steady_clock::time_point start) const {
        VoiceTranscription result;
        result.processing_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        if (audio.empty()) {
            result.error = "No audio data to transcribe";
            return result;
        }
        if (!transcriber_) {
            result.error = "No voice transcription provider is configured";
            return result;
        }

        auto transcript = transcriber_(std::span<const uint8_t>(audio.data(), audio.size()));
        if (!transcript) {
            result.error = transcript.error().message();
            return result;
        }

        result.text = std::move(*transcript);
        result.success = true;
        result.processing_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        return result;
    }
};

} // namespace cc::services::voice
