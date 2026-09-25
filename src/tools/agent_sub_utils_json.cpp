// Implementation unit for cc.tools.agent.utils — JSON getters/parsers,
// tool-input agent_id injection and cwd-override shaping,
// parse_agent_tool_request, content-block/message JSON (de)serialization,
// and sidechain/message JSON.
module;

module cc.tools.agent.utils;

import std;

import cc.utils.json;
import cc.services.api.client;
import cc.tools.tool;

namespace cc::tools::agent::utils {

namespace fs = std::filesystem;

[[nodiscard]] std::string json_escape_string(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': escaped += R"(\\)"; break;
            case '"': escaped += R"(\")"; break;
            case '\b': escaped += R"(\b)"; break;
            case '\f': escaped += R"(\f)"; break;
            case '\n': escaped += R"(\n)"; break;
            case '\r': escaped += R"(\r)"; break;
            case '\t': escaped += R"(\t)"; break;
            default:
                if (ch < 0x20) {
                    escaped += std::format(R"(\u{:04x})", static_cast<unsigned int>(ch));
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

std::string inject_agent_id_into_tool_input(
    std::string_view raw_json,
    std::string_view agent_id
) {
    if (agent_id.empty()) return std::string(raw_json);

    auto doc = cc::utils::json::parse(raw_json);
    if (!doc) return std::string(raw_json);
    auto root = doc->root();
    if (!root.valid() || !root.is_obj()) return std::string(raw_json);
    if (root.get("agent_id").valid() || root.get("agentId").valid()) {
        return std::string(raw_json);
    }

    std::size_t open_brace = 0;
    while (open_brace < raw_json.size() && std::isspace(static_cast<unsigned char>(raw_json[open_brace]))) {
        ++open_brace;
    }
    if (open_brace >= raw_json.size() || raw_json[open_brace] != '{') {
        return std::string(raw_json);
    }

    std::size_t next = open_brace + 1;
    while (next < raw_json.size() && std::isspace(static_cast<unsigned char>(raw_json[next]))) {
        ++next;
    }
    const bool object_empty = next < raw_json.size() && raw_json[next] == '}';

    std::string scoped;
    scoped.reserve(raw_json.size() + agent_id.size() + 24);
    scoped.append(raw_json.substr(0, open_brace + 1));
    scoped += R"("agent_id":")";
    scoped += json_escape_string(agent_id);
    scoped += '"';
    if (!object_empty) scoped += ',';
    scoped.append(raw_json.substr(open_brace + 1));
    return scoped;
}

std::string inject_agent_id_into_todo_input(
    std::string_view raw_json,
    std::string_view agent_id
) {
    return inject_agent_id_into_tool_input(raw_json, agent_id);
}

void append_json_string_field(
    std::string& out,
    std::string_view name,
    std::string_view value,
    bool& first
) {
    if (!first) out += ',';
    first = false;
    out += '"';
    out += json_escape_string(name);
    out += R"(":")";
    out += json_escape_string(value);
    out += '"';
}

void append_json_optional_string_field(
    std::string& out,
    std::string_view name,
    const std::optional<std::string>& value,
    bool& first
) {
    if (value && !value->empty()) append_json_string_field(out, name, *value, first);
}

[[nodiscard]] SchemaProperty agent_schema_property(
    std::string name,
    std::string type,
    std::string description,
    bool required
) {
    return SchemaProperty{
        std::move(name),
        std::move(type),
        std::move(description),
        required,
        std::nullopt,
        std::nullopt
    };
}

[[nodiscard]] std::optional<std::string> json_string(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    if (!value.is_str()) return std::nullopt;
    return std::string(value.as_str());
}

[[nodiscard]] bool json_bool(
    cc::utils::json::JsonVal root,
    std::string_view key,
    bool fallback
) {
    auto value = root.get(key);
    return value.is_bool() ? value.as_bool() : fallback;
}

[[nodiscard]] std::optional<int> json_int(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    if (!value.is_num()) return std::nullopt;
    return static_cast<int>(value.as_int());
}

[[nodiscard]] bool has_non_empty_string(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    return value.is_str() && !value.as_str().empty();
}

[[nodiscard]] std::optional<std::string> resolve_agent_relative_path(
    const std::optional<std::string>& working_dir,
    std::string_view value
) {
    if (!working_dir || working_dir->empty() || value.empty()) return std::nullopt;
    fs::path path{std::string(value)};
    if (path.is_absolute()) return path.lexically_normal().string();
    return (fs::path{*working_dir} / path).lexically_normal().string();
}

[[nodiscard]] std::string json_object_with_string_overrides(
    std::string_view raw_json,
    const std::unordered_map<std::string, std::string>& overrides
) {
    if (overrides.empty()) return std::string(raw_json);
    auto parsed = cc::utils::json::parse(raw_json);
    if (!parsed || !parsed->root().is_obj()) return std::string(raw_json);

    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    std::unordered_set<std::string> written;
    parsed->root().iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str()) return;
        auto key_text = std::string(key.as_str());
        if (auto override = overrides.find(key_text); override != overrides.end()) {
            root.add(key_text, doc.string(override->second));
            written.insert(std::move(key_text));
            return;
        }
        root.add(key_text, doc.copy_val(value));
    });
    for (const auto& [key, value] : overrides) {
        if (!written.contains(key)) root.add(key, doc.string(value));
    }
    doc.set_root(root);
    return doc.to_string();
}

