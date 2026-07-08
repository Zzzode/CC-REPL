module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <variant>

export module cc.server.server_routes;

import cc.config.config;
import cc.hooks.tool_permissions;
import cc.query.query_engine;
import cc.services.api.session_ingress;
import cc.session.storage;
import cc.tools.mcp;
import cc.tools.runtime_registry;
import cc.tools.tool;
import cc.types.types;
import cc.utils.json;

export namespace cc::server {

// A route handler definition
struct Route {
    std::string method;
    std::string path;
    std::function<std::string(std::map<std::string, std::string>)> handler;
};

namespace detail {
    inline std::vector<Route> routes;
    inline std::mutex state_mutex;
    inline std::optional<std::filesystem::path> sessions_dir_override;
    inline std::optional<std::string> active_session_id;
    inline std::uint64_t message_counter = 0;

	    struct DirectQueryRequest {
	        std::string session_id;
	        std::string content;
	        std::optional<std::string> requested_model;
	        std::filesystem::path sessions_dir;
	        std::vector<std::string> prior_message_lines;
	        std::shared_ptr<std::atomic_bool> cancel_flag;
	    };

		    struct DirectPermissionRequest {
		        std::string request_id;
		        std::string session_id;
		        std::string tool_name;
		        std::string input_json;
		        std::string tool_use_id;
		    };

	    struct DirectPermissionRule {
	        std::string tool_name;
	        std::optional<std::string> rule_content;
	        cc::hooks::PermissionDecision decision{cc::hooks::PermissionDecision::ask_user};
	        std::string destination;
	    };

	    struct DirectPermissionDirectory {
	        std::string path;
	        std::string destination;
	    };

	    struct DirectPermissionSessionState {
	        std::vector<DirectPermissionRule> rules;
	        std::vector<DirectPermissionDirectory> additional_directories;
	        std::optional<std::string> mode;
	    };

    struct DirectQueryResult {
        std::string assistant_id;
        std::string content;
        std::string model;
        std::uint32_t input_tokens = 0;
        std::uint32_t output_tokens = 0;
        std::uint32_t tool_rounds = 0;
        std::int64_t elapsed_ms = 0;
    };

		    using DirectQueryExecutor = std::function<std::expected<DirectQueryResult, std::string>(const DirectQueryRequest&)>;
		    using DirectPermissionHandler = std::function<cc::hooks::PermissionResponse(const DirectPermissionRequest&)>;
		    inline std::optional<DirectQueryExecutor> query_executor_override;
		    inline std::unordered_map<std::string, std::shared_ptr<std::atomic_bool>> active_query_cancels;
		    inline std::unordered_map<std::string, DirectPermissionHandler> direct_permission_handlers;
	    inline std::unordered_map<std::string, DirectPermissionSessionState> direct_permission_states;
		    inline std::uint64_t permission_request_counter = 0;

    [[nodiscard]] inline std::string json_escape(std::string_view value) {
        std::string out;
        out.reserve(value.size());
        for (char ch : value) {
            switch (ch) {
                case '\\': out += R"(\\)"; break;
                case '"': out += R"(\")"; break;
                case '\b': out += R"(\b)"; break;
                case '\f': out += R"(\f)"; break;
                case '\n': out += R"(\n)"; break;
                case '\r': out += R"(\r)"; break;
                case '\t': out += R"(\t)"; break;
                default: out += ch; break;
            }
        }
        return out;
    }

    [[nodiscard]] inline std::int64_t epoch_seconds(std::chrono::system_clock::time_point value) {
        return std::chrono::duration_cast<std::chrono::seconds>(
            value.time_since_epoch()).count();
    }

    [[nodiscard]] inline std::filesystem::path default_sessions_dir() {
        if (sessions_dir_override) return *sessions_dir_override;
        if (const char* env = std::getenv("CC_REPL_SERVER_SESSIONS_DIR"); env && *env) {
            return std::filesystem::path{env};
        }
        if (const char* home = std::getenv("HOME"); home && *home) {
            return std::filesystem::path{home} / ".config" / "claude" / "sessions";
        }
        return std::filesystem::current_path() / ".claude" / "sessions";
    }

    [[nodiscard]] inline std::string make_session_id() {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return "server_" + std::to_string(now) + "_" + std::to_string(++message_counter);
    }

    [[nodiscard]] inline std::string make_message_id() {
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return "msg_" + std::to_string(now) + "_" + std::to_string(++message_counter);
    }

    [[nodiscard]] inline std::string title_from_content(std::string_view content) {
        auto title = std::string(content.substr(0, std::min<std::size_t>(content.size(), 80)));
        if (content.size() > 80) title += "...";
        return title.empty() ? std::string("Direct connect session") : title;
    }

