/// @file buddy_sprites.cppm
/// ASCII sprite rendering for companion species, hats, and faces.
module;

#include <string>
#include <vector>
#include <array>
#include <algorithm>

export module cc.buddy.buddy_sprites;

import cc.buddy.buddy_types;

export namespace cc::buddy {

// -- Sprite body definitions ------------------------------------------------
// Each species has 3 frames, each frame is 5 lines of ASCII art.
// Line 0 is the hat slot — must be blank in frames 0-1; frame 2 may use it.
// {E} is replaced at render time with the companion's eye character.

namespace sprites {

// ---- duck ----------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> duck = {{
    {{
        "            ",
        "    __      ",
        "  <({E} )___  ",
        "   (  ._>   ",
        "    `--´    ",
    }},
    {{
        "            ",
        "    __      ",
        "  <({E} )___  ",
        "   (  ._>   ",
        "    `--´~   ",
    }},
    {{
        "            ",
        "    __      ",
        "  <({E} )___  ",
        "   (  .__>  ",
        "    `--´    ",
    }},
}};

// ---- goose ---------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> goose = {{
    {{
        "            ",
        "     ({E}>    ",
        "     ||     ",
        "   _(__)_   ",
        "    ^^^^    ",
    }},
    {{
        "            ",
        "    ({E}>     ",
        "     ||     ",
        "   _(__)_   ",
        "    ^^^^    ",
    }},
    {{
        "            ",
        "     ({E}>>   ",
        "     ||     ",
        "   _(__)_   ",
        "    ^^^^    ",
    }},
}};

// ---- blob ----------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> blob = {{
    {{
        "            ",
        "   .----.   ",
        "  ( {E}  {E} )  ",
        "  (      )  ",
        "   `----´   ",
    }},
    {{
        "            ",
        "  .------.  ",
        " (  {E}  {E}  ) ",
        " (        ) ",
        "  `------´  ",
    }},
    {{
        "            ",
        "    .--.    ",
        "   ({E}  {E})   ",
        "   (    )   ",
        "    `--´    ",
    }},
}};

// ---- cat -----------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> cat = {{
    {{
        "            ",
        "   /\\_/\\    ",
        "  ( {E}   {E})  ",
        "  (  ω  )   ",
        "  (\")_(\")   ",
    }},
    {{
        "            ",
        "   /\\_/\\    ",
        "  ( {E}   {E})  ",
        "  (  ω  )   ",
        "  (\")_(\")~  ",
    }},
    {{
        "            ",
        "   /\\-/\\    ",
        "  ( {E}   {E})  ",
        "  (  ω  )   ",
        "  (\")_(\")   ",
    }},
}};

// ---- dragon --------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> dragon = {{
    {{
        "            ",
        "  /^\\  /^\\  ",
        " <  {E}  {E}  > ",
        " (   ~~   ) ",
        "  `-vvvv-´  ",
    }},
    {{
        "            ",
        "  /^\\  /^\\  ",
        " <  {E}  {E}  > ",
        " (        ) ",
        "  `-vvvv-´  ",
    }},
    {{
        "   ~    ~   ",
        "  /^\\  /^\\  ",
        " <  {E}  {E}  > ",
        " (   ~~   ) ",
        "  `-vvvv-´  ",
    }},
}};

// ---- octopus -------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> octopus = {{
    {{
        "            ",
        "   .----.   ",
        "  ( {E}  {E} )  ",
        "  (______)  ",
        "  /\\/\\/\\/\\  ",
    }},
    {{
        "            ",
        "   .----.   ",
        "  ( {E}  {E} )  ",
        "  (______)  ",
        "  \\/\\/\\/\\/  ",
    }},
    {{
        "     o      ",
        "   .----.   ",
        "  ( {E}  {E} )  ",
        "  (______)  ",
        "  /\\/\\/\\/\\  ",
    }},
}};