[[nodiscard]] std::vector<std::string> json_string_array(cc::utils::json::JsonVal value) {
    std::vector<std::string> out;
    if (!value.valid()) return out;
    if (value.is_arr()) {
        value.iter([&](cc::utils::json::JsonVal item) {
            if (item.is_str()) {
                out.emplace_back(item.as_str());
            } else if (item.valid()) {
                out.push_back(item.to_string());
            }
        });
    } else if (value.is_str()) {
        out.emplace_back(value.as_str());
    }
    return out;
}

[[nodiscard]] std::vector<std::string> json_string_array_field(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    return json_string_array(root.get(key));
}

[[nodiscard]] bool json_array_looks_like_content_blocks(cc::utils::json::JsonVal value) {
    if (!value.valid() || !value.is_arr() || value.size() == 0) return false;
    bool saw_content_block = false;
    bool saw_message = false;
    value.iter([&](cc::utils::json::JsonVal item) {
        if (item.is_str()) {
            saw_content_block = true;
            return;
        }
        if (!item.is_obj()) return;
        if (item.get("message").is_obj() || item.get("role").is_str()) {
            saw_message = true;
            return;
        }
        if (item.get("type").is_str()) saw_content_block = true;
    });
    return saw_content_block && !saw_message;
}

[[nodiscard]] std::string assistant_content_array_json_to_message_json(std::string_view content_json) {
    std::string out = R"({"role":"assistant","content":)";
    out += content_json;
    out += '}';
    return out;
}

[[nodiscard]] std::vector<std::string> json_message_entries(cc::utils::json::JsonVal value) {
    std::vector<std::string> entries;
    if (!value.valid()) return entries;
    if (value.is_str()) {
        entries.emplace_back(value.as_str());
        return entries;
    }
    if (value.is_obj()) {
        entries.push_back(value.to_string());
        return entries;
    }
    if (!value.is_arr()) return entries;

    if (json_array_looks_like_content_blocks(value)) {
        entries.push_back(assistant_content_array_json_to_message_json(value.to_string()));
        return entries;
    }

    value.iter([&](cc::utils::json::JsonVal item) {
        if (item.is_str()) {
            entries.emplace_back(item.as_str());
        } else if (item.valid()) {
            entries.push_back(item.to_string());
        }
    });
    return entries;
}

[[nodiscard]] std::vector<std::string> json_message_entries_field(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    return json_message_entries(root.get(key));
}

[[nodiscard]] bool agent_tool_input_omits_agent_type(std::string_view raw_json) {
    auto doc = cc::utils::json::parse(raw_json);
    if (!doc) return true;
    auto root = doc->root();
    if (!root.valid() || !root.is_obj()) return true;

    auto type = json_string(root, "subagent_type").or_else([&] { return json_string(root, "skill"); });
    return !type || type->empty();
}

