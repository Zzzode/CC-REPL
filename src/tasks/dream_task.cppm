/// @file dream_task.cppm
/// @brief Dream (memory consolidation) task implementation.
/// Migrated from src/tasks/DreamTask/DreamTask.ts
module;

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>
#include <set>
#include <algorithm>

export module cc.tasks.dream_task;

import cc.tasks.task;
import cc.tasks.types;

export namespace cc::tasks {

// ============================================================
// Dream Task Lifecycle
// ============================================================

/// Callback type for updating dream task state
using UpdateDreamTaskFn = std::function<void(const std::string&, std::function<void(DreamTaskState&)>)>;

/// Register a new dream task
/// @param generate_id Function to generate unique task IDs
/// @param sessions_reviewing Number of sessions being reviewed
/// @param prior_mtime Lock mtime for rollback on kill
/// @returns Generated task ID
[[nodiscard]] inline DreamTaskState create_dream_task_state(
    const std::string& task_id,
    int sessions_reviewing,
    std::chrono::system_clock::time_point prior_mtime
) {
    DreamTaskState state{};
    state.id = cc::core::TaskId{task_id};
    state.type = cc::core::TaskType::Dream;
    state.status = cc::core::TaskStatus::Running;
    state.description = "dreaming";
    state.start_time = std::chrono::system_clock::now();
    state.notified = false;
    state.phase = DreamPhase::Starting;
    state.sessions_reviewing = sessions_reviewing;
    state.prior_mtime = prior_mtime;
    return state;
}

/// Add a turn to the dream task's display log
inline void add_dream_turn(
    const std::string& task_id,
    const DreamTurn& turn,
    const std::vector<std::string>& touched_paths,
    UpdateDreamTaskFn update_task
) {
    update_task(task_id, [&](DreamTaskState& task) {
        // Deduplicate touched paths
        std::set<std::string> seen(task.files_touched.begin(), task.files_touched.end());
        std::vector<std::string> new_touched;
        for (const auto& p : touched_paths) {
            if (seen.insert(p).second) {
                new_touched.push_back(p);
            }
        }
        
        // Skip update if turn is empty and nothing new was touched
        if (turn.text.empty() && turn.tool_use_count == 0 && new_touched.empty()) {
            return;
        }
        
        // Update phase if new files were touched
        if (!new_touched.empty()) {
            task.phase = DreamPhase::Updating;
            task.files_touched.insert(task.files_touched.end(), 
                                       new_touched.begin(), new_touched.end());
        }
        
        // Append turn, capping at MAX_DREAM_TURNS
        if (task.turns.size() >= MAX_DREAM_TURNS) {
            task.turns.erase(task.turns.begin());
        }
        task.turns.push_back(turn);
    });
}

/// Complete a dream task successfully
inline void complete_dream_task(
    const std::string& task_id,
    UpdateDreamTaskFn update_task
) {
    update_task(task_id, [](DreamTaskState& task) {
        task.status = cc::core::TaskStatus::Completed;
        task.end_time = std::chrono::system_clock::now();
        task.notified = true;  // Dream has no model-facing notification
    });
}

/// Fail a dream task
inline void fail_dream_task(
    const std::string& task_id,
    UpdateDreamTaskFn update_task
) {
    update_task(task_id, [](DreamTaskState& task) {
        task.status = cc::core::TaskStatus::Failed;
        task.end_time = std::chrono::system_clock::now();
        task.notified = true;
    });
}

/// Kill a dream task and return the prior_mtime for lock rollback
/// @returns prior_mtime if task was killed, nullopt if already terminal
[[nodiscard]] inline std::optional<std::chrono::system_clock::time_point> kill_dream_task(
    const std::string& task_id,
    UpdateDreamTaskFn update_task,
    std::function<void()> abort_fn
) {
    std::optional<std::chrono::system_clock::time_point> prior_mtime;
    
    update_task(task_id, [&](DreamTaskState& task) {
        if (task.status != cc::core::TaskStatus::Running) return;
        
        abort_fn();
        prior_mtime = task.prior_mtime;
        task.status = cc::core::TaskStatus::Killed;
        task.end_time = std::chrono::system_clock::now();
        task.notified = true;
    });
    
    return prior_mtime;
}

} // namespace cc::tasks
