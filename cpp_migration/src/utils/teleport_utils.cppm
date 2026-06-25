// Teleport API client, environment selection, git bundle transfer.
// Migrated from src/utils/teleport/*.ts
module;

#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

export module cc.utils.teleport_utils;

import cc.utils.http;
import cc.utils.json;
import cc.utils.bash_execution;

export namespace cc::utils::teleport {

using namespace std::chrono;
namespace fs = std::filesystem;

// =========================================================================
// api.ts — Types & API Client
// =========================================================================

/// Retry delays for teleport API requests (exponential backoff).
inline constexpr std::array kRetryDelays = {
    milliseconds{2000}, milliseconds{4000},
    milliseconds{8000}, milliseconds{16000}
};
inline constexpr std::size_t kMaxRetries = kRetryDelays.size();

/// CCR BYOC beta header value.
inline constexpr std::string_view kCcrByocBeta = "ccr-byoc-2025-07-29";

/// Session status from the Sessions API.
enum class SessionStatus {
    requires_action,
    running,
    idle,
    archived,
};

/// Git source in a session context.
struct GitSource {
    std::string url;
    std::optional<std::string> revision;
    bool allow_unrestricted_git_push{false};
};

/// Knowledge base source in a session context.
struct KnowledgeBaseSource {
    std::string knowledge_base_id;
};

/// A session context source (tagged union).
using SessionContextSource = std::variant<GitSource, KnowledgeBaseSource>;

/// Outcome git info.
struct OutcomeGitInfo {
    std::string repo;
    std::vector<std::string> branches;
};

/// Git repository outcome.
struct GitRepositoryOutcome {
    OutcomeGitInfo git_info;
};

/// GitHub PR reference.
struct GithubPr {
    std::string owner;
    std::string repo;
    int number;
};

/// Session context payload.
struct SessionContext {
    std::vector<SessionContextSource> sources;
    std::string cwd;
    std::vector<GitRepositoryOutcome> outcomes;
    std::optional<std::string> custom_system_prompt;
    std::optional<std::string> append_system_prompt;
    std::optional<std::string> model;
    std::optional<std::string> seed_bundle_file_id;
    std::optional<GithubPr> github_pr;
    bool reuse_outcome_branches{false};
};

/// Full session resource from the API.
struct SessionResource {
    std::string id;
    std::optional<std::string> title;
    SessionStatus session_status;
    std::string environment_id;
    std::string created_at;
    std::string updated_at;
    SessionContext session_context;
};

/// List sessions response.
struct ListSessionsResponse {
    std::vector<SessionResource> data;
    bool has_more;
    std::optional<std::string> first_id;
    std::optional<std::string> last_id;
};

/// Code session status.
enum class CodeSessionStatus {
    idle,
    working,
    waiting,
    completed,
    archived,
    cancelled,
    rejected,
};

/// Repository info within a code session.
struct CodeSessionRepo {
    std::string name;
    std::string owner_login;
    std::optional<std::string> default_branch;
};

/// Lightweight code session (transformed from SessionResource).
struct CodeSession {
    std::string id;
    std::string title;
    std::string description;
    CodeSessionStatus status;
    std::optional<CodeSessionRepo> repo;
    std::vector<std::string> turns;
    std::string created_at;
    std::string updated_at;
};

/// Content for a remote session message (plain string or structured blocks).
using RemoteMessageContent = std::variant<
    std::string,
    std::vector<std::string>  // simplified content blocks
>;

/// API credentials prepared for requests.
struct ApiCredentials {
    std::string access_token;
    std::string org_uuid;
};

/// Fully resolved Sessions API configuration.
struct RemoteSessionApiConfig {
    std::string access_token;
    std::string org_uuid;
    std::string base_url;
};

/// Result of polling a remote session event stream page set.
struct PollRemoteSessionResponse {
    std::vector<std::string> new_events;
    std::optional<std::string> last_event_id;
    std::optional<std::string> branch;
    std::optional<std::string> session_status;
};

/// Check if an error is a transient network error that should be retried.
[[nodiscard]] bool is_transient_network_error(int http_status, bool has_response);

/// Prepare API request credentials (access token + org UUID).
[[nodiscard]] std::expected<ApiCredentials, std::string> prepare_api_request();

/// Get OAuth headers for API requests.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
get_oauth_headers(std::string_view access_token);

/// Fetch code sessions from the Sessions API (/v1/sessions).
[[nodiscard]] std::expected<std::vector<CodeSession>, std::string>
fetch_code_sessions_from_sessions_api();

/// Fetch a single session by ID.
[[nodiscard]] std::expected<SessionResource, std::string>
fetch_session(std::string_view session_id);

/// Extract the first branch name from a session's git repository outcomes.
[[nodiscard]] std::optional<std::string>
get_branch_from_session(const SessionResource& session);

/// Send a user message event to an existing remote session.
[[nodiscard]] std::expected<bool, std::string>
send_event_to_remote_session(
    std::string_view session_id,
    const RemoteMessageContent& content);

/// Send a user message event with explicit UUID for dedup.
[[nodiscard]] std::expected<bool, std::string>
send_event_to_remote_session(
    std::string_view session_id,
    const RemoteMessageContent& content,
    std::string_view uuid);

/// Update the title of an existing remote session.
[[nodiscard]] std::expected<bool, std::string>
update_session_title(std::string_view session_id, std::string_view title);

/// Resolve credentials and base URL for the Sessions API.
[[nodiscard]] std::expected<RemoteSessionApiConfig, std::string>
default_remote_session_api_config();

/// Poll remote session events after the given event ID.
[[nodiscard]] std::expected<PollRemoteSessionResponse, std::string>
poll_remote_session_events(
    const RemoteSessionApiConfig& config,
    std::string_view session_id,
    std::optional<std::string> after_id = std::nullopt,
    bool skip_metadata = false);

/// Archive an existing remote session.
[[nodiscard]] std::expected<void, std::string>
archive_remote_session(const RemoteSessionApiConfig& config, std::string_view session_id);

namespace detail {

[[nodiscard]] inline std::string trim_copy(std::string_view value) {
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(begin, end);
}

[[nodiscard]] inline std::optional<std::string> env_string(std::string_view name) {
    if (const char* value = std::getenv(std::string(name).c_str())) {
        auto trimmed = trim_copy(value);
        if (!trimmed.empty()) return trimmed;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> first_env(std::initializer_list<std::string_view> names) {
    for (auto name : names) {
        if (auto value = env_string(name)) return value;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string strip_trailing_slashes(std::string value) {
    while (value.size() > std::string_view("https://x").size() && value.ends_with('/')) {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] inline fs::path home_path() {
    if (const char* home = std::getenv("HOME")) return fs::path(home);
    return {};
}

[[nodiscard]] inline std::vector<fs::path> credential_file_candidates() {
    std::vector<fs::path> paths;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        paths.push_back(fs::path(xdg) / "cc-repl" / "credentials.json");
    }
    auto home = home_path();
    if (!home.empty()) {
        paths.push_back(home / ".config" / "cc-repl" / "credentials.json");
        paths.push_back(home / ".claude" / "credentials.json");
        paths.push_back(home / ".claude" / "auth_token.json");
    }
    return paths;
}

[[nodiscard]] inline std::optional<std::string> json_string_field(
    cc::utils::json::JsonVal obj,
    std::initializer_list<std::string_view> keys
) {
    if (!obj.is_obj()) return std::nullopt;
    for (auto key : keys) {
        auto val = obj.get(key);
        if (val.is_str()) {
            auto text = trim_copy(val.as_str());
            if (!text.empty()) return text;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> json_string_field_recursive(
    cc::utils::json::JsonVal node,
    std::initializer_list<std::string_view> keys,
    int depth = 0
) {
    if (!node.valid() || depth > 4) return std::nullopt;
    if (auto direct = json_string_field(node, keys)) return direct;
    if (node.is_obj()) {
        std::optional<std::string> found;
        node.iter_obj([&](cc::utils::json::JsonVal, cc::utils::json::JsonVal value) {
            if (!found) found = json_string_field_recursive(value, keys, depth + 1);
        });
        return found;
    }
    if (node.is_arr()) {
        std::optional<std::string> found;
        node.iter([&](cc::utils::json::JsonVal item) {
            if (!found) found = json_string_field_recursive(item, keys, depth + 1);
        });
        return found;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> credential_file_field(
    std::initializer_list<std::string_view> keys
) {
    for (const auto& path : credential_file_candidates()) {
        if (path.empty() || !fs::exists(path)) continue;
        auto parsed = cc::utils::json::parse_file(path);
        if (!parsed || !parsed->root().is_obj()) continue;
        if (auto value = json_string_field_recursive(parsed->root(), keys)) return value;
    }
    return std::nullopt;
}

	[[nodiscard]] inline std::string url_encode(std::string_view value) {
	    std::string out;
	    out.reserve(value.size());
	    for (unsigned char ch : value) {
        const bool unreserved =
            std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(ch));
        } else {
            out += std::format("%{:02X}", ch);
        }
    }
	    return out;
	}

	[[nodiscard]] inline std::string json_escape(std::string_view value) {
	    std::string out;
	    out.reserve(value.size() + 8);
	    for (unsigned char ch : value) {
	        switch (ch) {
	            case '"': out += "\\\""; break;
	            case '\\': out += "\\\\"; break;
	            case '\b': out += "\\b"; break;
	            case '\f': out += "\\f"; break;
	            case '\n': out += "\\n"; break;
	            case '\r': out += "\\r"; break;
	            case '\t': out += "\\t"; break;
	            default:
	                if (ch < 0x20) {
	                    out += std::format("\\u{:04x}", static_cast<unsigned>(ch));
	                } else {
	                    out.push_back(static_cast<char>(ch));
	                }
	        }
	    }
	    return out;
	}

	[[nodiscard]] inline std::string json_string(std::string_view value) {
	    return "\"" + json_escape(value) + "\"";
	}

	[[nodiscard]] inline std::string pseudo_uuid() {
	    static std::atomic<std::uint64_t> counter{0};
	    const auto now = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
	    return std::format("cpp-{:x}-{:x}", static_cast<std::uint64_t>(now), counter.fetch_add(1));
	}

	[[nodiscard]] inline std::optional<std::string> parse_github_repository(std::string_view url) {
	    auto value = trim_copy(url);
	    if (value.ends_with(".git")) value.resize(value.size() - 4);
	    std::string_view view(value);
	    if (auto pos = view.find("github.com/"); pos != std::string_view::npos) {
	        view.remove_prefix(pos + std::string_view("github.com/").size());
	    } else if (auto ssh = view.find("github.com:"); ssh != std::string_view::npos) {
	        view.remove_prefix(ssh + std::string_view("github.com:").size());
	    }
	    while (view.starts_with('/')) view.remove_prefix(1);
	    const auto slash = view.find('/');
	    if (slash == std::string_view::npos || slash == 0) return std::nullopt;
	    auto owner = view.substr(0, slash);
	    auto rest = view.substr(slash + 1);
	    const auto next = rest.find('/');
	    auto repo = next == std::string_view::npos ? rest : rest.substr(0, next);
	    if (repo.empty()) return std::nullopt;
	    return std::string(owner) + "/" + std::string(repo);
	}

	[[nodiscard]] inline CodeSessionStatus code_session_status_from_session(SessionStatus status) {
	    switch (status) {
	        case SessionStatus::requires_action: return CodeSessionStatus::waiting;
	        case SessionStatus::running: return CodeSessionStatus::working;
	        case SessionStatus::idle: return CodeSessionStatus::idle;
	        case SessionStatus::archived: return CodeSessionStatus::archived;
	    }
	    return CodeSessionStatus::working;
	}

	[[nodiscard]] inline std::string shell_quote(std::string_view value) {
	    std::string out = "'";
	    for (char ch : value) {
	        if (ch == '\'') out += "'\\''";
	        else out.push_back(ch);
	    }
	    out.push_back('\'');
	    return out;
	}

	struct ShellResult {
	    int status{0};
	    std::string output;
	};

	[[nodiscard]] inline ShellResult run_shell(std::string command) {
	    command += " 2>&1";
	    std::array<char, 4096> buffer{};
	    std::string output;
	    FILE* pipe = cc::utils::bash::popen_spawn(command.c_str());
	    if (!pipe) return ShellResult{.status = -1, .output = "popen failed"};
	    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
	        output += buffer.data();
	    }
	    return ShellResult{.status = cc::utils::bash::pclose_spawn(pipe), .output = trim_copy(output)};
	}

	[[nodiscard]] inline ShellResult run_git(const fs::path& cwd, std::string_view args) {
	    return run_shell("git -C " + shell_quote(cwd.string()) + " " + std::string(args));
	}

	[[nodiscard]] inline std::string session_url(
	    const RemoteSessionApiConfig& config,
    std::string_view session_id,
    std::string_view suffix = {}
) {
    return std::format(
        "{}/v1/sessions/{}{}",
        strip_trailing_slashes(config.base_url),
        url_encode(session_id),
        suffix);
}

[[nodiscard]] inline std::unordered_map<std::string, std::string> oauth_header_map(
    const RemoteSessionApiConfig& config
) {
    return {
        {"Authorization", "Bearer " + config.access_token},
        {"Content-Type", "application/json"},
        {"anthropic-version", "2023-06-01"},
        {"anthropic-beta", std::string(kCcrByocBeta)},
        {"x-organization-uuid", config.org_uuid},
    };
}

[[nodiscard]] inline cc::utils::HttpClient remote_http_client() {
    cc::utils::HttpConfig config;
    config.max_retries = 2;
    config.retry_backoff_ms = 50;
    return cc::utils::HttpClient(std::move(config));
}

[[nodiscard]] inline std::optional<std::string> json_last_id(cc::utils::json::JsonVal obj) {
    return json_string_field(obj, {"last_id", "lastId"});
}

[[nodiscard]] inline bool json_bool_field(cc::utils::json::JsonVal obj, std::initializer_list<std::string_view> keys) {
    if (!obj.is_obj()) return false;
    for (auto key : keys) {
        auto val = obj.get(key);
        if (val.is_bool()) return val.as_bool();
    }
    return false;
}

[[nodiscard]] inline SessionStatus session_status_from_string(std::string_view value) {
    if (value == "requires_action") return SessionStatus::requires_action;
    if (value == "archived") return SessionStatus::archived;
    if (value == "idle") return SessionStatus::idle;
    return SessionStatus::running;
}

[[nodiscard]] inline std::string session_status_to_string(SessionStatus status) {
    switch (status) {
        case SessionStatus::requires_action: return "requires_action";
        case SessionStatus::running: return "running";
        case SessionStatus::idle: return "idle";
        case SessionStatus::archived: return "archived";
    }
    return "running";
}

[[nodiscard]] inline SessionResource parse_session_resource(cc::utils::json::JsonVal root) {
    SessionResource session;
    session.id = json_string_field(root, {"id", "session_id", "sessionId"}).value_or("");
    session.title = json_string_field(root, {"title"});
    session.session_status = session_status_from_string(
        json_string_field(root, {"session_status", "sessionStatus", "status"}).value_or("running"));
    session.environment_id = json_string_field(root, {"environment_id", "environmentId"}).value_or("");
    session.created_at = json_string_field(root, {"created_at", "createdAt"}).value_or("");
    session.updated_at = json_string_field(root, {"updated_at", "updatedAt"}).value_or("");

    auto context = root.get("session_context");
    if (!context.is_obj()) context = root.get("sessionContext");
	    if (context.is_obj()) {
	        session.session_context.cwd = json_string_field(context, {"cwd"}).value_or("");
	        auto sources = context.get("sources");
	        if (sources.is_arr()) {
	            sources.iter([&](cc::utils::json::JsonVal item) {
	                if (!item.is_obj()) return;
	                auto type = json_string_field(item, {"type"});
	                if (!type) return;
	                if (*type == "git_repository") {
	                    GitSource source;
	                    source.url = json_string_field(item, {"url"}).value_or("");
	                    source.revision = json_string_field(item, {"revision"});
	                    source.allow_unrestricted_git_push =
	                        json_bool_field(item, {"allow_unrestricted_git_push", "allowUnrestrictedGitPush"});
	                    session.session_context.sources.emplace_back(std::move(source));
	                } else if (*type == "knowledge_base") {
	                    KnowledgeBaseSource source{
	                        .knowledge_base_id = json_string_field(item, {"knowledge_base_id", "knowledgeBaseId"}).value_or("")
	                    };
	                    session.session_context.sources.emplace_back(std::move(source));
	                }
	            });
	        }
	        auto outcomes = context.get("outcomes");
	        if (outcomes.is_arr()) {
            outcomes.iter([&](cc::utils::json::JsonVal item) {
                if (!item.is_obj()) return;
                auto git_info = item.get("git_info");
                if (!git_info.is_obj()) git_info = item.get("gitInfo");
                if (!git_info.is_obj()) return;

                GitRepositoryOutcome outcome;
                outcome.git_info.repo = json_string_field(git_info, {"repo", "repository"}).value_or("");
                auto branches = git_info.get("branches");
                if (branches.is_arr()) {
                    branches.iter([&](cc::utils::json::JsonVal branch) {
                        if (branch.is_str()) outcome.git_info.branches.emplace_back(branch.as_str());
                    });
                }
                session.session_context.outcomes.push_back(std::move(outcome));
            });
        }
    }

    return session;
}

[[nodiscard]] inline std::expected<SessionResource, std::string> fetch_session_with_config(
    const RemoteSessionApiConfig& config,
    std::string_view session_id
) {
    if (session_id.empty()) return std::unexpected("session_id is required");
    auto http = remote_http_client();
    auto response = http.get(session_url(config, session_id), oauth_header_map(config));
    if (!response) {
        return std::unexpected("Failed to fetch remote session: " + response.error().message);
    }
    if (response->status >= 500 || response->status == 404 || response->status == 401 || response->status == 403) {
        return std::unexpected(std::format(
            "Failed to fetch remote session {}: HTTP {} {}",
            session_id,
            response->status,
            response->body));
    }
    if (!response->is_ok()) {
        return std::unexpected(std::format(
            "Failed to fetch remote session {}: HTTP {}",
            session_id,
            response->status));
    }
    auto parsed = cc::utils::json::parse(response->body);
    if (!parsed || !parsed->root().is_obj()) {
        return std::unexpected("Remote session response was not a JSON object");
    }
    return parse_session_resource(parsed->root());
}

} // namespace detail

[[nodiscard]] inline bool is_transient_network_error(int http_status, bool has_response) {
    if (!has_response || http_status == 0) return true;
    return http_status == 429 || http_status >= 500;
}

[[nodiscard]] inline std::expected<ApiCredentials, std::string> prepare_api_request() {
    auto access_token = detail::first_env({
        "CC_REPL_REMOTE_OAUTH_TOKEN",
        "CLAUDE_CODE_OAUTH_TOKEN",
        "ANTHROPIC_OAUTH_TOKEN",
    }).or_else([&] {
        return detail::credential_file_field({"access_token", "accessToken"});
    });

    if (!access_token || access_token->empty()) {
        return std::unexpected(
            "No Claude.ai OAuth access token found. Set CC_REPL_REMOTE_OAUTH_TOKEN or run /login.");
    }

    auto org_uuid = detail::first_env({
        "CC_REPL_REMOTE_ORG_UUID",
        "CLAUDE_CODE_ORG_UUID",
        "ANTHROPIC_ORGANIZATION_UUID",
        "ANTHROPIC_ORGANIZATION_ID",
    }).or_else([&] {
        return detail::credential_file_field({
            "organization_uuid",
            "organizationUuid",
            "org_uuid",
            "orgUuid",
        });
    });

    if (!org_uuid || org_uuid->empty()) {
        return std::unexpected(
            "No organization UUID found for the remote Sessions API. Set CC_REPL_REMOTE_ORG_UUID.");
    }

    return ApiCredentials{.access_token = std::move(*access_token), .org_uuid = std::move(*org_uuid)};
}

	[[nodiscard]] inline std::vector<std::pair<std::string, std::string>>
	get_oauth_headers(std::string_view access_token) {
	    return {
	        {"Authorization", "Bearer " + std::string(access_token)},
	        {"Content-Type", "application/json"},
	        {"anthropic-version", "2023-06-01"},
	    };
	}

	[[nodiscard]] inline std::optional<CodeSessionRepo> code_session_repo_from_sources(
	    const SessionResource& session
	) {
	    for (const auto& source : session.session_context.sources) {
	        if (const auto* git = std::get_if<GitSource>(&source)) {
	            auto repo_path = detail::parse_github_repository(git->url);
	            if (!repo_path) continue;
	            const auto slash = repo_path->find('/');
	            if (slash == std::string::npos) continue;
	            return CodeSessionRepo{
	                .name = repo_path->substr(slash + 1),
	                .owner_login = repo_path->substr(0, slash),
	                .default_branch = git->revision,
	            };
	        }
	    }
	    return std::nullopt;
	}

	[[nodiscard]] inline CodeSession code_session_from_session_resource(const SessionResource& session) {
	    return CodeSession{
	        .id = session.id,
	        .title = session.title.value_or("Untitled"),
	        .description = "",
	        .status = detail::code_session_status_from_session(session.session_status),
	        .repo = code_session_repo_from_sources(session),
	        .turns = {},
	        .created_at = session.created_at,
	        .updated_at = session.updated_at,
	    };
	}

	[[nodiscard]] inline std::expected<std::vector<CodeSession>, std::string>
	fetch_code_sessions_from_sessions_api() {
	    auto config = default_remote_session_api_config();
	    if (!config) return std::unexpected(config.error());

	    auto http = detail::remote_http_client();
	    auto response = http.get(
	        detail::strip_trailing_slashes(config->base_url) + "/v1/sessions",
	        detail::oauth_header_map(*config));
	    if (!response) return std::unexpected("Failed to fetch code sessions: " + response.error().message);
	    if (!response->is_ok()) {
	        return std::unexpected(std::format(
	            "Failed to fetch code sessions: HTTP {} {}",
	            response->status,
	            response->body));
	    }

	    auto parsed = cc::utils::json::parse(response->body);
	    if (!parsed || !parsed->root().is_obj()) return std::unexpected("Code sessions response was not JSON");
	    auto data = parsed->root().get("data");
	    if (!data.is_arr()) return std::unexpected("Code sessions response did not include a data array");

	    std::vector<CodeSession> sessions;
	    data.iter([&](cc::utils::json::JsonVal item) {
	        if (!item.is_obj()) return;
	        auto session = detail::parse_session_resource(item);
	        if (!session.id.empty()) sessions.push_back(code_session_from_session_resource(session));
	    });
	    return sessions;
	}

	[[nodiscard]] inline cc::utils::json::JsonMutVal remote_message_content_to_json(
	    const RemoteMessageContent& content,
	    cc::utils::json::JsonMutDoc& doc
	) {
	    if (const auto* text = std::get_if<std::string>(&content)) {
	        return doc.string(*text);
	    }
	    auto arr = doc.array();
	    for (const auto& block_text : std::get<std::vector<std::string>>(content)) {
	        auto block = doc.object();
	        block.add("type", doc.string("text"));
	        block.add("text", doc.string(block_text));
	        arr.append(block);
	    }
	    return arr;
	}

	[[nodiscard]] inline std::string remote_session_event_body(
	    std::string_view session_id,
	    const RemoteMessageContent& content,
	    std::string_view uuid
	) {
	    cc::utils::json::JsonMutDoc doc;
	    auto root = doc.object();
	    auto events = doc.array();
	    auto event = doc.object();
	    event.add("uuid", doc.string(uuid));
	    event.add("session_id", doc.string(session_id));
	    event.add("type", doc.string("user"));
	    event.add("parent_tool_use_id", doc.null());
	    auto message = doc.object();
	    message.add("role", doc.string("user"));
	    message.add("content", remote_message_content_to_json(content, doc));
	    event.add("message", message);
	    events.append(event);
	    root.add("events", events);
	    doc.set_root(root);
	    return doc.to_string();
	}

	[[nodiscard]] inline std::expected<bool, std::string>
	send_event_to_remote_session(
	    std::string_view session_id,
	    const RemoteMessageContent& content) {
	    return send_event_to_remote_session(session_id, content, detail::pseudo_uuid());
	}

	[[nodiscard]] inline std::expected<bool, std::string>
	send_event_to_remote_session(
	    std::string_view session_id,
	    const RemoteMessageContent& content,
	    std::string_view uuid) {
	    if (session_id.empty()) return std::unexpected("session_id is required");
	    auto config = default_remote_session_api_config();
	    if (!config) return std::unexpected(config.error());
	    auto http = detail::remote_http_client();
	    auto response = http.post(
	        detail::session_url(*config, session_id, "/events"),
	        remote_session_event_body(session_id, content, uuid),
	        detail::oauth_header_map(*config));
	    if (!response) return std::unexpected("Failed to send remote session event: " + response.error().message);
	    if (response->status == 200 || response->status == 201) return true;
	    if (response->status < 500) return false;
	    return std::unexpected(std::format(
	        "Failed to send remote session event {}: HTTP {} {}",
	        session_id,
	        response->status,
	        response->body));
	}

	[[nodiscard]] inline std::expected<bool, std::string>
	update_session_title(std::string_view session_id, std::string_view title) {
	    if (session_id.empty()) return std::unexpected("session_id is required");
	    auto config = default_remote_session_api_config();
	    if (!config) return std::unexpected(config.error());
	    auto http = detail::remote_http_client();
	    auto response = http.patch(
	        detail::session_url(*config, session_id),
	        std::format(R"({{"title":{}}})", detail::json_string(title)),
	        detail::oauth_header_map(*config));
	    if (!response) return std::unexpected("Failed to update remote session title: " + response.error().message);
	    if (response->status == 200) return true;
	    if (response->status < 500) return false;
	    return std::unexpected(std::format(
	        "Failed to update remote session title {}: HTTP {} {}",
	        session_id,
	        response->status,
	        response->body));
	}

	[[nodiscard]] inline std::expected<SessionResource, std::string>
	fetch_session(std::string_view session_id) {
    auto config = default_remote_session_api_config();
    if (!config) return std::unexpected(config.error());
    return detail::fetch_session_with_config(*config, session_id);
}

[[nodiscard]] inline std::optional<std::string>
get_branch_from_session(const SessionResource& session) {
    for (const auto& outcome : session.session_context.outcomes) {
        if (!outcome.git_info.branches.empty()) return outcome.git_info.branches.front();
    }
    return std::nullopt;
}

[[nodiscard]] inline std::expected<RemoteSessionApiConfig, std::string>
default_remote_session_api_config() {
    auto credentials = prepare_api_request();
    if (!credentials) return std::unexpected(credentials.error());

    auto base_url = detail::first_env({
        "CC_REPL_REMOTE_API_BASE_URL",
        "CLAUDE_CODE_REMOTE_API_BASE_URL",
        "ANTHROPIC_BASE_URL",
    }).value_or("https://api.anthropic.com");

    return RemoteSessionApiConfig{
        .access_token = std::move(credentials->access_token),
        .org_uuid = std::move(credentials->org_uuid),
        .base_url = detail::strip_trailing_slashes(std::move(base_url)),
    };
}

[[nodiscard]] inline std::expected<PollRemoteSessionResponse, std::string>
poll_remote_session_events(
    const RemoteSessionApiConfig& config,
    std::string_view session_id,
    std::optional<std::string> after_id,
    bool skip_metadata
) {
    if (session_id.empty()) return std::unexpected("session_id is required");

    PollRemoteSessionResponse result;
    auto http = detail::remote_http_client();
    auto next_after = std::move(after_id);

    for (int page = 0; page < 50; ++page) {
        auto url = detail::session_url(config, session_id, "/events");
        if (next_after && !next_after->empty()) {
            url += "?after_id=" + detail::url_encode(*next_after);
        }

        auto response = http.get(url, detail::oauth_header_map(config));
        if (!response) {
            return std::unexpected("Failed to poll remote session events: " + response.error().message);
        }
        if (!response->is_ok()) {
            return std::unexpected(std::format(
                "Failed to poll remote session events for {}: HTTP {} {}",
                session_id,
                response->status,
                response->body));
        }

        auto parsed = cc::utils::json::parse(response->body);
        if (!parsed || !parsed->root().is_obj()) {
            return std::unexpected("Remote session events response was not a JSON object");
        }

        auto root = parsed->root();
        auto data = root.get("data");
        if (data.is_arr()) {
            data.iter([&](cc::utils::json::JsonVal event) {
                if (!event.is_obj()) return;
                auto type = detail::json_string_field(event, {"type"});
                if (!type || type->empty()) return;
                if (*type == "env_manager_log" || *type == "control_response") return;
                auto session = detail::json_string_field(event, {"session_id", "sessionId"});
                if (!session || session->empty()) return;
                result.new_events.push_back(cc::utils::json::to_string(event));
            });
        }

        auto last_id = detail::json_last_id(root);
        if (last_id && !last_id->empty()) {
            result.last_event_id = *last_id;
            next_after = std::move(last_id);
        }

        if (!detail::json_bool_field(root, {"has_more", "hasMore"})) break;
        if (!next_after || next_after->empty()) break;
    }

    if (!skip_metadata) {
        auto session = detail::fetch_session_with_config(config, session_id);
        if (!session) return std::unexpected(session.error());
        result.session_status = detail::session_status_to_string(session->session_status);
        result.branch = get_branch_from_session(*session);
    }

    return result;
}

[[nodiscard]] inline std::expected<void, std::string>
archive_remote_session(const RemoteSessionApiConfig& config, std::string_view session_id) {
    if (session_id.empty()) return std::unexpected("session_id is required");

    auto http = detail::remote_http_client();
    auto response = http.post(detail::session_url(config, session_id, "/archive"), "{}", detail::oauth_header_map(config));
    if (!response) {
        return std::unexpected("Failed to archive remote session: " + response.error().message);
    }
    if (response->is_ok() || response->status == 409) return {};
    return std::unexpected(std::format(
        "Failed to archive remote session {}: HTTP {} {}",
        session_id,
        response->status,
        response->body));
}

// =========================================================================
// environments.ts — Environment Management
// =========================================================================

/// Environment kind.
enum class EnvironmentKind {
    anthropic_cloud,
    byoc,
    bridge,
};

/// Environment state.
enum class EnvironmentState {
    active,
};

/// Environment resource from the API.
	struct EnvironmentResource {
	    EnvironmentKind kind;
	    std::string environment_id;
	    std::string name;
	    std::string created_at;
	    EnvironmentState state;
	};

	[[nodiscard]] inline EnvironmentKind environment_kind_from_string(std::string_view value) {
	    if (value == "byoc") return EnvironmentKind::byoc;
	    if (value == "bridge") return EnvironmentKind::bridge;
	    return EnvironmentKind::anthropic_cloud;
	}

	[[nodiscard]] inline std::string environment_kind_to_string(EnvironmentKind kind) {
	    switch (kind) {
	        case EnvironmentKind::anthropic_cloud: return "anthropic_cloud";
	        case EnvironmentKind::byoc: return "byoc";
	        case EnvironmentKind::bridge: return "bridge";
	    }
	    return "anthropic_cloud";
	}

	[[nodiscard]] inline EnvironmentState environment_state_from_string(std::string_view) {
	    return EnvironmentState::active;
	}

	[[nodiscard]] inline EnvironmentResource parse_environment_resource(cc::utils::json::JsonVal root) {
	    return EnvironmentResource{
	        .kind = environment_kind_from_string(
	            detail::json_string_field(root, {"kind"}).value_or("anthropic_cloud")),
	        .environment_id = detail::json_string_field(root, {"environment_id", "environmentId", "id"}).value_or(""),
	        .name = detail::json_string_field(root, {"name"}).value_or(""),
	        .created_at = detail::json_string_field(root, {"created_at", "createdAt"}).value_or(""),
	        .state = environment_state_from_string(
	            detail::json_string_field(root, {"state", "status"}).value_or("active")),
	    };
	}

	/// Fetch available environments from the Environment API.
	[[nodiscard]] inline std::expected<std::vector<EnvironmentResource>, std::string>
	fetch_environments() {
	    auto config = default_remote_session_api_config();
	    if (!config) return std::unexpected(config.error());

	    auto http = detail::remote_http_client();
	    auto response = http.get(
	        detail::strip_trailing_slashes(config->base_url) + "/v1/environment_providers",
	        detail::oauth_header_map(*config));
	    if (!response) return std::unexpected("Failed to fetch environments: " + response.error().message);
	    if (!response->is_ok()) {
	        return std::unexpected(std::format(
	            "Failed to fetch environments: HTTP {} {}",
	            response->status,
	            response->body));
	    }

	    auto parsed = cc::utils::json::parse(response->body);
	    if (!parsed || !parsed->root().is_obj()) return std::unexpected("Environment response was not JSON");
	    auto array = parsed->root().get("environments");
	    if (!array.is_arr()) array = parsed->root().get("data");
	    if (!array.is_arr()) return std::unexpected("Environment response did not include environments");

	    std::vector<EnvironmentResource> environments;
	    array.iter([&](cc::utils::json::JsonVal item) {
	        if (!item.is_obj()) return;
	        auto env = parse_environment_resource(item);
	        if (!env.environment_id.empty()) environments.push_back(std::move(env));
	    });
	    return environments;
	}

	/// Create a default anthropic_cloud environment.
	[[nodiscard]] inline std::expected<EnvironmentResource, std::string>
	create_default_cloud_environment(std::string_view name) {
	    auto config = default_remote_session_api_config();
	    if (!config) return std::unexpected(config.error());
	    auto body = std::format(
	        R"({{"name":{},"kind":"anthropic_cloud","description":"","config":{{"environment_type":"anthropic","cwd":"/home/user","init_script":null,"environment":{{}},"languages":[{{"name":"python","version":"3.11"}},{{"name":"node","version":"20"}}],"network_config":{{"allowed_hosts":[],"allow_default_hosts":true}}}}}})",
	        detail::json_string(name));
	    auto http = detail::remote_http_client();
	    auto response = http.post(
	        detail::strip_trailing_slashes(config->base_url) + "/v1/environment_providers/cloud/create",
	        body,
	        detail::oauth_header_map(*config));
	    if (!response) return std::unexpected("Failed to create default cloud environment: " + response.error().message);
	    if (!response->is_ok()) {
	        return std::unexpected(std::format(
	            "Failed to create default cloud environment: HTTP {} {}",
	            response->status,
	            response->body));
	    }
	    auto parsed = cc::utils::json::parse(response->body);
	    if (!parsed || !parsed->root().is_obj()) return std::unexpected("Create environment response was not JSON");
	    return parse_environment_resource(parsed->root());
	}

// =========================================================================
// environmentSelection.ts — Environment Selection
// =========================================================================

/// Setting source identifier for environment selection tracking.
enum class SettingSource {
    user_settings,
    project_settings,
    enterprise_settings,
    flag_settings,
};

/// Information about environment selection state.
struct EnvironmentSelectionInfo {
    std::vector<EnvironmentResource> available_environments;
    std::optional<EnvironmentResource> selected_environment;
    std::optional<SettingSource> selected_environment_source;
};

	/// Get info about available environments and the currently selected one.
	[[nodiscard]] inline std::expected<EnvironmentSelectionInfo, std::string>
	get_environment_selection_info() {
	    auto environments = fetch_environments();
	    if (!environments) return std::unexpected(environments.error());

	    EnvironmentSelectionInfo info;
	    info.available_environments = std::move(*environments);
	    if (info.available_environments.empty()) return info;

	    auto default_id = detail::first_env({
	        "CC_REPL_REMOTE_DEFAULT_ENVIRONMENT_ID",
	        "CLAUDE_CODE_REMOTE_DEFAULT_ENVIRONMENT_ID",
	        "ANTHROPIC_REMOTE_DEFAULT_ENVIRONMENT_ID",
	    });
	    if (default_id) {
	        auto it = std::ranges::find_if(info.available_environments, [&](const EnvironmentResource& env) {
	            return env.environment_id == *default_id;
	        });
	        if (it != info.available_environments.end()) {
	            info.selected_environment = *it;
	            info.selected_environment_source = SettingSource::flag_settings;
	            return info;
	        }
	    }

	    auto non_bridge = std::ranges::find_if(info.available_environments, [](const EnvironmentResource& env) {
	        return env.kind != EnvironmentKind::bridge;
	    });
	    info.selected_environment = non_bridge != info.available_environments.end()
	        ? *non_bridge
	        : info.available_environments.front();
	    return info;
	}

// =========================================================================
// gitBundle.ts — Git Bundle Creation & Upload
// =========================================================================

/// Default maximum bundle size (100MB).
inline constexpr std::size_t kDefaultBundleMaxBytes = 100 * 1024 * 1024;

/// Bundle scope: how much of the repo was packed.
enum class BundleScope {
    all,       // --all (full refs)
    head,      // HEAD only (current branch history)
    squashed,  // single parentless commit (snapshot only)
};

/// Reason a bundle operation failed.
enum class BundleFailReason {
    git_error,
    too_large,
    empty_repo,
};

/// Successful bundle upload result.
struct BundleUploadSuccess {
    std::string file_id;
    std::size_t bundle_size_bytes;
    BundleScope scope;
    bool has_wip;
};

/// Failed bundle upload result.
struct BundleUploadFailure {
    std::string error;
    std::optional<BundleFailReason> fail_reason;
};

/// Result of createAndUploadGitBundle.
using BundleUploadResult = std::expected<BundleUploadSuccess, BundleUploadFailure>;

/// Configuration for the Files API upload.
	struct FilesApiConfig {
	    std::string access_token;
	    std::string org_uuid;
	    std::string base_url;
	};

	namespace detail {

	[[nodiscard]] inline std::string files_api_url(const FilesApiConfig& config, std::string_view suffix = {}) {
	    return std::format("{}/v1/files{}", strip_trailing_slashes(config.base_url), suffix);
	}

	[[nodiscard]] inline std::unordered_map<std::string, std::string> files_api_headers(
	    const FilesApiConfig& config,
	    std::optional<std::string> content_type = std::nullopt
	) {
	    std::unordered_map<std::string, std::string> headers{
	        {"Authorization", "Bearer " + config.access_token},
	        {"anthropic-version", "2023-06-01"},
	        {"anthropic-beta", "files-api-2025-04-14,oauth-2025-04-20"},
	    };
	    if (!config.org_uuid.empty()) headers.emplace("x-organization-uuid", config.org_uuid);
	    if (content_type) headers.emplace("Content-Type", std::move(*content_type));
	    return headers;
	}

	[[nodiscard]] inline std::expected<std::string, std::string> read_binary_file(const fs::path& path) {
	    std::ifstream input(path, std::ios::binary);
	    if (!input) return std::unexpected("failed to read file: " + path.string());
	    std::ostringstream buffer;
	    buffer << input.rdbuf();
	    return buffer.str();
	}

	struct UploadedFile {
	    std::string file_id;
	    std::size_t size{};
	};

	[[nodiscard]] inline std::expected<UploadedFile, std::string> upload_file(
	    const fs::path& file_path,
	    std::string_view relative_path,
	    const FilesApiConfig& config
	) {
	    auto content = read_binary_file(file_path);
	    if (!content) return std::unexpected(content.error());

	    auto boundary = "----CcReplFormBoundary" + pseudo_uuid();
	    auto filename = fs::path(relative_path).filename().string();
	    if (filename.empty()) filename = "_source_seed.bundle";

	    std::string body;
	    body.reserve(content->size() + 512);
	    body += "--" + boundary + "\r\n";
	    body += "Content-Disposition: form-data; name=\"file\"; filename=\"" + json_escape(filename) + "\"\r\n";
	    body += "Content-Type: application/octet-stream\r\n\r\n";
	    body += *content;
	    body += "\r\n";
	    body += "--" + boundary + "\r\n";
	    body += "Content-Disposition: form-data; name=\"purpose\"\r\n\r\n";
	    body += "user_data\r\n";
	    body += "--" + boundary + "--\r\n";

	    auto http = remote_http_client();
	    auto response = http.post(
	        files_api_url(config),
	        body,
	        files_api_headers(config, "multipart/form-data; boundary=" + boundary));
	    if (!response) return std::unexpected("Files API upload failed: " + response.error().message);
	    if (!response->is_ok()) {
	        return std::unexpected(std::format(
	            "Files API upload failed: HTTP {} {}",
	            response->status,
	            response->body));
	    }
	    auto parsed = cc::utils::json::parse(response->body);
	    if (!parsed || !parsed->root().is_obj()) return std::unexpected("Files API upload response was not JSON");
	    auto id = json_string_field(parsed->root(), {"id", "file_id", "fileId"});
	    if (!id || id->empty()) return std::unexpected("Files API upload succeeded without a file id");
	    return UploadedFile{.file_id = std::move(*id), .size = content->size()};
	}

	struct BundleCreateResult {
	    bool ok{false};
	    std::size_t size{};
	    BundleScope scope{BundleScope::all};
	    std::string error;
	    BundleFailReason fail_reason{BundleFailReason::git_error};
	};

	[[nodiscard]] inline std::optional<std::size_t> bundle_file_size(const fs::path& path) {
	    std::error_code ec;
	    const auto size = fs::file_size(path, ec);
	    if (ec) return std::nullopt;
	    return static_cast<std::size_t>(size);
	}

	[[nodiscard]] inline BundleCreateResult bundle_with_fallback(
	    const fs::path& git_root,
	    const fs::path& bundle_path,
	    std::size_t max_bytes,
	    bool has_stash,
	    const std::function<bool()>& should_abort
	) {
	    auto extra = has_stash ? std::string{" refs/seed/stash"} : std::string{};
	    auto mk_bundle = [&](std::string_view base) {
	        return run_git(git_root, "bundle create " + shell_quote(bundle_path.string()) + " " + std::string(base) + extra);
	    };
	    auto success = [](std::size_t size, BundleScope scope) {
	        BundleCreateResult result;
	        result.ok = true;
	        result.size = size;
	        result.scope = scope;
	        return result;
	    };

	    if (should_abort && should_abort()) {
	        return BundleCreateResult{
	            .ok = false,
	            .error = "bundle creation aborted",
	            .fail_reason = BundleFailReason::git_error,
	        };
	    }

	    auto all = mk_bundle("--all");
	    if (all.status != 0) {
	        return BundleCreateResult{
	            .ok = false,
	            .error = "git bundle create --all failed: " + all.output,
	            .fail_reason = BundleFailReason::git_error,
	        };
	    }
	    if (auto size = bundle_file_size(bundle_path); size && *size <= max_bytes) {
	        return success(*size, BundleScope::all);
	    }

	    if (should_abort && should_abort()) {
	        return BundleCreateResult{
	            .ok = false,
	            .error = "bundle creation aborted",
	            .fail_reason = BundleFailReason::git_error,
	        };
	    }

	    auto head = mk_bundle("HEAD");
	    if (head.status != 0) {
	        return BundleCreateResult{
	            .ok = false,
	            .error = "git bundle create HEAD failed: " + head.output,
	            .fail_reason = BundleFailReason::git_error,
	        };
	    }
	    if (auto size = bundle_file_size(bundle_path); size && *size <= max_bytes) {
	        return success(*size, BundleScope::head);
	    }

	    const auto tree_ref = has_stash ? std::string{"refs/seed/stash^{tree}"} : std::string{"HEAD^{tree}"};
	    auto commit = run_git(git_root, "commit-tree " + shell_quote(tree_ref) + " -m seed");
	    if (commit.status != 0 || commit.output.empty()) {
	        return BundleCreateResult{
	            .ok = false,
	            .error = "git commit-tree failed: " + commit.output,
	            .fail_reason = BundleFailReason::git_error,
	        };
	    }
	    auto update_root = run_git(git_root, "update-ref refs/seed/root " + shell_quote(commit.output));
	    if (update_root.status != 0) {
	        return BundleCreateResult{
	            .ok = false,
	            .error = "git update-ref refs/seed/root failed: " + update_root.output,
	            .fail_reason = BundleFailReason::git_error,
	        };
	    }
	    auto squashed = mk_bundle("refs/seed/root");
	    if (squashed.status != 0) {
	        return BundleCreateResult{
	            .ok = false,
	            .error = "git bundle create refs/seed/root failed: " + squashed.output,
	            .fail_reason = BundleFailReason::git_error,
	        };
	    }
	    if (auto size = bundle_file_size(bundle_path); size && *size <= max_bytes) {
	        return success(*size, BundleScope::squashed);
	    }

	    return BundleCreateResult{
	        .ok = false,
	        .error = "Repo is too large to bundle. Please setup GitHub on https://claude.ai/code",
	        .fail_reason = BundleFailReason::too_large,
	    };
	}

	[[nodiscard]] inline std::optional<fs::path> find_git_root(std::string_view cwd) {
	    auto root = run_git(fs::path(cwd), "rev-parse --show-toplevel");
	    if (root.status != 0 || root.output.empty()) return std::nullopt;
	    return fs::path(root.output);
	}

	[[nodiscard]] inline std::size_t bundle_max_bytes() {
	    if (auto configured = env_string("CC_REPL_CCR_BUNDLE_MAX_BYTES")) {
	        try {
	            return static_cast<std::size_t>(std::stoull(*configured));
	        } catch (...) {
	        }
	    }
	    return kDefaultBundleMaxBytes;
	}

	} // namespace detail

	[[nodiscard]] BundleUploadResult create_and_upload_git_bundle(
	    const FilesApiConfig& config,
	    std::string_view cwd,
	    std::function<bool()> should_abort);

	/// Create a git bundle of the repository and upload to the Files API.
	/// Fallback chain: --all -> HEAD -> squashed-root.
	/// Captures tracked WIP via stash create.
	[[nodiscard]] inline BundleUploadResult create_and_upload_git_bundle(
	    const FilesApiConfig& config) {
	    return create_and_upload_git_bundle(config, fs::current_path().string(), [] { return false; });
	}

	/// Create a git bundle with explicit working directory and abort signal.
	[[nodiscard]] inline BundleUploadResult create_and_upload_git_bundle(
	    const FilesApiConfig& config,
	    std::string_view cwd,
	    std::function<bool()> should_abort) {
	    auto git_root = detail::find_git_root(cwd);
	    if (!git_root) {
	        return std::unexpected(BundleUploadFailure{
	            .error = "Not in a git repository",
	            .fail_reason = BundleFailReason::git_error,
	        });
	    }

	    (void)detail::run_git(*git_root, "update-ref -d refs/seed/stash");
	    (void)detail::run_git(*git_root, "update-ref -d refs/seed/root");

	    auto refs = detail::run_git(*git_root, "for-each-ref --count=1 refs/");
	    if (refs.status == 0 && refs.output.empty()) {
	        return std::unexpected(BundleUploadFailure{
	            .error = "Repository has no commits yet",
	            .fail_reason = BundleFailReason::empty_repo,
	        });
	    }

	    auto stash = detail::run_git(*git_root, "stash create");
	    const bool has_wip = stash.status == 0 && !stash.output.empty();
	    if (has_wip) {
	        (void)detail::run_git(*git_root, "update-ref refs/seed/stash " + detail::shell_quote(stash.output));
	    }

	    auto bundle_path = fs::temp_directory_path() /
	        std::format("ccr-seed-{}.bundle", detail::pseudo_uuid());
	    auto cleanup = [&] {
	        std::error_code ec;
	        fs::remove(bundle_path, ec);
	        (void)detail::run_git(*git_root, "update-ref -d refs/seed/stash");
	        (void)detail::run_git(*git_root, "update-ref -d refs/seed/root");
	    };

	    auto bundle = detail::bundle_with_fallback(
	        *git_root,
	        bundle_path,
	        detail::bundle_max_bytes(),
	        has_wip,
	        should_abort);
	    if (!bundle.ok) {
	        cleanup();
	        return std::unexpected(BundleUploadFailure{
	            .error = std::move(bundle.error),
	            .fail_reason = bundle.fail_reason,
	        });
	    }

	    auto uploaded = detail::upload_file(bundle_path, "_source_seed.bundle", config);
	    cleanup();
	    if (!uploaded) {
	        return std::unexpected(BundleUploadFailure{
	            .error = uploaded.error(),
	            .fail_reason = std::nullopt,
	        });
	    }

	    return BundleUploadSuccess{
	        .file_id = std::move(uploaded->file_id),
	        .bundle_size_bytes = uploaded->size,
	        .scope = bundle.scope,
	        .has_wip = has_wip,
	    };
	}

} // namespace cc::utils::teleport
