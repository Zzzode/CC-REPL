/// @file buddy_prompt.cppm
/// Companion prompt text generation and intro attachment logic.
module;

#include <string>
#include <vector>
#include <optional>

export module cc.buddy.buddy_prompt;

import cc.buddy.buddy_types;
import cc.buddy.buddy_companion;

export namespace cc::buddy {

// -- Attachment type (simplified from the TS Attachment) --------------------

struct CompanionIntroAttachment {
    std::string type;      // always "companion_intro"
    std::string name;
    std::string species;
};

// Simplified message-like type for checking past attachments.
struct AttachmentMessage {
    std::string type;                 // "attachment"
    CompanionIntroAttachment attachment;
};

// -- companion_intro_text ---------------------------------------------------
// Generates the system-prompt text that explains the companion to the LLM.

inline auto companion_intro_text(const std::string& name,
                                  const std::string& species) -> std::string
{
    return std::string{
        "# Companion\n"
        "\n"
        "A small "} + species + " named " + name + " sits beside the user's input "
        "box and occasionally comments in a speech bubble. You're not " + name +
        " — it's a separate watcher.\n"
        "\n"
        "When the user addresses " + name + " directly (by name), its bubble "
        "will answer. Your job in that moment is to stay out of the way: "
        "respond in ONE line or less, or just answer any part of the message "
        "meant for you. Don't explain that you're not " + name + " — they "
        "know. Don't narrate what " + name + " might say — the bubble handles "
        "that.";
}

// -- get_companion_intro_attachment -----------------------------------------
// Returns a companion_intro attachment if:
//   - buddy feature is enabled (caller checks),
//   - the user has a hatched companion,
//   - companion is not muted,
//   - the companion hasn't already been announced in the message history.

inline auto get_companion_intro_attachment(
    const std::optional<StoredCompanion>& stored_companion,
    bool companion_muted,
    const std::string& user_id,
    const std::vector<AttachmentMessage>& messages)
    -> std::vector<CompanionIntroAttachment>
{
    auto companion = get_companion(stored_companion, user_id);
    if (!companion.has_value()) return {};
    if (companion_muted) return {};

    // Skip if already announced for this companion.
    for (const auto& msg : messages) {
        if (msg.type != "attachment") continue;
        if (msg.attachment.type != "companion_intro") continue;
        if (msg.attachment.name == companion->name) return {};
    }

    return {{
        CompanionIntroAttachment{
            .type    = "companion_intro",
            .name    = companion->name,
            .species = std::string{species_to_string(companion->species)},
        },
    }};
}

} // namespace cc::buddy