[[nodiscard]] std::expected<AgentToolRequest, std::string> parse_agent_tool_request(
    const ToolInput& input
) {
    auto doc = cc::utils::json::parse(input.json());
    if (!doc) return std::unexpected(std::string(doc.error().format()));

    auto root = doc->root();
    if (!root.is_obj()) return std::unexpected("Agent input must be a JSON object");

    AgentToolRequest request;
    if (auto prompt = json_string(root, "prompt").or_else([&] { return json_string(root, "task"); })) {
        request.prompt = *prompt;
    }
    request.description = json_string(root, "description");
    if (auto subagent = json_string(root, "subagent_type").or_else([&] { return json_string(root, "skill"); })) {
        if (!subagent->empty()) request.subagent_type = *subagent;
    }
    request.model = resolve_agent_model(json_string(root, "model"));
    request.run_in_background = json_bool(root, "run_in_background", false);
    request.agent_id_override = json_string(root, "agent_id").or_else([&] { return json_string(root, "agentId"); });
    request.resume_existing = json_bool(root, "resume_existing", false) ||
        json_bool(root, "resumeExisting", false);
    request.query_source = json_string(root, "query_source").or_else([&] { return json_string(root, "querySource"); });
    request.fork_child_context = json_bool(root, "fork_child", false) ||
        json_bool(root, "forkChild", false) ||
        (request.query_source && query_source_is_fork_child(*request.query_source)) ||
        text_contains_fork_boilerplate(request.prompt);
    request.parent_system_prompt = json_string(root, "parent_system_prompt")
        .or_else([&] { return json_string(root, "parentSystemPrompt"); })
        .or_else([&] { return json_string(root, "override_system_prompt"); })
        .or_else([&] { return json_string(root, "system_prompt_override"); });
    request.exact_tools = json_string_array_field(root, "exact_tools");
    if (request.exact_tools.empty()) request.exact_tools = json_string_array_field(root, "exactTools");
    if (request.exact_tools.empty()) request.exact_tools = json_string_array_field(root, "available_tools");
    if (request.exact_tools.empty()) request.exact_tools = json_string_array_field(root, "availableTools");
    request.use_exact_tools = json_bool(root, "use_exact_tools", false) ||
        json_bool(root, "useExactTools", false) ||
        !request.exact_tools.empty();
    request.fork_context_entries = json_string_array_field(root, "fork_context");
    if (request.fork_context_entries.empty()) {
        request.fork_context_entries = json_string_array_field(root, "forkContext");
    }
    if (request.fork_context_entries.empty()) {
        request.fork_context_entries = json_string_array_field(root, "fork_context_messages");
    }
    if (request.fork_context_entries.empty()) {
        request.fork_context_entries = json_string_array_field(root, "forkContextMessages");
    }
    request.parent_assistant_message_entries = json_message_entries_field(root, "parent_assistant_message");
    if (request.parent_assistant_message_entries.empty()) {
        request.parent_assistant_message_entries = json_message_entries_field(root, "parentAssistantMessage");
    }
    if (request.parent_assistant_message_entries.empty()) {
        request.parent_assistant_message_entries = json_message_entries_field(root, "assistant_message");
    }
    if (request.parent_assistant_message_entries.empty()) {
        request.parent_assistant_message_entries = json_message_entries_field(root, "assistantMessage");
    }
    if (request.parent_assistant_message_entries.empty()) {
        request.parent_assistant_message_entries = json_message_entries_field(root, "live_parent_assistant_message");
    }
    if (request.parent_assistant_message_entries.empty()) {
        request.parent_assistant_message_entries = json_message_entries_field(root, "liveParentAssistantMessage");
    }
    if (!request.parent_assistant_message_entries.empty()) request.fork_child_context = true;
    request.name = json_string(root, "name");
    request.team_name = json_string(root, "team_name").or_else([&] { return json_string(root, "teamName"); });
    request.mode = json_string(root, "mode")
        .or_else([&] { return json_string(root, "permission_mode"); })
        .or_else([&] { return json_string(root, "permissionMode"); });
    request.isolation = json_string(root, "isolation");
    request.cwd = json_string(root, "cwd");
    return request;
}