    [[nodiscard]] inline std::string message_json(
        std::string_view id,
        std::string_view role,
        std::string_view content,
        std::optional<std::string_view> model = std::nullopt,
        std::optional<std::uint32_t> input_tokens = std::nullopt,
        std::optional<std::uint32_t> output_tokens = std::nullopt
    ) {
        std::ostringstream out;
        out << R"({"id":")" << json_escape(id)
            << R"(","role":")" << json_escape(role)
            << R"(","content":")" << json_escape(content)
            << R"(","created_at":)" << epoch_seconds(std::chrono::system_clock::now());
        if (model && !model->empty()) {
            out << R"(,"model":")" << json_escape(*model) << "\"";
        }
        if (input_tokens || output_tokens) {
            out << R"(,"usage":{"input_tokens":)" << input_tokens.value_or(0)
                << R"(,"output_tokens":)" << output_tokens.value_or(0)
                << "}";
        }
        out << "}";
        return out.str();
    }

    [[nodiscard]] inline std::string sdk_assistant_ingress_event(
        std::string_view session_id,
        std::string_view assistant_message_id,
        const DirectQueryResult& query_result
    ) {
        std::ostringstream out;
        out << R"({"type":"assistant","message":{"id":")" << json_escape(assistant_message_id)
            << R"(","role":"assistant","model":")" << json_escape(query_result.model)
            << R"(","content":[{"type":"text","text":")" << json_escape(query_result.content)
            << R"("}]},"parent_tool_use_id":null,"uuid":")" << json_escape(assistant_message_id)
            << R"(","session_id":")" << json_escape(session_id) << "\"}";
        return out.str();
    }

    [[nodiscard]] inline std::string sdk_result_ingress_event(
        std::string_view session_id,
        std::string_view assistant_message_id,
        const DirectQueryResult& query_result
    ) {
        std::ostringstream out;
        out << R"({"type":"result","subtype":"success","duration_ms":)" << query_result.elapsed_ms
            << R"(,"duration_api_ms":)" << query_result.elapsed_ms
            << R"(,"is_error":false,"num_turns":1,"result":")" << json_escape(query_result.content)
            << R"(","stop_reason":"end_turn","total_cost_usd":0)"
            << R"(,"usage":{"input_tokens":)" << query_result.input_tokens
            << R"(,"output_tokens":)" << query_result.output_tokens
            << R"(,"cache_creation_input_tokens":0,"cache_read_input_tokens":0)"
            << R"(,"server_tool_use":{"web_search_requests":0}})"
            << R"(,"modelUsage":{},"permission_denials":[],"tool_rounds":)" << query_result.tool_rounds
            << R"(,"uuid":"result_)" << json_escape(assistant_message_id)
            << R"(","session_id":")" << json_escape(session_id) << "\"}";
        return out.str();
    }

    inline void publish_direct_query_ingress_events(
        std::string_view session_id,
        std::string_view assistant_message_id,
        const DirectQueryResult& query_result
    ) {
        if (!cc::services::api::is_ingress_active()) return;
        (void)cc::services::api::send_ingress_message(
            sdk_assistant_ingress_event(session_id, assistant_message_id, query_result));
        (void)cc::services::api::send_ingress_message(
            sdk_result_ingress_event(session_id, assistant_message_id, query_result));
    }

    [[nodiscard]] inline std::optional<std::string> json_line_string_field(
        std::string_view line,
        std::string_view key
    ) {
        auto parsed = cc::utils::json::parse(line);
        if (!parsed || !parsed->root().is_obj()) return std::nullopt;
        auto value = parsed->root().get(key);
        if (!value.is_str()) return std::nullopt;
        return std::string(value.as_str());
    }

    [[nodiscard]] inline std::string compact_line_excerpt(std::string text) {
        constexpr std::size_t max_excerpt_chars = 320;
        if (text.size() <= max_excerpt_chars) return text;
        text.resize(max_excerpt_chars);
        text += "...";
        return text;
    }

    [[nodiscard]] inline std::string build_compact_summary(
        const std::vector<std::string>& lines,
        std::size_t summary_count
    ) {
        std::string summary = std::format(
            "Direct-connect conversation compacted: {} older messages summarized.\n"
            "Preserve these details from the compacted history:",
            summary_count);
        constexpr std::size_t max_summary_chars = 8000;
        for (std::size_t index = 0; index < summary_count && index < lines.size(); ++index) {
            auto role = json_line_string_field(lines[index], "role").value_or("unknown");
            auto content = compact_line_excerpt(
                json_line_string_field(lines[index], "content").value_or(lines[index]));
            auto entry = std::format("\n- {} #{}: {}", role, index + 1, content);
            if (summary.size() + entry.size() > max_summary_chars) {
                summary += "\n- [remaining compacted messages omitted due to summary size limit]";
                break;
            }
            summary += std::move(entry);
        }
        return summary;
    }

    [[nodiscard]] inline std::string compact_boundary_json(
        std::string_view id,
        std::string_view summary,
        std::size_t messages_before,
        std::size_t messages_after,
        std::size_t messages_summarized,
        std::size_t keep_recent
    ) {
        std::ostringstream out;
        out << R"({"id":")" << json_escape(id)
            << R"(","role":"system","subtype":"compact_boundary","content":")"
            << json_escape(summary)
            << R"(","created_at":)" << epoch_seconds(std::chrono::system_clock::now())
            << R"(,"compact_metadata":{"messages_before":)" << messages_before
            << R"(,"messages_after":)" << messages_after
            << R"(,"messages_removed":)" << (messages_before >= messages_after ? messages_before - messages_after : 0)
            << R"(,"messages_summarized":)" << messages_summarized
            << R"(,"preserved_recent_messages":)" << keep_recent
            << "}}";
        return out.str();
    }

    [[nodiscard]] inline std::vector<std::string> load_message_lines(
        const std::filesystem::path& sessions_dir,
        std::string_view session_id
    ) {
        std::vector<std::string> lines;
        auto path = cc::session::get_messages_path(sessions_dir, session_id);
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty()) lines.push_back(std::move(line));
        }
        return lines;
    }

    [[nodiscard]] inline std::string json_line_id_or_fallback(
        cc::utils::json::JsonVal root,
        std::size_t index
    ) {
        auto id = root.get("id");
        if (id.valid() && id.is_str()) return std::string(id.as_str());
        return "restored_" + std::to_string(index);
    }

    [[nodiscard]] inline std::optional<cc::core::Message> session_line_to_message(
        const std::string& line,
        std::size_t index
    ) {
        auto parsed = cc::utils::json::parse(line);
        if (!parsed || !parsed->root().is_obj()) return std::nullopt;
        auto root = parsed->root();
        auto role_val = root.get("role");
        auto content_val = root.get("content");
        if (!role_val.is_str() || !content_val.is_str()) return std::nullopt;

        const auto role = std::string(role_val.as_str());
        const auto content = std::string(content_val.as_str());
        const auto id = json_line_id_or_fallback(root, index);
        const auto timestamp = std::chrono::system_clock::now();

        if (role == "assistant") {
            cc::core::AssistantMessage msg{};
            msg.id.value = id;
            msg.timestamp = timestamp;
            msg.content.push_back(cc::core::TextBlock{content});
            auto model = root.get("model");
            if (model.valid() && model.is_str()) msg.model = std::string(model.as_str());
            return cc::core::Message{std::move(msg)};
        }

        if (role == "user" || role == "system") {
            cc::core::UserMessage msg{};
            msg.id.value = id;
            msg.timestamp = timestamp;
            msg.content.push_back(cc::core::TextBlock{content});
            return cc::core::Message{std::move(msg)};
        }

        return std::nullopt;
    }

    inline void seed_query_engine_from_session(
        cc::core::QueryEngine& engine,
        const std::vector<std::string>& prior_message_lines
    ) {
        for (std::size_t index = 0; index < prior_message_lines.size(); ++index) {
            if (auto msg = session_line_to_message(prior_message_lines[index], index)) {
                engine.append_message_for_testing(std::move(*msg));
            }
        }
    }

	    [[nodiscard]] inline std::string assistant_text(const cc::core::AssistantMessage& message) {
	        std::string text;
	        for (const auto& block : message.content) {
	            if (const auto* text_block = std::get_if<cc::core::TextBlock>(&block)) {
	                text += text_block->text;
	            }
	        }
	        return text;
	    }

		    [[nodiscard]] inline std::string make_permission_request_id();

		    [[nodiscard]] inline std::optional<DirectPermissionHandler> permission_handler_for_session(
		        std::string_view session_id
		    );

	    [[nodiscard]] inline cc::hooks::PermissionDecision direct_permission_decision_from_behavior(
	        std::string_view behavior
	    ) {
	        if (behavior == "allow") return cc::hooks::PermissionDecision::allow;
	        if (behavior == "deny") return cc::hooks::PermissionDecision::deny;
	        return cc::hooks::PermissionDecision::ask_user;
	    }

	    inline void append_direct_permission_input_field(
	        std::vector<std::string>& values,
	        cc::utils::json::JsonVal root,
	        std::string_view key
	    ) {
	        auto value = root.get(key);
	        if (value.is_str()) values.emplace_back(value.as_str());
	    }

	    [[nodiscard]] inline std::vector<std::string> direct_permission_input_values(
	        std::string_view input_json
	    ) {
	        std::vector<std::string> values;
	        auto parsed = cc::utils::json::parse(input_json);
	        if (!parsed || !parsed->root().is_obj()) return values;
	        auto root = parsed->root();
	        append_direct_permission_input_field(values, root, "file_path");
	        append_direct_permission_input_field(values, root, "path");
	        append_direct_permission_input_field(values, root, "notebook_path");
	        append_direct_permission_input_field(values, root, "command");
	        append_direct_permission_input_field(values, root, "pattern");
	        append_direct_permission_input_field(values, root, "url");
	        return values;
	    }

	    [[nodiscard]] inline std::vector<std::string> direct_permission_file_path_values(
	        std::string_view input_json
	    ) {
	        std::vector<std::string> values;
	        auto parsed = cc::utils::json::parse(input_json);
	        if (!parsed || !parsed->root().is_obj()) return values;
	        auto root = parsed->root();
	        append_direct_permission_input_field(values, root, "file_path");
	        append_direct_permission_input_field(values, root, "path");
	        append_direct_permission_input_field(values, root, "notebook_path");
	        return values;
	    }

	    [[nodiscard]] inline bool direct_permission_tool_uses_file_paths(std::string_view tool_name) {
	        return tool_name == "Read" ||
	               tool_name == "Write" ||
	               tool_name == "Edit" ||
	               tool_name == "MultiEdit" ||
	               tool_name == "NotebookEdit";
	    }

	    [[nodiscard]] inline std::string direct_permission_normalized_path(std::string_view path) {
	        return std::filesystem::path(std::string(path)).lexically_normal().string();
	    }

	    [[nodiscard]] inline bool direct_permission_path_is_under_directory(
	        std::string_view path,
	        std::string_view directory
	    ) {
	        auto normalized_path = direct_permission_normalized_path(path);
	        auto normalized_directory = direct_permission_normalized_path(directory);
	        if (normalized_path == normalized_directory) return true;
	        if (normalized_directory.empty()) return false;
	        if (normalized_directory.back() != std::filesystem::path::preferred_separator) {
	            normalized_directory.push_back(std::filesystem::path::preferred_separator);
	        }
	        return std::string_view(normalized_path).starts_with(normalized_directory);
	    }

	    [[nodiscard]] inline bool direct_permission_directory_allows_request(
	        const DirectPermissionSessionState& state,
	        const DirectPermissionRequest& request
	    ) {
	        if (state.additional_directories.empty() ||
	            !direct_permission_tool_uses_file_paths(request.tool_name)) {
	            return false;
	        }
	        auto paths = direct_permission_file_path_values(request.input_json);
	        for (const auto& path : paths) {
	            for (const auto& directory : state.additional_directories) {
	                if (direct_permission_path_is_under_directory(path, directory.path)) return true;
	            }
	        }
	        return false;
	    }

	    [[nodiscard]] inline bool direct_permission_rule_content_matches(
	        std::string_view rule_content,
	        const std::vector<std::string>& input_values
	    ) {
	        if (rule_content.empty()) return true;
	        bool prefix_rule = false;
	        auto prefix = rule_content;
	        if (rule_content.ends_with(":*")) {
	            prefix_rule = true;
	            prefix = rule_content.substr(0, rule_content.size() - 2);
	        }
	        for (const auto& value : input_values) {
	            if (value == rule_content) return true;
	            if (prefix_rule && std::string_view(value).starts_with(prefix)) return true;
	        }
	        return false;
	    }

	    [[nodiscard]] inline bool direct_permission_rule_matches(
	        const DirectPermissionRule& rule,
	        const DirectPermissionRequest& request,
	        const std::vector<std::string>& input_values
	    ) {
	        if (rule.tool_name != "*" && rule.tool_name != request.tool_name) return false;
	        if (!rule.rule_content) return true;
	        return direct_permission_rule_content_matches(*rule.rule_content, input_values);
	    }

	    [[nodiscard]] inline bool direct_permission_mode_allows_tool(
	        std::string_view mode,
	        std::string_view tool_name
	    ) {
	        if (mode == "bypassPermissions") return true;
	        if (mode != "acceptEdits") return false;
	        return tool_name == "Edit" ||
	               tool_name == "MultiEdit" ||
	               tool_name == "Write" ||
	               tool_name == "NotebookEdit";
	    }

	    [[nodiscard]] inline bool direct_permission_mode_denies_tool(std::string_view mode) {
	        return mode == "dontAsk" || mode == "plan";
	    }

	    [[nodiscard]] inline std::optional<cc::hooks::PermissionResponse> direct_permission_response_for_session(
	        std::string_view session_id,
	        const DirectPermissionRequest& request
	    ) {
	        std::lock_guard lock(state_mutex);
	        auto state_it = direct_permission_states.find(std::string(session_id));
	        if (state_it == direct_permission_states.end()) return std::nullopt;
	        const auto& state = state_it->second;
	        if (state.mode && direct_permission_mode_allows_tool(*state.mode, request.tool_name)) {
	            cc::hooks::PermissionResponse response{};
	            response.decision = cc::hooks::PermissionDecision::allow;
	            return response;
	        }
	        if (state.mode && direct_permission_mode_denies_tool(*state.mode)) {
	            cc::hooks::PermissionResponse response{};
	            response.decision = cc::hooks::PermissionDecision::deny;
	            response.message = "Permission denied by session permission mode";
	            return response;
	        }

	        if (direct_permission_directory_allows_request(state, request)) {
	            cc::hooks::PermissionResponse response{};
	            response.decision = cc::hooks::PermissionDecision::allow;
	            return response;
	        }

	        auto input_values = direct_permission_input_values(request.input_json);
	        for (auto it = state.rules.rbegin(); it != state.rules.rend(); ++it) {
	            if (!direct_permission_rule_matches(*it, request, input_values)) continue;
	            if (it->decision == cc::hooks::PermissionDecision::ask_user) return std::nullopt;
	            cc::hooks::PermissionResponse response{};
	            response.decision = it->decision;
	            if (it->decision == cc::hooks::PermissionDecision::deny) {
	                response.message = "Permission denied by session permission rule";
	            }
	            return response;
	        }
	        return std::nullopt;
	    }

	    inline void append_direct_permission_rules_from_update(
	        DirectPermissionSessionState& state,
	        cc::utils::json::JsonVal update,
	        cc::hooks::PermissionDecision decision,
	        std::string_view destination
	    ) {
	        auto rules = update.get("rules");
	        if (!rules.is_arr()) return;
	        rules.iter([&](cc::utils::json::JsonVal rule) {
	            if (!rule.is_obj()) return;
	            auto tool_name = rule.get("toolName");
	            if (!tool_name.is_str() || tool_name.as_str().empty()) return;
	            DirectPermissionRule stored_rule{};
	            stored_rule.tool_name = std::string(tool_name.as_str());
	            auto rule_content = rule.get("ruleContent");
	            if (rule_content.is_str()) stored_rule.rule_content = std::string(rule_content.as_str());
	            stored_rule.decision = decision;
	            stored_rule.destination = std::string(destination);
	            state.rules.push_back(std::move(stored_rule));
	        });
	    }

	    inline void remove_direct_permission_rules_from_update(
	        DirectPermissionSessionState& state,
	        cc::utils::json::JsonVal update,
	        cc::hooks::PermissionDecision decision,
	        std::string_view destination
	    ) {
	        auto rules = update.get("rules");
	        if (!rules.is_arr()) return;
	        std::vector<DirectPermissionRule> removals;
	        rules.iter([&](cc::utils::json::JsonVal rule) {
	            if (!rule.is_obj()) return;
	            auto tool_name = rule.get("toolName");
	            if (!tool_name.is_str() || tool_name.as_str().empty()) return;
	            DirectPermissionRule removal{};
	            removal.tool_name = std::string(tool_name.as_str());
	            auto rule_content = rule.get("ruleContent");
	            if (rule_content.is_str()) removal.rule_content = std::string(rule_content.as_str());
	            removal.decision = decision;
	            removal.destination = std::string(destination);
	            removals.push_back(std::move(removal));
	        });
	        state.rules.erase(
	            std::remove_if(state.rules.begin(), state.rules.end(), [&](const DirectPermissionRule& existing) {
	                for (const auto& removal : removals) {
	                    if (existing.tool_name == removal.tool_name &&
	                        existing.rule_content == removal.rule_content &&
	                        existing.decision == removal.decision &&
	                        existing.destination == removal.destination) {
	                        return true;
	                    }
	                }
	                return false;
	            }),
	            state.rules.end());
	    }

	    inline void replace_direct_permission_rules_from_update(
	        DirectPermissionSessionState& state,
	        cc::utils::json::JsonVal update,
	        cc::hooks::PermissionDecision decision,
	        std::string_view destination
	    ) {
	        state.rules.erase(
	            std::remove_if(state.rules.begin(), state.rules.end(), [&](const DirectPermissionRule& existing) {
	                return existing.decision == decision && existing.destination == destination;
	            }),
	            state.rules.end());
	        append_direct_permission_rules_from_update(state, update, decision, destination);
	    }

	    inline void add_direct_permission_directories_from_update(
	        DirectPermissionSessionState& state,
	        cc::utils::json::JsonVal update,
	        std::string_view destination
	    ) {
	        auto directories = update.get("directories");
	        if (!directories.is_arr()) return;
	        directories.iter([&](cc::utils::json::JsonVal directory) {
	            if (!directory.is_str() || directory.as_str().empty()) return;
	            DirectPermissionDirectory stored_directory{};
	            stored_directory.path = std::string(directory.as_str());
	            stored_directory.destination = std::string(destination);
	            state.additional_directories.push_back(std::move(stored_directory));
	        });
	    }

	    inline void remove_direct_permission_directories_from_update(
	        DirectPermissionSessionState& state,
	        cc::utils::json::JsonVal update
	    ) {
	        auto directories = update.get("directories");
	        if (!directories.is_arr()) return;
	        std::vector<std::string> removals;
	        directories.iter([&](cc::utils::json::JsonVal directory) {
	            if (directory.is_str() && !directory.as_str().empty()) {
	                removals.emplace_back(directory.as_str());
	            }
	        });
	        state.additional_directories.erase(
	            std::remove_if(
	                state.additional_directories.begin(),
	                state.additional_directories.end(),
	                [&](const DirectPermissionDirectory& existing) {
	                    for (const auto& removal : removals) {
	                        if (direct_permission_normalized_path(existing.path) ==
	                            direct_permission_normalized_path(removal)) {
	                            return true;
	                        }
	                    }
	                    return false;
	                }),
	            state.additional_directories.end());
	    }

		    inline void apply_direct_permission_updates(
		        std::string_view session_id,
		        std::string_view updated_permissions_json
	    ) {
	        auto parsed = cc::utils::json::parse(updated_permissions_json);
	        if (!parsed || !parsed->root().is_arr()) return;
	        std::lock_guard lock(state_mutex);
	        auto& state = direct_permission_states[std::string(session_id)];
	        parsed->root().iter([&](cc::utils::json::JsonVal update) {
	            if (!update.is_obj()) return;
	            const auto type = update.get_string("type");
	            const auto destination = update.get_string("destination").empty()
	                ? std::string("session")
	                : update.get_string("destination");
	            if (type == "setMode") {
	                auto mode = update.get("mode");
	                if (mode.is_str()) state.mode = std::string(mode.as_str());
	                return;
	            }
	            const auto decision = direct_permission_decision_from_behavior(update.get_string("behavior"));
	            if (type == "addRules") {
	                append_direct_permission_rules_from_update(state, update, decision, destination);
	            } else if (type == "removeRules") {
	                remove_direct_permission_rules_from_update(state, update, decision, destination);
	            } else if (type == "replaceRules") {
	                replace_direct_permission_rules_from_update(state, update, decision, destination);
	            } else if (type == "addDirectories") {
	                add_direct_permission_directories_from_update(state, update, destination);
	            } else if (type == "removeDirectories") {
	                remove_direct_permission_directories_from_update(state, update);
	            }
		        });
		    }

    [[nodiscard]] inline cc::tools::AgentLivePermissionCheck check_agent_tool_permission(
        cc::hooks::ToolPermissionHook& permission_hook,
        std::string_view tool_name,
        std::string_view input_json,
        std::string_view tool_use_id
    ) {
        permission_hook.set_current_tool_use_id(tool_use_id);
        auto response = permission_hook.can_use_response(tool_name, input_json);
        permission_hook.clear_current_tool_use_id();

        cc::tools::AgentLivePermissionCheck check;
        check.allowed = response.decision == cc::hooks::PermissionDecision::allow ||
                        response.decision == cc::hooks::PermissionDecision::allow_once;
        check.updated_input_json = std::move(response.updated_input_json);
        check.message = std::move(response.message);
        return check;
    }

			    [[nodiscard]] inline std::expected<DirectQueryResult, std::string> execute_native_query(
			        const DirectQueryRequest& request
		    ) {
        if (query_executor_override) return (*query_executor_override)(request);

        cc::core::ConfigManager manager;
        if (auto loaded = manager.load(); !loaded) {
            return std::unexpected(loaded.error().format());
        }

        cc::core::QueryEngineConfig config;
        const auto& settings = manager.settings();
        config.api_key = settings.network.api_key.value_or("");
        if (settings.network.base_url) config.base_url = *settings.network.base_url;
        config.model_params.model = request.requested_model.value_or(settings.model.default_model);
        config.model_params.max_tokens = settings.model.max_output_tokens;
        config.model_params.temperature = settings.model.temperature;
        config.context_window.max_context_tokens = settings.model.context_window_size;
        config.retry_policy.max_retries = settings.network.max_retries;
        config.thinking_config.mode = settings.model.extended_thinking
            ? cc::core::ThinkingConfig::Mode::Adaptive
            : cc::core::ThinkingConfig::Mode::Disabled;
        config.thinking_config.budget_tokens = settings.model.thinking_budget;
        config.cwd = std::filesystem::current_path().string();

        if (config.api_key.empty()) {
            return std::unexpected("ANTHROPIC_API_KEY is required for direct-connect /message");
        }

        auto permission_handler = permission_handler_for_session(request.session_id);
        cc::hooks::ToolPermissionHook permission_hook;
        if (permission_handler) {
            permission_hook.set_auto_approve(false);
            permission_hook.set_working_dir(std::filesystem::current_path().string());
            permission_hook.set_ask_user_response_fn(
                [handler = *permission_handler, session_id = request.session_id](
                    const cc::hooks::PermissionContext& ctx
                ) -> cc::hooks::PermissionResponse {
                    auto cached_request = DirectPermissionRequest{
                        .request_id = {},
                        .session_id = session_id,
                        .tool_name = ctx.tool_name,
                        .input_json = ctx.args,
                        .tool_use_id = ctx.tool_use_id,
                    };
                    if (auto cached = direct_permission_response_for_session(session_id, cached_request)) {
                        return *cached;
                    }

                    auto request_id = make_permission_request_id();
                    cached_request.request_id = std::move(request_id);
                    if (cached_request.tool_use_id.empty()) {
                        cached_request.tool_use_id = cached_request.request_id;
                    }
                    auto response = handler(cached_request);
                    if (response.decision == cc::hooks::PermissionDecision::allow &&
                        response.updated_permissions_json) {
                        apply_direct_permission_updates(session_id, *response.updated_permissions_json);
                    }
                    return response;
                });
        }

        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry, cc::tools::RuntimeToolOptions{
            .parent_permission_mode = std::nullopt,
            .permission_check = permission_handler
                ? cc::tools::AgentLivePermissionCheckFn{[&permission_hook](
                    std::string_view tool_name,
                    std::string_view input_json,
                    std::string_view tool_use_id
                ) {
                    return check_agent_tool_permission(permission_hook, tool_name, input_json, tool_use_id);
                }}
                : cc::tools::AgentLivePermissionCheckFn{},
            .permission_hook_valid_for_background = false,
        });

        // TS PARITY FALLBACK: route unregistered tool names (e.g. MCP server
        // tools like "analyze_image") to connected MCP servers.  In TS,
        // `assembleToolPool` merges built-in tools with per-server MCP tools
        // so the model can call them directly by short name.
        registry.set_missing_tool_handler(
            [](std::string_view tool_name,
               const cc::core::ToolInput& input) -> cc::core::Result<cc::core::ToolResult> {
                namespace mcp = cc::tools;
                auto& runtime = mcp::NativeMcpRuntime::instance();
                std::string last_error;
                auto statuses = runtime.all_statuses();
                for (const auto& s : statuses) {
                    auto result = runtime.call_tool(
                        s.name, tool_name, std::string{input.json()});
                    if (result) {
                        std::vector<cc::core::ToolOutputContent> contents;
                        contents.push_back(
                            cc::core::ToolOutputContent::text_output(result->content));
                        return cc::core::ToolResult{
                            .content = std::move(contents),
                            .is_error = result->is_error,
                        };
                    }
                    last_error = std::string{mcp::format_error(result.error())};
                }
                return std::unexpected(cc::core::Error::make(
                    cc::core::ErrorCode::ToolNotFound,
                    std::format("Tool '{}' not found in registry or on any "
                                "configured MCP server{}",
                                tool_name,
                                last_error.empty() ? "" : " (" + last_error + ")")));
            });

        config.tools = registry.get_visible_definitions();
        // TS PARITY: MCP tools discovered dynamically after server connection.
        // Provider callback ensures build_request_body() picks up newly
        // connected MCP servers' tools on every API call.
        config.dynamic_tools_provider = []() -> std::vector<cc::core::ToolDefinition> {
            return cc::tools::collect_mcp_tool_definitions();
        };

        cc::core::QueryEngine engine(std::move(config), registry);
        if (request.cancel_flag) {
            engine.set_external_abort_callback([flag = request.cancel_flag] {
                return flag->load();
            });
        }
        if (permission_handler) {
            engine.set_permission_hook(&permission_hook);
        }
        seed_query_engine_from_session(engine, request.prior_message_lines);

	        if (request.cancel_flag && request.cancel_flag->load()) {
	            return std::unexpected("Query interrupted");
	        }
	        auto response = engine.query(request.content);
	        if (!response) return std::unexpected(response.error().format());

        return DirectQueryResult{
            .assistant_id = response->message.id.value,
            .content = assistant_text(response->message),
            .model = response->message.model.value_or(settings.model.default_model),
            .input_tokens = response->total_usage.input_tokens,
            .output_tokens = response->total_usage.output_tokens,
            .tool_rounds = response->tool_rounds,
            .elapsed_ms = response->elapsed.count(),
        };
    }

    inline bool write_message_lines(
        const std::filesystem::path& sessions_dir,
        std::string_view session_id,
        const std::vector<std::string>& lines
    ) {
        auto path = cc::session::get_messages_path(sessions_dir, session_id);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::trunc);
        if (!output.is_open()) return false;
        for (const auto& line : lines) output << line << '\n';
        return output.good();
    }

    inline void save_active_session_metadata(
        const std::filesystem::path& sessions_dir,
        cc::session::SessionMetadata& metadata
    ) {
        metadata.last_active = std::chrono::system_clock::now();
        (void)cc::session::save_session_metadata(sessions_dir, metadata);
    }

    [[nodiscard]] inline cc::session::SessionMetadata create_metadata(
        std::string_view content,
        std::string_view model
    ) {
        const auto now = std::chrono::system_clock::now();
        return cc::session::SessionMetadata{
            .session_id = make_session_id(),
            .model = model.empty() ? std::string("default") : std::string(model),
            .cwd = std::filesystem::current_path(),
            .created_at = now,
            .last_active = now,
            .message_count = 0,
            .title = title_from_content(content),
            .is_archived = false,
        };
    }

	    [[nodiscard]] inline std::string get_param(
	        const std::map<std::string, std::string>& params,
	        std::string_view key
	    ) {
	        auto it = params.find(std::string(key));
	        return it == params.end() ? std::string{} : it->second;
	    }

	    [[nodiscard]] inline std::shared_ptr<std::atomic_bool> register_active_query_cancel(
	        std::string_view session_id
	    ) {
	        auto flag = std::make_shared<std::atomic_bool>(false);
	        std::lock_guard lock(state_mutex);
	        active_query_cancels[std::string(session_id)] = flag;
	        return flag;
	    }

	    inline void clear_active_query_cancel(
	        std::string_view session_id,
	        const std::shared_ptr<std::atomic_bool>& flag
	    ) {
	        std::lock_guard lock(state_mutex);
	        auto it = active_query_cancels.find(std::string(session_id));
	        if (it != active_query_cancels.end() && it->second == flag) {
	            active_query_cancels.erase(it);
	        }
	    }

	    [[nodiscard]] inline bool cancel_active_query(std::string_view session_id) {
	        std::shared_ptr<std::atomic_bool> flag;
	        {
	            std::lock_guard lock(state_mutex);
	            auto it = active_query_cancels.find(std::string(session_id));
	            if (it == active_query_cancels.end()) return false;
	            flag = it->second;
	        }
	        flag->store(true);
	        return true;
	    }

	    [[nodiscard]] inline std::string make_permission_request_id() {
	        std::lock_guard lock(state_mutex);
	        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
	            std::chrono::system_clock::now().time_since_epoch()).count();
	        return "perm_" + std::to_string(now) + "_" + std::to_string(++permission_request_counter);
	    }

	    [[nodiscard]] inline std::optional<DirectPermissionHandler> permission_handler_for_session(
	        std::string_view session_id
	    ) {
	        std::lock_guard lock(state_mutex);
	        auto it = direct_permission_handlers.find(std::string(session_id));
	        if (it == direct_permission_handlers.end()) return std::nullopt;
	        return it->second;
	    }

	    [[nodiscard]] inline std::size_t parse_limit(
	        const std::map<std::string, std::string>& params,
        std::size_t fallback
    ) {
        auto text = get_param(params, "limit");
        if (text.empty()) return fallback;
        std::size_t value = 0;
        for (char ch : text) {
            if (ch < '0' || ch > '9') return fallback;
            value = value * 10 + static_cast<std::size_t>(ch - '0');
        }
        return value == 0 ? fallback : std::min<std::size_t>(value, 100);
    }

    [[nodiscard]] inline std::string handle_message(std::map<std::string, std::string> params) {
        auto content = get_param(params, "content");
        if (content.empty()) return R"({"error":"content is required"})";

        std::filesystem::path sessions_dir;
        cc::session::SessionMetadata metadata;
        std::vector<std::string> prior_lines;
        std::string user_message_id;
        {
            std::lock_guard lock(state_mutex);
            sessions_dir = default_sessions_dir();
            auto requested_session = get_param(params, "session_id");
            if (requested_session.empty() && active_session_id) requested_session = *active_session_id;
            auto loaded_metadata = requested_session.empty()
                ? std::optional<cc::session::SessionMetadata>{}
                : cc::session::load_session_metadata(sessions_dir, requested_session);
            if (!loaded_metadata) {
                loaded_metadata = create_metadata(content, get_param(params, "model"));
            }
            metadata = *loaded_metadata;
            prior_lines = load_message_lines(sessions_dir, metadata.session_id);
            user_message_id = make_message_id();
        }

	        auto requested_model = get_param(params, "model");
	        auto cancel_flag = register_active_query_cancel(metadata.session_id);
	        auto query_result = execute_native_query(DirectQueryRequest{
	            .session_id = metadata.session_id,
	            .content = content,
	            .requested_model = requested_model.empty()
	                ? std::nullopt
	                : std::optional<std::string>{requested_model},
	            .sessions_dir = sessions_dir,
	            .prior_message_lines = prior_lines,
	            .cancel_flag = cancel_flag,
	        });
	        clear_active_query_cancel(metadata.session_id, cancel_flag);
	        if (!query_result) {
	            std::ostringstream error;
            error << R"({"error":")" << json_escape(query_result.error())
                  << R"(","session_id":")" << json_escape(metadata.session_id)
                  << R"(","status":"failed"})";
            return error.str();
        }

        std::string assistant_message_id = query_result->assistant_id;
        if (assistant_message_id.empty()) {
            std::lock_guard lock(state_mutex);
            assistant_message_id = make_message_id();
        }

        if (!cc::session::append_message(
                sessions_dir,
                metadata.session_id,
                message_json(user_message_id, "user", content))) {
            return R"({"error":"failed to append user message"})";
        }
        if (!cc::session::append_message(
                sessions_dir,
                metadata.session_id,
                message_json(
                    assistant_message_id,
                    "assistant",
                    query_result->content,
                    query_result->model,
                    query_result->input_tokens,
                    query_result->output_tokens))) {
            return R"({"error":"failed to append assistant message"})";
        }

        {
            std::lock_guard lock(state_mutex);
            metadata.message_count = static_cast<int>(
                load_message_lines(sessions_dir, metadata.session_id).size());
            if (metadata.model.empty() || metadata.model == "default") {
                metadata.model = query_result->model;
            }
            save_active_session_metadata(sessions_dir, metadata);
            active_session_id = metadata.session_id;
        }

        publish_direct_query_ingress_events(metadata.session_id, assistant_message_id, *query_result);

        std::ostringstream response;
        response << R"({"id":")" << json_escape(assistant_message_id)
                 << R"(","session_id":")" << json_escape(metadata.session_id)
                 << R"(","status":"completed","response":")" << json_escape(query_result->content)
                 << R"(","model":")" << json_escape(query_result->model)
                 << R"(","messages_appended":2)"
                 << R"(,"usage":{"input_tokens":)" << query_result->input_tokens
                 << R"(,"output_tokens":)" << query_result->output_tokens
                 << R"(},"tool_rounds":)" << query_result->tool_rounds
                 << R"(,"elapsed_ms":)" << query_result->elapsed_ms
                 << "}";
        return response.str();
    }

    [[nodiscard]] inline std::string handle_sessions(std::map<std::string, std::string> params) {
        std::lock_guard lock(state_mutex);
        const auto sessions_dir = default_sessions_dir();
        auto sessions = cc::session::list_recent_sessions(sessions_dir, parse_limit(params, 20));

        std::ostringstream response;
        response << R"({"sessions":[)";
        for (std::size_t i = 0; i < sessions.size(); ++i) {
            const auto& session = sessions[i];
            if (i > 0) response << ",";
            response << R"({"session_id":")" << json_escape(session.session_id)
                     << R"(","model":")" << json_escape(session.model)
                     << R"(","cwd":")" << json_escape(session.cwd.string())
                     << R"(","message_count":)" << session.message_count
                     << R"(,"created_at":)" << epoch_seconds(session.created_at)
                     << R"(,"last_active":)" << epoch_seconds(session.last_active)
                     << R"(,"is_archived":)" << (session.is_archived ? "true" : "false");
            if (session.title) {
                response << R"(,"title":")" << json_escape(*session.title) << "\"";
            }
            response << "}";
        }
        response << R"(],"total":)" << sessions.size() << "}";
        return response.str();
    }

    [[nodiscard]] inline std::string handle_compact(std::map<std::string, std::string> params) {
        std::lock_guard lock(state_mutex);
        const auto sessions_dir = default_sessions_dir();
        auto session_id = get_param(params, "session_id");
        if (session_id.empty() && active_session_id) session_id = *active_session_id;
        if (session_id.empty()) return R"({"error":"session_id is required"})";

        auto metadata = cc::session::load_session_metadata(sessions_dir, session_id);
        if (!metadata) return R"({"error":"session not found"})";

        auto lines = load_message_lines(sessions_dir, session_id);
        const auto before = lines.size();
        constexpr std::size_t keep_recent = 6;
        std::size_t after = before;
        std::size_t messages_summarized = 0;
        std::string boundary_id;
        if (before > keep_recent) {
            messages_summarized = before - keep_recent;
            after = keep_recent + 1;
            boundary_id = make_message_id();
            auto summary = build_compact_summary(lines, messages_summarized);
            std::vector<std::string> compacted;
            compacted.push_back(compact_boundary_json(
                boundary_id,
                summary,
                before,
                after,
                messages_summarized,
                keep_recent));
            compacted.insert(
                compacted.end(),
                lines.end() - static_cast<std::ptrdiff_t>(keep_recent),
                lines.end()
            );
            if (!write_message_lines(sessions_dir, session_id, compacted)) {
                return R"({"error":"failed to write compacted messages"})";
            }
            after = compacted.size();
        }
        metadata->message_count = static_cast<int>(after);
        save_active_session_metadata(sessions_dir, *metadata);

        std::ostringstream response;
        response << R"({"status":"compacted","session_id":")" << json_escape(session_id)
                 << R"(","messages_before":)" << before
                 << R"(,"messages_after":)" << after
                 << R"(,"messages_removed":)" << (before >= after ? before - after : 0)
                 << R"(,"messages_summarized":)" << messages_summarized;
        if (!boundary_id.empty()) {
            response << R"(,"compact_boundary_id":")" << json_escape(boundary_id) << "\"";
        }
        response << "}";
        return response.str();
    }
}

