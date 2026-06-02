/// @file lorem_ipsum.cppm
/// @brief Lorem Ipsum skill - placeholder text generation.
module;
#include <string>
#include <vector>
#include <optional>
#include <expected>

export module cc.skills.lorem_ipsum;

import cc.skills.skill;

export namespace cc::skills::lorem_ipsum {

/// Lorem Ipsum placeholder text generation skill
[[nodiscard]] inline SkillDefinition make_lorem_ipsum_skill() {
    return SkillDefinition{
        .name = "lorem-ipsum",
        .description = "Generate placeholder text for testing and prototyping",
        .trigger_patterns = {
            R"(lorem\s*ipsum)",
            R"(placeholder\s+text)",
            R"(dummy\s+(?:text|content))",
            R"(filler\s+text)",
        },
        .content = R"(## Lorem Ipsum Generator

### Usage
Generate placeholder content for UI prototyping and testing.

### Variants
- **Classic**: Standard Lorem Ipsum paragraphs
- **Short**: Single sentences for labels and headings
- **Structured**: Lists, headings, and mixed content
- **Technical**: Code-like placeholder with realistic identifiers

### Parameters
- paragraphs: Number of paragraphs to generate (default: 3)
- sentences_per_paragraph: Sentences per paragraph (default: 4-6)
- format: plain | markdown | html

### Example Output
Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod
tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim
veniam, quis nostrud exercitation ullamco laboris.
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

} // namespace cc::skills::lorem_ipsum