[[nodiscard]] std::string apply_agent_tool_execution_context_to_input(
    std::string_view tool_name,
    std::string_view raw_json,
    const AgentExecutionPlan& plan
) {
    auto scoped_json = (is_todo_write_tool_name(tool_name) ||
                        is_agent_scoped_shell_tool_name(tool_name))
        ? inject_agent_id_into_tool_input(raw_json, plan.agent_id)
        : std::string(raw_json);
    if (!plan.working_dir || plan.working_dir->empty()) return scoped_json;

    auto parsed = cc::utils::json::parse(scoped_json);
    if (!parsed || !parsed->root().is_obj()) return scoped_json;
    auto root = parsed->root();
    std::unordered_map<std::string, std::string> overrides;

    auto override_relative_path_field = [&](std::string_view key) {
        auto value = json_string(root, key);
        if (!value || value->empty()) return;
        if (auto resolved = resolve_agent_relative_path(plan.working_dir, *value)) {
            overrides.emplace(std::string(key), std::move(*resolved));
        }
    };
    auto default_or_override_path_field = [&](std::string_view key) {
        auto value = json_string(root, key);
        if (!value || value->empty()) {
            overrides.emplace(std::string(key), *plan.working_dir);
            return;
        }
        if (auto resolved = resolve_agent_relative_path(plan.working_dir, *value)) {
            overrides.emplace(std::string(key), std::move(*resolved));
        }
    };

    if (is_agent_scoped_shell_tool_name(tool_name)) {
        auto cwd = json_string(root, "cwd");
        if (!cwd || cwd->empty()) {
            overrides.emplace("cwd", *plan.working_dir);
        } else if (auto resolved = resolve_agent_relative_path(plan.working_dir, *cwd)) {
            overrides.emplace("cwd", std::move(*resolved));
        }
    } else if (normalized_tool_name_in(tool_name, {"Read", "Write", "Edit"})) {
        override_relative_path_field("file_path");
    } else if (normalized_tool_name_in(tool_name, {"notebook_edit", "NotebookEdit"})) {
        override_relative_path_field("notebook_path");
    } else if (normalized_tool_name_in(tool_name, {"Grep", "Glob"})) {
        default_or_override_path_field("path");
    }

    return json_object_with_string_overrides(scoped_json, overrides);
}

[[nodiscard]] std::string normalized_tool_input_json(std::string_view raw_json) {
    if (raw_json.empty()) return "{}";
    auto parsed = cc::utils::json::parse(raw_json);
    if (!parsed || !parsed->root().valid()) return "{}";
    return cc::utils::json::to_string(parsed->root());
}

[[nodiscard]] std::string message_content_sidechain_json(const Message& message) {
    std::string out = "[";
    bool first = true;
    auto begin_block = [&] {
        if (!first) out += ',';
        first = false;
        out += '{';
    };
    auto append_field_prefix = [&](std::string_view name, bool& first_field) {
        if (!first_field) out += ',';
        first_field = false;
        out += '"';
        out += json_escape_string(name);
        out += R"(":)";
    };
    auto append_string_field = [&](std::string_view name, std::string_view value, bool& first_field) {
        append_field_prefix(name, first_field);
        out += '"';
        out += json_escape_string(value);
        out += '"';
    };

    for (const auto& block : message.content) {
        begin_block();
        bool first_field = true;
        switch (block.type) {
            case ContentBlockType::Text:
                append_string_field("type", "text", first_field);
                append_string_field("text", block.text, first_field);
                break;
            case ContentBlockType::ToolUse:
                append_string_field("type", "tool_use", first_field);
                append_string_field("id", block.tool_use_id, first_field);
                append_string_field("name", block.tool_name, first_field);
                append_field_prefix("input", first_field);
                out += normalized_tool_input_json(block.tool_input_json);
                break;
            case ContentBlockType::ToolResult:
                append_string_field("type", "tool_result", first_field);
                append_string_field("tool_use_id", block.tool_use_id, first_field);
                append_field_prefix("content", first_field);
                out += R"([{"type":"text","text":")";
                out += json_escape_string(block.text);
                out += R"("}])";
                break;
            case ContentBlockType::Image:
            case ContentBlockType::Document:
                append_string_field(
                    "type",
                    block.type == ContentBlockType::Image ? "image" : "document",
                    first_field);
                append_field_prefix("source", first_field);
                out += R"({"type":"base64","media_type":")";
                out += json_escape_string(block.media_type);
                out += R"(","data":")";
                out += json_escape_string(block.image_data);
                out += R"("})";
                break;
            case ContentBlockType::Thinking:
                append_string_field("type", "thinking", first_field);
                append_string_field("thinking", block.thinking, first_field);
                if (!block.signature.empty()) append_string_field("signature", block.signature, first_field);
                break;
            case ContentBlockType::RedactedThinking:
                append_string_field("type", "redacted_thinking", first_field);
                append_string_field("data", block.thinking, first_field);
                break;
        }
        out += '}';
    }
    out += ']';
    return out;
}