inline void set_sessions_dir_for_testing(std::filesystem::path path) {
    std::lock_guard lock(detail::state_mutex);
    detail::sessions_dir_override = std::move(path);
    detail::active_session_id.reset();
}

inline void reset_route_state_for_testing() {
    std::lock_guard lock(detail::state_mutex);
    detail::sessions_dir_override.reset();
    detail::active_session_id.reset();
    detail::message_counter = 0;
	    detail::permission_request_counter = 0;
	    detail::query_executor_override.reset();
	    detail::active_query_cancels.clear();
	    detail::direct_permission_handlers.clear();
	    detail::direct_permission_states.clear();
	    detail::routes.clear();
	}

inline bool request_direct_query_cancel(std::string_view session_id) {
    return detail::cancel_active_query(session_id);
}

inline void register_direct_permission_handler(
    std::string_view session_id,
    detail::DirectPermissionHandler handler
) {
    std::lock_guard lock(detail::state_mutex);
    detail::direct_permission_handlers[std::string(session_id)] = std::move(handler);
}

inline void unregister_direct_permission_handler(std::string_view session_id) {
    std::lock_guard lock(detail::state_mutex);
    detail::direct_permission_handlers.erase(std::string(session_id));
}

inline void set_query_executor_for_testing(detail::DirectQueryExecutor executor) {
    std::lock_guard lock(detail::state_mutex);
    detail::query_executor_override = std::move(executor);
}

