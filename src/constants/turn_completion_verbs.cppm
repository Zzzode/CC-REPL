// C++23 module: Past tense verbs for turn completion messages.
// These verbs work naturally with "for [duration]" (e.g., "Worked for 5s").
module;
#include <array>
#include <string>
#include <string_view>

export module cc.constants.turn_completion_verbs;


export namespace cc::constants::turn_completion_verbs {

inline constexpr std::array<std::string_view, 8> turn_completion_verbs = {
    "Baked",
    "Brewed",
    "Churned",
    "Cogitated",
    "Cooked",
    "Crunched",
    "Sautéed",
    "Worked",
};

} // namespace cc::constants::turn_completion_verbs
