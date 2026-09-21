// Teammate-list view-model helpers: filtering, sorting, pinning, persistence.
//
// Duplication note: hooks/swarm_hooks.cppm, hooks/teammate_view_auto_exit.cppm
// and ui/components/agent_view.cppm each carry ad-hoc teammate state.  Those
// files will migrate toward the TeammateStateView / TeammateViewState types
// defined here as the canonical view model.
module;

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <yyjson.h>

export module cc.state.teammate_view_helpers;

import cc.utils.json;
import cc.task_types;

export namespace cc::state::teammate_view {

using TaskSortKey = cc::tasks::TaskSortKey;

struct TeammateStateView {
    std::string agent_id;
    std::string display_name;

    enum class OnlineState {
        Offline,
        Online,
        Busy,
        Crashed,
    } online = OnlineState::Offline;

    int active_tasks = 0;
    int completed_tasks = 0;
    int64_t last_heartbeat_ms = 0;
    std::string status_message;
};

struct TeammateViewState {
    bool collapsed = false;
    std::unordered_set<std::string> pinned_ids;
    std::string filter_text;
    TaskSortKey sort = TaskSortKey::PriorityDescStatusAsc;
    bool show_offline = true;
};

// ---------------------------------------------------------------------------
// Filter + sort
// ---------------------------------------------------------------------------

namespace detail {

inline std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace detail

inline bool teammate_matches_filter(const TeammateStateView& m, std::string_view filter) {
    if (filter.empty()) return true;
    auto needle = detail::to_lower(filter);
    auto hay = [&](std::string_view s) {
        return detail::to_lower(s).find(needle) != std::string::npos;
    };
    return hay(m.display_name) || hay(m.agent_id) || hay(m.status_message);
}

inline std::vector<TeammateStateView>
apply_view_filter(const std::vector<TeammateStateView>& all,
                  const TeammateViewState& view) {
    std::vector<TeammateStateView> out;
    out.reserve(all.size());
    for (const auto& m : all) {
        // Visibility rules
        if (m.online == TeammateStateView::OnlineState::Offline && !view.show_offline) {
            continue;
        }
        if (!teammate_matches_filter(m, view.filter_text)) continue;
        out.push_back(m);
    }

    // Heuristic priority order for PriorityDescStatusAsc: Busy < Online < Offline < Crashed
    // (lower = earlier).
    auto online_rank = [](TeammateStateView::OnlineState s) -> int {
        switch (s) {
            case TeammateStateView::OnlineState::Online: return 0;
            case TeammateStateView::OnlineState::Busy: return 1;
            case TeammateStateView::OnlineState::Offline: return 3;
            case TeammateStateView::OnlineState::Crashed: return 4;
        }
        return 3;
    };
    auto is_pinned = [&](const TeammateStateView& m) {
        return view.pinned_ids.contains(m.agent_id);
    };
    auto disp_ci = [](const TeammateStateView& m) { return detail::to_lower(m.display_name); };

    std::sort(out.begin(), out.end(), [&](const TeammateStateView& a, const TeammateStateView& b) {
        // Pinned first
        bool pa = is_pinned(a), pb = is_pinned(b);
        if (pa != pb) return pa;

        switch (view.sort) {
            case TaskSortKey::CreatedAtAsc:
                return a.last_heartbeat_ms < b.last_heartbeat_ms;
            case TaskSortKey::CreatedAtDesc:
                return a.last_heartbeat_ms > b.last_heartbeat_ms;
            case TaskSortKey::PriorityDescStatusAsc: {
                int ra = online_rank(a.online);
                int rb = online_rank(b.online);
                if (ra != rb) return ra < rb;
                // Secondary: active tasks descending (more active = earlier)
                if (a.active_tasks != b.active_tasks) return a.active_tasks > b.active_tasks;
                return disp_ci(a) < disp_ci(b);
            }
            case TaskSortKey::TitleAsc:
                return disp_ci(a) < disp_ci(b);
        }
        return disp_ci(a) < disp_ci(b);
    });
    return out;
}

// ---------------------------------------------------------------------------
// State mutators
// ---------------------------------------------------------------------------

inline void toggle_collapsed(TeammateViewState& view) { view.collapsed = !view.collapsed; }

inline void toggle_pin(TeammateViewState& view, std::string_view agent_id) {
    std::string key(agent_id);
    auto it = view.pinned_ids.find(key);
    if (it == view.pinned_ids.end()) view.pinned_ids.insert(std::move(key));
    else view.pinned_ids.erase(it);
}

inline bool is_pinned(const TeammateViewState& view, std::string_view agent_id) {
    return view.pinned_ids.contains(std::string(agent_id));
}

inline void set_filter(TeammateViewState& view, std::string f) {
    view.filter_text = std::move(f);
}

// ---------------------------------------------------------------------------
// Ser/de
// ---------------------------------------------------------------------------

namespace detail {

inline const char* online_str(TeammateStateView::OnlineState s) {
    switch (s) {
        case TeammateStateView::OnlineState::Offline: return "offline";
        case TeammateStateView::OnlineState::Online: return "online";
        case TeammateStateView::OnlineState::Busy: return "busy";
        case TeammateStateView::OnlineState::Crashed: return "crashed";
    }
    return "offline";
}

inline TeammateStateView::OnlineState online_from(std::string_view s) {
    if (s == "online") return TeammateStateView::OnlineState::Online;
    if (s == "busy") return TeammateStateView::OnlineState::Busy;
    if (s == "crashed") return TeammateStateView::OnlineState::Crashed;
    return TeammateStateView::OnlineState::Offline;
}

inline const char* sort_str(TaskSortKey k) {
    switch (k) {
        case TaskSortKey::CreatedAtAsc: return "created_asc";
        case TaskSortKey::CreatedAtDesc: return "created_desc";
        case TaskSortKey::PriorityDescStatusAsc: return "priority_desc_status_asc";
        case TaskSortKey::TitleAsc: return "title_asc";
    }
    return "priority_desc_status_asc";
}

inline TaskSortKey sort_from(std::string_view s) {
    if (s == "created_asc") return TaskSortKey::CreatedAtAsc;
    if (s == "created_desc") return TaskSortKey::CreatedAtDesc;
    if (s == "priority_desc_status_asc") return TaskSortKey::PriorityDescStatusAsc;
    if (s == "title_asc") return TaskSortKey::TitleAsc;
    return TaskSortKey::PriorityDescStatusAsc;
}

} // namespace detail

inline std::string teammate_view_state_to_json(const TeammateViewState& v) {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    root.add("collapsed", doc.boolean(v.collapsed));
    root.add("filter_text", doc.string(v.filter_text));
    root.add("sort", doc.string(detail::sort_str(v.sort)));
    root.add("show_offline", doc.boolean(v.show_offline));
    auto arr = doc.array();
    for (const auto& id : v.pinned_ids) arr.append(doc.string(id));
    root.add("pinned_ids", arr);
    doc.set_root(root);
    return doc.to_string();
}

inline std::expected<TeammateViewState, std::string>
teammate_view_state_from_json(std::string_view s) {
    auto parsed = cc::utils::json::parse(s);
    if (!parsed) return std::unexpected("invalid JSON for TeammateViewState");
    auto root = parsed->root();
    if (!root.is_obj()) return std::unexpected("root not object");

    TeammateViewState v;
    v.collapsed = root.has("collapsed") && root.get("collapsed").as_bool();
    v.filter_text = std::string(root.get_string("filter_text"));
    v.sort = detail::sort_from(root.get_string("sort"));
    v.show_offline = !root.has("show_offline") || root.get("show_offline").as_bool();
    auto pins = root.get("pinned_ids");
    if (pins.is_arr()) {
        pins.iter([&](cc::utils::json::JsonVal e) {
            if (e.is_str()) v.pinned_ids.emplace(e.as_str());
        });
    }
    return v;
}

} // namespace cc::state::teammate_view
