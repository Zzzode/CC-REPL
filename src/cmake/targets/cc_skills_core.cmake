# ─── cc_skills_core: Skill primitives without tool dependencies ──────────────
add_library(cc_skills_core)
target_sources(cc_skills_core
    PUBLIC FILE_SET CXX_MODULES FILES
        skills/skill.cppm
)
target_link_libraries(cc_skills_core
    PUBLIC
        cc_types
)
