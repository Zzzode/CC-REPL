module;
#include <string>
#include <sstream>
#include <array>
#include <cmath>
#include <algorithm>

export module cc.ui.prompt.voice_indicator;

export namespace cc::ui::prompt {

// States of the voice input system
enum class VoiceState { Idle, Listening, Processing, Speaking };

// Render a voice state indicator with animation
inline auto render_voice_indicator(VoiceState state, int frame) -> std::string {
    std::ostringstream out;

    switch (state) {
        case VoiceState::Idle:
            out << "\033[2m🎤\033[0m";
            break;

        case VoiceState::Listening: {
            // Pulsing microphone icon
            static constexpr std::array<const char*, 4> pulse = {"🎤", "🎤 ", "🎤  ", "🎤 "};
            int idx = (frame / 4) % 4;
            out << "\033[31m" << pulse[idx] << "\033[0m";
            // Audio wave animation
            static constexpr std::array<const char*, 4> waves = {"▁▃▅▇", "▃▅▇▅", "▅▇▅▃", "▇▅▃▁"};
            int wave_idx = (frame / 2) % 4;
            out << " \033[31m" << waves[wave_idx] << "\033[0m";
            break;
        }

        case VoiceState::Processing: {
            // Spinner while processing audio
            static constexpr std::array<const char*, 8> spinner = {
                "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧"
            };
            int idx = frame % 8;
            out << "\033[33m🎤 " << spinner[idx] << " Processing...\033[0m";
            break;
        }

        case VoiceState::Speaking: {
            // Speaking/output animation
            static constexpr std::array<const char*, 3> speak = {"🔊", "🔊 )", "🔊 ))"};
            int idx = (frame / 3) % 3;
            out << "\033[32m" << speak[idx] << "\033[0m";
            break;
        }
    }

    return out.str();
}

// Render an audio level meter (VU meter style)
inline auto render_audio_level(float level, int width) -> std::string {
    // Clamp level to [0, 1]
    level = std::max(0.0f, std::min(1.0f, level));

    std::ostringstream out;
    int filled = static_cast<int>(level * width);

    for (int i = 0; i < width; ++i) {
        float threshold = static_cast<float>(i) / width;
        if (i < filled) {
            // Color gradient: green -> yellow -> red
            if (threshold < 0.6f) {
                out << "\033[32m▮\033[0m"; // green
            } else if (threshold < 0.8f) {
                out << "\033[33m▮\033[0m"; // yellow
            } else {
                out << "\033[31m▮\033[0m"; // red
            }
        } else {
            out << "\033[2m▯\033[0m"; // empty
        }
    }

    return out.str();
}

} // namespace cc::ui::prompt
