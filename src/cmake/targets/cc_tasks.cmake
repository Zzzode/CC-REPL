# ─── cc_tasks: Task System ────────────────────────────────────────────────────
add_library(cc_tasks)
target_sources(cc_tasks
    PUBLIC FILE_SET CXX_MODULES FILES
        tasks/in_process_teammate_task.cppm
        tasks/local_agent_task.cppm
        tasks/pill_label.cppm
        tasks/task.cppm
        tasks/task_graph.cppm
        tasks/types.cppm
)
target_link_libraries(cc_tasks PUBLIC cc_utils cc_types cc_state cc_coordinator cc_hooks)