// Register a route handler
inline auto register_route(Route route) -> void {
    detail::routes.push_back(std::move(route));
}

// Get all default API routes
inline auto get_default_routes() -> std::vector<Route> {
    std::vector<Route> defaults;

    // GET /health - Health check endpoint
    defaults.push_back({
        "GET",
        "/health",
        [](std::map<std::string, std::string>) -> std::string {
            return R"({"status":"ok","version":"1.0.0"})";
        }
    });

    // POST /message - Send a message to the assistant
	defaults.push_back({
	    "POST",
	    "/message",
	    [](std::map<std::string, std::string> params) -> std::string {
		return detail::handle_message(std::move(params));
	    }
	});

    // GET /sessions - List active and recent sessions
	defaults.push_back({
	    "GET",
	    "/sessions",
	    [](std::map<std::string, std::string> params) -> std::string {
		return detail::handle_sessions(std::move(params));
	    }
	});

    // POST /compact - Compact the current conversation
	defaults.push_back({
	    "POST",
	    "/compact",
	    [](std::map<std::string, std::string> params) -> std::string {
		return detail::handle_compact(std::move(params));
	    }
	});

    return defaults;
}

// Initialize the route table with default routes
inline auto initialize_routes() -> void {
    detail::routes.clear();
    auto defaults = get_default_routes();
    for (auto& route : defaults) {
	register_route(std::move(route));
    }
}

// Find a route matching the given method and path
inline auto find_route(std::string_view method, std::string_view path)
    -> const Route* {
    for (const auto& route : detail::routes) {
        if (route.method == method && route.path == path) {
            return &route;
        }
    }
    return nullptr;
}

} // namespace cc::server