// ---- owl -----------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> owl = {{
    {{
        "            ",
        "   /\\  /\\   ",
        "  (({E})({E}))  ",
        "  (  ><  )  ",
        "   `----´   ",
    }},
    {{
        "            ",
        "   /\\  /\\   ",
        "  (({E})({E}))  ",
        "  (  ><  )  ",
        "   .----.   ",
    }},
    {{
        "            ",
        "   /\\  /\\   ",
        "  (({E})(-))  ",
        "  (  ><  )  ",
        "   `----´   ",
    }},
}};

// ---- penguin -------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> penguin = {{
    {{
        "            ",
        "  .---.     ",
        "  ({E}>{E})     ",
        " /(   )\\    ",
        "  `---´     ",
    }},
    {{
        "            ",
        "  .---.     ",
        "  ({E}>{E})     ",
        " |(   )|    ",
        "  `---´     ",
    }},
    {{
        "  .---.     ",
        "  ({E}>{E})     ",
        " /(   )\\    ",
        "  `---´     ",
        "   ~ ~      ",
    }},
}};

// ---- turtle --------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> turtle = {{
    {{
        "            ",
        "   _,--._   ",
        "  ( {E}  {E} )  ",
        " /[______]\\ ",
        "  ``    ``  ",
    }},
    {{
        "            ",
        "   _,--._   ",
        "  ( {E}  {E} )  ",
        " /[______]\\ ",
        "   ``  ``   ",
    }},
    {{
        "            ",
        "   _,--._   ",
        "  ( {E}  {E} )  ",
        " /[======]\\ ",
        "  ``    ``  ",
    }},
}};

// ---- snail ---------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> snail = {{
    {{
        "            ",
        " {E}    .--.  ",
        "  \\  ( @ )  ",
        "   \\_`--´   ",
        "  ~~~~~~~   ",
    }},
    {{
        "            ",
        "  {E}   .--.  ",
        "  |  ( @ )  ",
        "   \\_`--´   ",
        "  ~~~~~~~   ",
    }},
    {{
        "            ",
        " {E}    .--.  ",
        "  \\  ( @  ) ",
        "   \\_`--´   ",
        "   ~~~~~~   ",
    }},
}};

// ---- ghost ---------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> ghost = {{
    {{
        "            ",
        "   .----.   ",
        "  / {E}  {E} \\  ",
        "  |      |  ",
        "  ~`~``~`~  ",
    }},
    {{
        "            ",
        "   .----.   ",
        "  / {E}  {E} \\  ",
        "  |      |  ",
        "  `~`~~`~`  ",
    }},
    {{
        "    ~  ~    ",
        "   .----.   ",
        "  / {E}  {E} \\  ",
        "  |      |  ",
        "  ~~`~~`~~  ",
    }},
}};

// ---- axolotl -------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> axolotl = {{
    {{
        "            ",
        "}~(______)~{",
        "}~({E} .. {E})~{",
        "  ( .--. )  ",
        "  (_/  \\_)  ",
    }},
    {{
        "            ",
        "~}(______){~",
        "~}({E} .. {E}){~",
        "  ( .--. )  ",
        "  (_/  \\_)  ",
    }},
    {{
        "            ",
        "}~(______)~{",
        "}~({E} .. {E})~{",
        "  (  --  )  ",
        "  ~_/  \\_~  ",
    }},
}};

// ---- capybara ------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> capybara = {{
    {{
        "            ",
        "  n______n  ",
        " ( {E}    {E} ) ",
        " (   oo   ) ",
        "  `------´  ",
    }},
    {{
        "            ",
        "  n______n  ",
        " ( {E}    {E} ) ",
        " (   Oo   ) ",
        "  `------´  ",
    }},
    {{
        "    ~  ~    ",
        "  u______n  ",
        " ( {E}    {E} ) ",
        " (   oo   ) ",
        "  `------´  ",
    }},
}};

