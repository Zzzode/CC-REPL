/// @file runtime_message_delivery.cppm
/// @brief Cross-session message delivery (UDS, Bridge) and in-process
/// "native agent resume + mailbox" delivery, extracted from the (former)
/// monolithic runtime_registry.cppm as part of audit §13 #1.
///
/// Contents (all were previously inline helpers / lambda bodies inside the
/// `send_message` branch of `execute_simple_runtime_tool`):
///   * Peer-address parsing (uds:/bridge:/plain-id)
///   * UDS + HTTP-Bridge cross-session transports
///   * Structured message payloads (shutdown request/response, plan approval)
///   * Queued-message id formatting
///   * Native agent resume plumbing (env-credential check, cwd recovery,
///     agent-input JSON building, ToolRegistry invocation with rollback on
///     failure via mark_failed)
///   * The `execute_send_message` dispatcher itself — thin wrappers in the
///     registry just forward to this function.
module;

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

export module cc.tools.runtime_message_delivery;

import cc.tools.tool;
import cc.tools.agent_runtime;       // NativeAgentRecord / native_agent_store
import cc.tools.send_message;        // SendMessageTool / MessagePriority / DeliveryStatus
import cc.tools.team;                // TeamMember / global_team_store
import cc.tools.runtime_shared_utils;
import cc.utils.json;
import cc.utils.http;
import cc.utils.uuid_utils;
import cc.utils.team_helpers;

