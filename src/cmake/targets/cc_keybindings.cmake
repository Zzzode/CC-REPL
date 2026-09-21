# ─── cc_keybindings: Keybinding System ────────────────────────────────────────
add_library(cc_keybindings)
target_sources(cc_keybindings
    PUBLIC FILE_SET CXX_MODULES FILES
        keybindings/defaults.cppm
        keybindings/keybinding_system.cppm
        keybindings/keybindings.cppm
        keybindings/load_user_bindings.cppm
        keybindings/match.cppm
        keybindings/resolver.cppm
        keybindings/schema.cppm
        keybindings/shortcut_format.cppm
        keybindings/template.cppm
        keybindings/validate.cppm
)
target_link_libraries(cc_keybindings PUBLIC cc_utils cc_config)