// ---- cactus --------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> cactus = {{
    {{
        "            ",
        " n  ____  n ",
        " | |{E}  {E}| | ",
        " |_|    |_| ",
        "   |    |   ",
    }},
    {{
        "            ",
        "    ____    ",
        " n |{E}  {E}| n ",
        " |_|    |_| ",
        "   |    |   ",
    }},
    {{
        " n        n ",
        " |  ____  | ",
        " | |{E}  {E}| | ",
        " |_|    |_| ",
        "   |    |   ",
    }},
}};

// ---- robot ---------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> robot = {{
    {{
        "            ",
        "   .[||].   ",
        "  [ {E}  {E} ]  ",
        "  [ ==== ]  ",
        "  `------´  ",
    }},
    {{
        "            ",
        "   .[||].   ",
        "  [ {E}  {E} ]  ",
        "  [ -==- ]  ",
        "  `------´  ",
    }},
    {{
        "     *      ",
        "   .[||].   ",
        "  [ {E}  {E} ]  ",
        "  [ ==== ]  ",
        "  `------´  ",
    }},
}};

// ---- rabbit --------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> rabbit = {{
    {{
        "            ",
        "   (\\__/)   ",
        "  ( {E}  {E} )  ",
        " =(  ..  )= ",
        "  (\")__(\")  ",
    }},
    {{
        "            ",
        "   (|__/)   ",
        "  ( {E}  {E} )  ",
        " =(  ..  )= ",
        "  (\")__(\")  ",
    }},
    {{
        "            ",
        "   (\\__/)   ",
        "  ( {E}  {E} )  ",
        " =( .  . )= ",
        "  (\")__(\")  ",
    }},
}};

// ---- mushroom ------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> mushroom = {{
    {{
        "            ",
        " .-o-OO-o-. ",
        "(__________)",
        "   |{E}  {E}|   ",
        "   |____|   ",
    }},
    {{
        "            ",
        " .-O-oo-O-. ",
        "(__________)",
        "   |{E}  {E}|   ",
        "   |____|   ",
    }},
    {{
        "   . o  .   ",
        " .-o-OO-o-. ",
        "(__________)",
        "   |{E}  {E}|   ",
        "   |____|   ",
    }},
}};

// ---- chonk ---------------------------------------------------------------
inline const std::array<std::array<std::string, 5>, 3> chonk = {{
    {{
        "            ",
        "  /\\    /\\  ",
        " ( {E}    {E} ) ",
        " (   ..   ) ",
        "  `------´  ",
    }},
    {{
        "            ",
        "  /\\    /|  ",
        " ( {E}    {E} ) ",
        " (   ..   ) ",
        "  `------´  ",
    }},
    {{
        "            ",
        "  /\\    /\\  ",
        " ( {E}    {E} ) ",
        " (   ..   ) ",
        "  `------´~ ",
    }},
}};

} // namespace sprites

// -- Species-to-sprite lookup -----------------------------------------------

inline auto get_species_frames(Species species)
    -> const std::array<std::array<std::string, 5>, 3>&
{
    switch (species) {
        case Species::duck:     return sprites::duck;
        case Species::goose:    return sprites::goose;
        case Species::blob:     return sprites::blob;
        case Species::cat:      return sprites::cat;
        case Species::dragon:   return sprites::dragon;
        case Species::octopus:  return sprites::octopus;
        case Species::owl:      return sprites::owl;
        case Species::penguin:  return sprites::penguin;
        case Species::turtle:   return sprites::turtle;
        case Species::snail:    return sprites::snail;
        case Species::ghost:    return sprites::ghost;
        case Species::axolotl:  return sprites::axolotl;
        case Species::capybara: return sprites::capybara;
        case Species::cactus:   return sprites::cactus;
        case Species::robot:    return sprites::robot;
        case Species::rabbit:   return sprites::rabbit;
        case Species::mushroom: return sprites::mushroom;
        case Species::chonk:    return sprites::chonk;
    }
    return sprites::duck;
}

