/// @file voice.cppm
/// @brief Voice input context state.
/// Migrated from src/context/voice.tsx
module;

#include <string>
#include <optional>
#include <functional>

export module cc.context.voice;

export namespace cc::context {

/// Voice input state
enum class VoiceState : std::uint8_t {
    Idle,
    Listening,
    Processing,
    Error,
};

/// Voice context state
struct VoiceContextState {
    VoiceState state = VoiceState::Idle;
    std::optional<std::string> transcript;
    std::optional<std::string> error_message;
    double volume_level = 0.0;
};

} // namespace cc::context