export namespace cc::tools::runtime_message_delivery {

namespace fs = std::filesystem;
namespace json = cc::utils::json;
using cc::core::ErrorCode;
using cc::core::Result;
using cc::core::ToolInput;
using cc::core::ToolRegistry;
using cc::core::ToolResult;
using cc::tools::agent_runtime::NativeAgentRecord;
using cc::tools::agent_runtime::NativeAgentStatus;

// ---------------------------------------------------------------------------
//  1. Types
// ---------------------------------------------------------------------------

struct StructuredSendMessagePayload {
    std::string text;
    std::optional<std::string> request_id;
    std::string type;
};

enum class RuntimePeerAddressScheme {
    Other,
    Uds,
    Bridge,
};

struct RuntimePeerAddress {
    RuntimePeerAddressScheme scheme{RuntimePeerAddressScheme::Other};
    std::string target;
};

// ---------------------------------------------------------------------------
//  2. Peer address + helper accessors (also re-used by dispatcher branches)
// ---------------------------------------------------------------------------

[[nodiscard]] inline RuntimePeerAddress parse_runtime_peer_address(std::string_view to) {
    if (to.starts_with("uds:")) {
        return RuntimePeerAddress{.scheme = RuntimePeerAddressScheme::Uds, .target = std::string(to.substr(4))};
    }
    if (to.starts_with("bridge:")) {
        return RuntimePeerAddress{.scheme = RuntimePeerAddressScheme::Bridge, .target = std::string(to.substr(7))};
    }
    if (to.starts_with("/")) {
        return RuntimePeerAddress{.scheme = RuntimePeerAddressScheme::Uds, .target = std::string(to)};
    }
    return RuntimePeerAddress{.scheme = RuntimePeerAddressScheme::Other, .target = std::string(to)};
}

[[nodiscard]] inline std::optional<std::string> runtime_json_string(json::JsonVal obj, std::string_view key) {
    auto val = obj.get(key);
    if (!val.is_str()) return std::nullopt;
    return std::string(val.as_str());
}

[[nodiscard]] inline std::optional<bool> runtime_json_bool(json::JsonVal obj, std::string_view key) {
    auto val = obj.get(key);
    if (!val.is_bool()) return std::nullopt;
    return val.as_bool();
}

[[nodiscard]] inline std::optional<bool> runtime_json_semantic_bool(
    json::JsonVal obj,
    std::string_view key
) {
    auto val = obj.get(key);
    if (val.is_bool()) return val.as_bool();
    if (!val.is_str()) return std::nullopt;
    std::string normalized(val.as_str());
    std::ranges::transform(normalized, normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (normalized == "true" || normalized == "yes" || normalized == "y" ||
        normalized == "1" || normalized == "approve" || normalized == "approved") {
        return true;
    }
    if (normalized == "false" || normalized == "no" || normalized == "n" ||
        normalized == "0" || normalized == "reject" || normalized == "rejected" ||
        normalized == "deny" || normalized == "denied") {
        return false;
    }
    return std::nullopt;
}

/// Build a small JSON object with a given set of string / bool fields. Used
/// by the structured-message builders to emit typed runtime payloads.
[[nodiscard]] inline std::string build_runtime_json_object(
    std::initializer_list<std::pair<std::string_view, std::string_view>> strings,
    std::initializer_list<std::pair<std::string_view, bool>> bools = {}
) {
    json::JsonMutDoc doc;
    auto root = doc.object();
    for (const auto& [key, value] : strings) root.add(key, doc.string(value));
    for (const auto& [key, value] : bools) root.add(key, doc.boolean(value));
    doc.set_root(root);
    return doc.to_string();
}

// ---------------------------------------------------------------------------
//  3. Cross-session messaging
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::string build_cross_session_prompt(
    std::string_view from_agent,
    std::string_view message
) {
    return std::format(
        "<cross-session-message from=\"{}\">\n{}\n</cross-session-message>",
        runtime_shared_utils::escape_xml(from_agent),
        runtime_shared_utils::escape_xml(message));
}

[[nodiscard]] inline std::string build_uds_cross_session_payload(
    std::string_view from_agent,
    std::string_view message
) {
    const auto prompt = build_cross_session_prompt(from_agent, message);
    json::JsonBuilder b;
    b.str("type", "cross_session_message");
    b.str("mode", "prompt");
    b.str("from", from_agent);
    b.str("message", message);
    b.str("value", prompt);
    return b.serialize() + "\n";
}

[[nodiscard]] inline std::optional<std::string> runtime_env_value(const char* name) {
    if (const char* value = std::getenv(name); value && *value) return std::string(value);
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> first_runtime_env(std::initializer_list<const char*> names) {
    for (const auto* name : names) {
        if (auto value = runtime_env_value(name)) return value;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string strip_runtime_trailing_slashes(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

[[nodiscard]] inline bool is_safe_runtime_session_id(std::string_view id) {
    if (id.empty()) return false;
    for (const auto ch : id) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') return false;
    }
    return true;
}

[[nodiscard]] inline std::string build_bridge_cross_session_event(
    std::string_view source_session_id,
    std::string_view target_session_id,
    std::string_view message
) {
    const auto prompt = build_cross_session_prompt(source_session_id, message);
    const auto uuid = cc::utils::generate_uuid_v4();
    json::JsonMutDoc doc;
    auto root = doc.object();
    root.add("type", doc.string("user"));
    auto message_obj = doc.object();
    message_obj.add("role", doc.string("user"));
    message_obj.add("content", doc.string(prompt));
    root.add("message", message_obj);
    root.add("parent_tool_use_id", doc.null());
    root.add("session_id", doc.string(target_session_id));
    root.add("uuid", doc.string(uuid));
    doc.set_root(root);
    return doc.to_string();
}

[[nodiscard]] inline std::expected<void, std::string> send_bridge_cross_session_message(
    std::string_view target_session_id,
    std::string_view message
) {
    if (!is_safe_runtime_session_id(target_session_id)) {
        return std::unexpected("Invalid bridge session ID");
    }

    auto endpoint = first_runtime_env({
        "CLAUDE_CODE_REMOTE_API_BASE_URL",
        "CC_REPL_REMOTE_API_BASE_URL",
        "CLAUDE_CODE_SESSION_INGRESS_URL",
        "CC_REPL_SESSION_INGRESS_URL",
    });
    auto source_session_id = first_runtime_env({
        "CC_REMOTE_SESSION_ID",
        "CLAUDE_CODE_REMOTE_SESSION_ID",
    });
    auto auth_token = runtime_env_value("CLAUDE_CODE_SESSION_ACCESS_TOKEN");
    if (!endpoint || !source_session_id || !auth_token) {
        return std::unexpected(
            "Remote Control is not connected - cannot send to a bridge: target. Reconnect with /remote-control first.");
    }
    if (!is_safe_runtime_session_id(*source_session_id)) {
        return std::unexpected("Invalid active bridge session ID");
    }

    std::unordered_map<std::string, std::string> headers{
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
    };
    if (auth_token->starts_with("sk-ant-sid")) {
        headers["Cookie"] = "sessionKey=" + *auth_token;
        if (auto org = runtime_env_value("CLAUDE_CODE_ORGANIZATION_UUID")) {
            headers["X-Organization-Uuid"] = *org;
        }
    } else {
        headers["Authorization"] = "Bearer " + *auth_token;
    }

    const auto event = build_bridge_cross_session_event(*source_session_id, target_session_id, message);
    const auto body = std::format(R"({{"events":[{}]}})", event);
    const auto url = std::format(
        "{}/v1/sessions/{}/events",
        strip_runtime_trailing_slashes(*endpoint),
        target_session_id);

    cc::utils::HttpClient http;
    auto response = http.post(url, body, headers);
    if (!response) return std::unexpected(response.error().message);
    if (!response->is_ok()) {
        return std::unexpected(std::format("Session ingress returned HTTP {}", response->status));
    }
    return {};
}

[[nodiscard]] inline std::expected<void, std::string> send_uds_cross_session_message(
    std::string_view socket_path,
    std::string_view from_agent,
    std::string_view message
) {
    if (socket_path.empty()) return std::unexpected("address target must not be empty");
#ifdef _WIN32
    (void)from_agent;
    (void)message;
    return std::unexpected("Unix domain socket messaging is not available on Windows");
#else
    if (socket_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        return std::unexpected("Unix domain socket path is too long");
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return std::unexpected("Failed to create Unix domain socket");

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socket_path.data(), socket_path.size());
    addr.sun_path[socket_path.size()] = '\0';

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const auto error = std::string(std::strerror(errno));
        ::close(fd);
        return std::unexpected("Failed to connect Unix domain socket: " + error);
    }

    auto payload = build_uds_cross_session_payload(from_agent, message);
    std::string_view remaining(payload);
    while (!remaining.empty()) {
        const auto sent = ::send(fd, remaining.data(), remaining.size(), 0);
        if (sent <= 0) {
            const auto error = std::string(std::strerror(errno));
            ::close(fd);
            return std::unexpected("Failed to send Unix domain socket message: " + error);
        }
        remaining.remove_prefix(static_cast<std::size_t>(sent));
    }
    ::shutdown(fd, SHUT_WR);
    ::close(fd);
    return {};
#endif
}

// ---------------------------------------------------------------------------
//  4. Structured message payloads (shutdown, plan approval)
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::expected<StructuredSendMessagePayload, std::string>
build_structured_send_message_payload(
    json::JsonVal message,
    std::string_view from_agent
) {
    auto type = runtime_json_string(message, "type");
    if (!type || type->empty()) return std::unexpected("structured message requires type");

    if (*type == "shutdown_request") {
        auto request_id = "shutdown-" + runtime_shared_utils::runtime_delivery_message_id()
            .substr(std::string_view("msg-").size());
        auto reason = runtime_json_string(message, "reason").value_or("");
        auto text = build_runtime_json_object({
            {"type", "shutdown_request"},
            {"requestId", request_id},
            {"from", from_agent},
            {"reason", reason},
            {"timestamp", runtime_shared_utils::runtime_timestamp_string()},
        });
        return StructuredSendMessagePayload{
            .text = std::move(text),
            .request_id = std::move(request_id),
            .type = *type,
        };
    }
    if (*type == "shutdown_response") {
        auto request_id = runtime_json_string(message, "request_id")
            .or_else([&] { return runtime_json_string(message, "requestId"); });
        if (!request_id || request_id->empty()) {
            return std::unexpected("shutdown_response requires request_id");
        }
        auto approve = runtime_json_semantic_bool(message, "approve")
            .or_else([&] { return runtime_json_semantic_bool(message, "approved"); });
        if (!approve) return std::unexpected("shutdown_response requires approve");

        if (*approve) {
            auto text = build_runtime_json_object({
                {"type", "shutdown_approved"},
                {"requestId", *request_id},
                {"from", from_agent},
                {"timestamp", runtime_shared_utils::runtime_timestamp_string()},
            });
            return StructuredSendMessagePayload{
                .text = std::move(text),
                .request_id = *request_id,
                .type = *type,
            };
        }
        auto reason = runtime_json_string(message, "reason");
        if (!reason || reason->empty()) {
            return std::unexpected("reason is required when rejecting a shutdown request");
        }
        auto text = build_runtime_json_object({
            {"type", "shutdown_rejected"},
            {"requestId", *request_id},
            {"from", from_agent},
            {"reason", *reason},
            {"timestamp", runtime_shared_utils::runtime_timestamp_string()},
        });
        return StructuredSendMessagePayload{
            .text = std::move(text),
            .request_id = *request_id,
            .type = *type,
        };
    }
    if (*type == "plan_approval_response") {
        auto request_id = runtime_json_string(message, "request_id")
            .or_else([&] { return runtime_json_string(message, "requestId"); });
        if (!request_id || request_id->empty()) {
            return std::unexpected("plan_approval_response requires request_id");
        }
        auto approve = runtime_json_semantic_bool(message, "approve")
            .or_else([&] { return runtime_json_semantic_bool(message, "approved"); });
        if (!approve) return std::unexpected("plan_approval_response requires approve");

        const auto feedback = runtime_json_string(message, "feedback");
        const auto permission_mode = runtime_json_string(message, "permission_mode")
            .or_else([&] { return runtime_json_string(message, "permissionMode"); });
        json::JsonBuilder b;
        b.str("type", "plan_approval_response");
        b.str("requestId", *request_id);
        b.boolean("approved", *approve);
        b.opt_str("feedback", feedback);
        b.opt_str("permissionMode", permission_mode);
        b.str("timestamp", runtime_shared_utils::runtime_timestamp_string());
        return StructuredSendMessagePayload{
            .text = b.serialize(),
            .request_id = *request_id,
            .type = *type,
        };
    }

    return std::unexpected("unsupported structured message type: " + *type);
}

// ---------------------------------------------------------------------------
//  5. Native agent resume (hooks used by the mailbox delivery path)
// ---------------------------------------------------------------------------

[[nodiscard]] inline bool runtime_has_agent_api_credentials() {
    if (auto* key = std::getenv("ANTHROPIC_API_KEY"); key && key[0] != '\0') return true;
    if (auto* token = std::getenv("CLAUDE_AUTH_TOKEN"); token && token[0] != '\0') return true;
    return false;
}

[[nodiscard]] inline bool native_agent_can_resume_locally(const NativeAgentRecord& record) {
    if (record.remote_session_id && !record.remote_session_id->empty()) return false;
    if (record.teammate_backend && !record.teammate_backend->empty() &&
        *record.teammate_backend != "in-process") {
        return false;
    }
    return true;
}

[[nodiscard]] inline std::optional<std::string> native_agent_resume_cwd(
    const NativeAgentRecord& record
) {
    if (record.worktree_path && !record.worktree_path->empty()) {
        std::error_code ec;
        const fs::path candidate{*record.worktree_path};
        if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec)) {
            return record.worktree_path;
        }
    }
    if (record.cwd && !record.cwd->empty()) return record.cwd;
    return std::nullopt;
}

[[nodiscard]] inline std::string build_native_agent_resume_input_json(
    const NativeAgentRecord& record
) {
    json::JsonBuilder b;
    b.str("agent_id", record.agent_id);
    b.str("subagent_type", record.agent_type);
    b.str("prompt",
         "Resume this existing background agent. Process queued follow-up messages and continue from the persisted agent context.");
    b.boolean("run_in_background", true);
    b.boolean("resume_existing", true);
    b.opt_str("description", record.description);
    b.opt_str("mode", record.mode);
    if (auto cwd = native_agent_resume_cwd(record)) {
        b.str("cwd", *cwd);
    }
    return b.serialize();
}

[[nodiscard]] inline std::string runtime_tool_result_text(const ToolResult& result) {
    std::string out;
    for (const auto& content : result.content) {
        if (!out.empty()) out += '\n';
        out += content.text;
    }
    return out;
}

[[nodiscard]] inline std::optional<std::string> try_start_native_agent_resume(
    const NativeAgentRecord& record,
    ToolRegistry* registry
) {
    if (!registry) return "background resume deferred: runtime registry is not attached";
    if (!native_agent_can_resume_locally(record)) {
        return "background resume deferred: agent is managed by a remote or external teammate backend";
    }
    if (!runtime_has_agent_api_credentials()) {
        return "background resume deferred: no Anthropic API credentials are configured";
    }

    auto started = registry->execute(
        "Agent",
        ToolInput::from_json(build_native_agent_resume_input_json(record)));
    if (!started) {
        auto error = "background resume failed: " + started.error().message;
        agent_runtime::native_agent_store().mark_failed(record.agent_id, error);
        return error;
    }
    auto text = runtime_tool_result_text(*started);
    if (started->is_error) {
        auto error = "background resume failed: " + text;
        agent_runtime::native_agent_store().mark_failed(record.agent_id, error);
        return error;
    }
    return "background resume started";
}

// ---------------------------------------------------------------------------
//  6. Dispatcher body
//     Mirrors the ~250 lines that were inline inside execute_simple_runtime_tool's
//     `if (name == "send_message")` branch — but now a testable free function.
// ---------------------------------------------------------------------------

struct MailboxTarget {
    std::string agent_id;
    std::string recipient_name;
    std::string team_name;
};

struct DeliveryOutcome {
    std::string target_agent;
    std::string message_id;
    DeliveryStatus delivery_status = DeliveryStatus::Delivered;
    bool resumed_terminal_agent = false;
    std::optional<std::string> resume_status_note;
};

[[nodiscard]] inline std::string team_agent_name_from_id(std::string_view agent_id) {
    if (auto at = agent_id.find('@'); at != std::string_view::npos) {
        return std::string(agent_id.substr(0, at));
    }
    return std::string(agent_id);
}

[[nodiscard]] inline Result<ToolResult> execute_send_message(
    std::string_view input_json,
    ToolRegistry* registry,
    bool (*status_is_terminal)(NativeAgentStatus)
) {
    auto parsed = json::parse(input_json);
    if (!parsed || !parsed->root().is_obj()) return ToolResult::error("send_message requires a JSON object input");
    auto root = parsed->root();

    auto recipient = runtime_json_string(root, "to")
        .or_else([&] { return runtime_json_string(root, "target_agent"); })
        .or_else([&] { return runtime_json_string(root, "target"); })
        .or_else([&] { return runtime_json_string(root, "recipient"); });
    if (!recipient || recipient->empty()) return ToolResult::error("send_message requires to or target_agent");
    const auto peer_address = parse_runtime_peer_address(*recipient);
    if (peer_address.scheme != RuntimePeerAddressScheme::Other && peer_address.target.empty()) {
        return ToolResult::error("address target must not be empty");
    }

    MessagePriority priority = MessagePriority::Normal;
    auto priority_text = runtime_json_string(root, "priority").value_or("normal");
    if (priority_text == "low") priority = MessagePriority::Low;
    else if (priority_text == "high") priority = MessagePriority::High;
    else if (priority_text == "urgent") priority = MessagePriority::Urgent;

    auto from_agent = runtime_json_string(root, "from_agent")
        .or_else([&] { return runtime_json_string(root, "from"); })
        .or_else([&] { return cc::utils::get_agent_name(); })
        .value_or("team-lead");
    auto team_name = runtime_json_string(root, "team_name")
        .or_else([&] { return cc::utils::get_team_name(); });
    auto summary = runtime_json_string(root, "summary");

    auto message_node = root.get("message");
    auto message = runtime_json_string(root, "content");
    std::optional<StructuredSendMessagePayload> structured_payload;
    if (!message && message_node.is_str()) {
        message = std::string(message_node.as_str());
    } else if (!message && message_node.is_obj()) {
        if (peer_address.scheme != RuntimePeerAddressScheme::Other) {
            return ToolResult::error("structured messages cannot be sent cross-session - only plain text");
        }
        if (*recipient == "*") {
            return ToolResult::error("structured messages cannot be broadcast (to: \"*\")");
        }
        auto structured_type = runtime_json_string(message_node, "type");
        if (structured_type && *structured_type == "shutdown_response" && *recipient != "team-lead") {
            return ToolResult::error("shutdown_response must be sent to \"team-lead\"");
        }
        auto built = build_structured_send_message_payload(message_node, from_agent);
        if (!built) return ToolResult::error(built.error());
        message = built->text;
        structured_payload = std::move(*built);
    }
    if (!message || message->empty()) return ToolResult::error("send_message requires content or message");

    if (peer_address.scheme == RuntimePeerAddressScheme::Bridge) {
        auto sent = send_bridge_cross_session_message(peer_address.target, *message);
        if (!sent) return ToolResult::error("Failed to send to " + *recipient + ": " + sent.error());
        const auto preview = summary.value_or(*message);
        return ToolResult::success(std::format(
            "\"{}\" -> {}",
            runtime_shared_utils::escape_xml(preview.size() > 50 ? preview.substr(0, 50) : preview),
            *recipient));
    }
    if (peer_address.scheme == RuntimePeerAddressScheme::Uds) {
        auto sent = send_uds_cross_session_message(peer_address.target, from_agent, *message);
        if (!sent) return ToolResult::error("Failed to send to " + *recipient + ": " + sent.error());
        const auto preview = summary.value_or(*message);
        return ToolResult::success(std::format(
            "\"{}\" -> {}",
            runtime_shared_utils::escape_xml(preview.size() > 50 ? preview.substr(0, 50) : preview),
            *recipient));
    }

    auto lower_ascii = [](std::string_view value) {
        std::string out(value);
        std::ranges::transform(out, out.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return out;
    };

    auto find_team_member = [&](std::string_view target) -> std::optional<MailboxTarget> {
        if (!team_name || team_name->empty()) return std::nullopt;
        auto team = cc::tools::global_team_store().get_by_id_or_name(*team_name);
        if (!team) return std::nullopt;
        const auto target_lower = lower_ascii(target);
        for (const auto& member : (*team)->members) {
            const auto member_name = team_agent_name_from_id(member.agent_id);
            if (lower_ascii(member.agent_id) != target_lower && lower_ascii(member_name) != target_lower) {
                continue;
            }
            return MailboxTarget{
                .agent_id = member.agent_id,
                .recipient_name = member_name,
                .team_name = (*team)->name,
            };
        }
        return std::nullopt;
    };

    auto deliver_to_target = [&](std::string target_agent) -> std::expected<DeliveryOutcome, std::string> {
        auto mailbox_target = find_team_member(target_agent);
        if (mailbox_target) target_agent = mailbox_target->agent_id;

        auto recipient_record = cc::tools::agent_runtime::native_agent_store().get(target_agent);
        if (!recipient_record) {
            for (const auto& candidate : cc::tools::agent_runtime::native_agent_store().list()) {
                const auto candidate_name = candidate.name.value_or(team_agent_name_from_id(candidate.agent_id));
                if (candidate_name != target_agent && candidate.agent_id != target_agent) continue;
                if (team_name && (!candidate.team_name || *candidate.team_name != *team_name)) continue;
                recipient_record = candidate;
                target_agent = candidate.agent_id;
                if (!mailbox_target && candidate.team_name && !candidate.team_name->empty()) {
                    mailbox_target = MailboxTarget{
                        .agent_id = candidate.agent_id,
                        .recipient_name = candidate.name.value_or(team_agent_name_from_id(candidate.agent_id)),
                        .team_name = *candidate.team_name,
                    };
                }
                break;
            }
        }

        DeliveryOutcome outcome{
            .target_agent = target_agent,
            .message_id = runtime_shared_utils::runtime_delivery_message_id(),
            .delivery_status = DeliveryStatus::Delivered,
            .resumed_terminal_agent = false,
            .resume_status_note = std::nullopt,
        };

        if (recipient_record) {
            SendMessageTool validator(from_agent);
            if (auto valid = validator.validate(target_agent, *message); !valid) {
                return std::unexpected(std::string(format_error(valid.error())));
            }
            outcome.resumed_terminal_agent = status_is_terminal(recipient_record->status);
            cc::tools::agent_runtime::native_agent_store().enqueue_resume_message(
                target_agent,
                runtime_shared_utils::format_agent_pending_user_message(from_agent, priority, *message));
            if (outcome.resumed_terminal_agent) {
                auto queued_record = cc::tools::agent_runtime::native_agent_store().get(target_agent)
                    .value_or(*recipient_record);
                outcome.resume_status_note = try_start_native_agent_resume(queued_record, registry);
            }
            cc::tools::agent_runtime::native_agent_store().append_transcript(
                target_agent,
                std::format(
                    "message {} from {} [{}]: {}",
                    outcome.message_id,
                    from_agent,
                    message_priority_name(priority),
                    *message));
            if (!mailbox_target && recipient_record->team_name && !recipient_record->team_name->empty()) {
                mailbox_target = MailboxTarget{
                    .agent_id = target_agent,
                    .recipient_name = recipient_record->name.value_or(team_agent_name_from_id(target_agent)),
                    .team_name = *recipient_record->team_name,
                };
            }
        } else if (!mailbox_target) {
            SendMessageTool tool(from_agent);
            auto sent = tool.execute(target_agent, *message, priority, runtime_json_string(root, "reply_to"));
            if (!sent) return std::unexpected(std::string(format_error(sent.error())));
            outcome.message_id = sent->message_id;
            outcome.delivery_status = sent->status;
        }

        if (mailbox_target) {
            auto mailbox = cc::utils::write_to_mailbox(
                mailbox_target->recipient_name,
                cc::utils::TeammateMessage{
                    .from = from_agent,
                    .text = *message,
                    .timestamp = {},
                    .read = false,
                    .color = cc::utils::get_teammate_color(),
                    .summary = summary,
                },
                std::optional<std::string_view>{std::string_view(mailbox_target->team_name)});
            if (!mailbox) {
                return std::unexpected("Delivered to runtime queue but failed to write teammate mailbox: " + mailbox.error());
            }
        }

        return outcome;
    };

    if (*recipient == "*") {
        if (!team_name || team_name->empty()) {
            return ToolResult::error("send_message broadcast requires team_name or active team context");
        }
        auto team = cc::tools::global_team_store().get_by_id_or_name(*team_name);
        if (!team) return ToolResult::error("Team not found: " + *team_name);

        std::vector<std::string> recipients;
        const auto sender_lower = lower_ascii(from_agent);
        for (const auto& member : (*team)->members) {
            const auto member_name = team_agent_name_from_id(member.agent_id);
            if (lower_ascii(member.agent_id) == sender_lower || lower_ascii(member_name) == sender_lower) {
                continue;
            }
            auto delivered = deliver_to_target(member.agent_id);
            if (!delivered) return ToolResult::error(delivered.error());
            recipients.push_back(member_name);
        }

        if (recipients.empty()) {
            return ToolResult::success("No teammates to broadcast to (you are the only team member)");
        }

        std::string joined;
        for (std::size_t i = 0; i < recipients.size(); ++i) {
            if (i != 0) joined += ", ";
            joined += recipients[i];
        }
        return ToolResult::success(std::format(
            "Message broadcast to {} teammate(s): {}",
            recipients.size(),
            joined));
    }

    auto delivered = deliver_to_target(*recipient);
    if (!delivered) return ToolResult::error(delivered.error());
    auto status_note = delivered->resumed_terminal_agent
        ? std::string{"\nAgent was stopped and has been queued for background resume."}
        : std::string{};
    if (delivered->resume_status_note && !delivered->resume_status_note->empty()) {
        status_note += "\n" + *delivered->resume_status_note;
    }
    if (structured_payload && structured_payload->request_id) {
        status_note += "\nrequest_id: " + *structured_payload->request_id;
    }
    return ToolResult::success(std::format(
        "Delivered message {} to {} [{}]{}",
        delivered->message_id,
        delivered->target_agent,
        delivery_status_name(delivered->delivery_status),
        status_note));
}

} // namespace cc::tools::runtime_message_delivery