// -- Hat line definitions ---------------------------------------------------

inline auto get_hat_line(Hat hat) -> const char* {
    switch (hat) {
        case Hat::none:      return "";
        case Hat::crown:     return "   \\^^^/    ";
        case Hat::tophat:    return "   [___]    ";
        case Hat::propeller: return "    -+-     ";
        case Hat::halo:      return "   (   )    ";
        case Hat::wizard:    return "    /^\\     ";
        case Hat::beanie:    return "   (___)    ";
        case Hat::tinyduck:  return "    ,>      ";
    }
    return "";
}

// -- String replacement helper ---------------------------------------------

namespace detail {

inline auto replace_all(std::string str, const std::string& from,
                        const std::string& to) -> std::string {
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos) {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
    return str;
}

inline auto is_blank(const std::string& s) -> bool {
    return s.find_first_not_of(' ') == std::string::npos;
}

} // namespace detail

// -- render_sprite ----------------------------------------------------------

inline auto render_sprite(const CompanionBones& bones, int frame = 0)
    -> std::vector<std::string>
{
    const auto& frames = get_species_frames(bones.species);
    int f = frame % static_cast<int>(frames.size());

    // Build body lines with eye substitution
    std::vector<std::string> lines;
    lines.reserve(5);
    for (const auto& line : frames[f]) {
        lines.push_back(detail::replace_all(line, "{E}", bones.eye));
    }

    // Only replace line 0 with hat if it is empty (some fidget frames use it
    // for smoke/antenna effects).
    if (bones.hat != Hat::none && !detail::is_blank(lines[0])) {
        // keep the frame's non-empty line 0
    } else if (bones.hat != Hat::none) {
        lines[0] = get_hat_line(bones.hat);
    }

    // Drop blank hat slot when ALL frames have blank line 0, to avoid
    // oscillating heights across animation frames.
    if (detail::is_blank(lines[0])) {
        bool all_blank = true;
        for (const auto& fr : frames) {
            if (!detail::is_blank(fr[0])) { all_blank = false; break; }
        }
        if (all_blank) lines.erase(lines.begin());
    }

    return lines;
}

// -- sprite_frame_count -----------------------------------------------------

inline auto sprite_frame_count([[maybe_unused]] Species species) -> int {
    return 3; // All species have 3 frames
}

// -- render_face ------------------------------------------------------------
// Compact single-line face rendering per species.

inline auto render_face(const CompanionBones& bones) -> std::string {
    const std::string& e = bones.eye;
    switch (bones.species) {
        case Species::duck:
        case Species::goose:
            return "(" + e + ">";
        case Species::blob:
            return "(" + e + e + ")";
        case Species::cat:
            return "=" + e + "\xCF\x89" + e + "=";   // ω = U+03C9
        case Species::dragon:
            return "<" + e + "~" + e + ">";
        case Species::octopus:
            return "~(" + e + e + ")~";
        case Species::owl:
            return "(" + e + ")(" + e + ")";
        case Species::penguin:
            return "(" + e + ">)";
        case Species::turtle:
            return "[" + e + "_" + e + "]";
        case Species::snail:
            return e + "(@)";
        case Species::ghost:
            return "/" + e + e + "\\";
        case Species::axolotl:
            return "}" + e + "." + e + "{";
        case Species::capybara:
            return "(" + e + "oo" + e + ")";
        case Species::cactus:
            return "|" + e + "  " + e + "|";
        case Species::robot:
            return "[" + e + e + "]";
        case Species::rabbit:
            return "(" + e + ".." + e + ")";
        case Species::mushroom:
            return "|" + e + "  " + e + "|";
        case Species::chonk:
            return "(" + e + "." + e + ")";
    }
    return "(" + e + e + ")";
}

} // namespace cc::buddy