[[nodiscard]] std::string json_string_member(
    cc::utils::json::JsonVal object,
    std::string_view key
) {
    if (!object.valid() || !object.is_obj()) return {};
    auto value = object.get(key);
    return value.is_str() ? std::string(value.as_str()) : std::string{};
}

[[nodiscard]] std::string text_from_json_content(cc::utils::json::JsonVal content) {
    if (!content.valid()) return {};
    if (content.is_str()) return std::string(content.as_str());
    if (!content.is_arr()) return {};

    std::string out;
    content.iter([&](cc::utils::json::JsonVal block) {
        std::string text;
        if (block.is_str()) {
            text = std::string(block.as_str());
        } else if (block.is_obj()) {
            auto type = json_string_member(block, "type");
            if (type == "text") {
                text = json_string_member(block, "text");
            } else if (type == "tool_result") {
                text = text_from_json_content(block.get("content"));
            }
        }
        if (text.empty()) return;
        if (!out.empty()) out += "\n";
        out += text;
    });
    return out;
}

[[nodiscard]] ContentBlock content_block_from_json(cc::utils::json::JsonVal block) {
    if (block.is_str()) {
        return ContentBlock{.type = ContentBlockType::Text, .text = std::string(block.as_str())};
    }
    if (!block.valid() || !block.is_obj()) {
        return ContentBlock{
            .type = ContentBlockType::Text,
            .text = block.valid() ? block.to_string() : std::string{},
        };
    }

    auto type = json_string_member(block, "type");
    if (type == "tool_use") {
        return ContentBlock{
            .type = ContentBlockType::ToolUse,
            .tool_use_id = json_string_member(block, "id"),
            .tool_name = json_string_member(block, "name"),
            .tool_input_json = block.get("input").valid() ? block.get("input").to_string() : std::string{"{}"},
        };
    }
    if (type == "tool_result") {
        return ContentBlock{
            .type = ContentBlockType::ToolResult,
            .text = text_from_json_content(block.get("content")),
            .tool_use_id = json_string_member(block, "tool_use_id"),
        };
    }
    if (type == "thinking") {
        return ContentBlock{
            .type = ContentBlockType::Thinking,
            .thinking = json_string_member(block, "thinking"),
            .signature = json_string_member(block, "signature"),
        };
    }
    if (type == "redacted_thinking") {
        return ContentBlock{
            .type = ContentBlockType::RedactedThinking,
            .thinking = json_string_member(block, "data"),
        };
    }

    auto text = json_string_member(block, "text");
    if (text.empty()) text = text_from_json_content(block.get("content"));
    return ContentBlock{.type = ContentBlockType::Text, .text = std::move(text)};
}

[[nodiscard]] std::optional<Message> message_from_json_value(cc::utils::json::JsonVal root) {
    if (!root.valid() || !root.is_obj()) return std::nullopt;

    auto message_obj = root.get("message");
    auto source = message_obj.is_obj() ? message_obj : root;
    auto role = json_string_member(source, "role");
    if (role.empty()) role = json_string_member(root, "type");
    role = role == "assistant" ? "assistant" : "user";

    auto content = source.get("content");
    if (content.is_str()) {
        return Message::from_text(role, content.as_str());
    }

    Message message;
    message.role = std::move(role);
    if (content.is_arr()) {
        content.iter([&](cc::utils::json::JsonVal block) {
            auto parsed = content_block_from_json(block);
            if (parsed.type == ContentBlockType::Text && parsed.text.empty()) return;
            message.content.push_back(std::move(parsed));
        });
    }

    if (message.content.empty()) {
        auto text = json_string_member(root, "raw");
        if (text.empty()) text = json_string_member(root, "text");
        if (text.empty()) text = json_string_member(root, "content");
        if (!text.empty()) return Message::from_text(message.role, text);
    }
    if (message.content.empty()) return std::nullopt;
    return message;
}

[[nodiscard]] std::string message_json_object(const Message& message) {
    std::string out = R"({"role":")";
    out += json_escape_string(message.role);
    out += R"(","content":)";
    out += message_content_sidechain_json(message);
    out += '}';
    return out;
}

} // namespace cc::tools::agent::utils
