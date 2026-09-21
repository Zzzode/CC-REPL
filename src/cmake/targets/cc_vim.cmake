# ─── cc_vim: Vim Mode ─────────────────────────────────────────────────────────
add_library(cc_vim)
target_sources(cc_vim
    PUBLIC FILE_SET CXX_MODULES FILES
        vim/vim_types.cppm
        vim/vim_controller.cppm
        vim/vim_commands.cppm
        vim/vim_mode.cppm
        vim/vim_motions.cppm
        vim/vim_operators.cppm
        vim/vim_text_objects.cppm
)
target_link_libraries(cc_vim PUBLIC cc_utils)
