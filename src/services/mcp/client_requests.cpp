// Implementation unit for cc.services.mcp.client — the public JSON-RPC API
// methods (tools/resources/prompts). Separate from client_protocol.cpp so a
// body edit to one RPC method recompiles this smaller object only.
module;

module cc.services.mcp.client;

import std;

import cc.services.mcp.types;
import cc.utils.json;

namespace cc::services::mcp {

using namespace cc::utils::json;

// List available tools from the server
McpResult<ListToolsResult> McpClient::list_tools() {
    if (state_ != ServerState::Ready) {
        return std::unexpected(McpClientError::NotConnected);
    }

    auto response = send_request_sync("tools/list", std::nullopt);
    if (!response) {
        return std::unexpected(response.error());
    }

    auto result = parse_list_tools_result(*response);
    if (!result) {
        return std::unexpected(McpClientError::InvalidResponse);
    }

    cached_tools_ = result->tools;
    return *result;
}

// Call a tool on the server
McpResult<ToolCallResult> McpClient::call_tool(const ToolCallRequest& request) {
    if (state_ != ServerState::Ready) {
        return std::unexpected(McpClientError::NotConnected);
    }

    JsonMutDoc doc;
    auto params = doc.object();
    params.add("name", doc.string(request.name));

    // Parse and add arguments
    auto args_doc = parse(request.arguments_json);
    if (args_doc) {
        params.add("arguments", doc.copy_val(args_doc->root()));
    } else {
        params.add("arguments", doc.object());
    }

    doc.set_root(params);
    auto params_json = doc.to_string();

    auto response = send_request_sync("tools/call", params_json);
    if (!response) {
        return std::unexpected(response.error());
    }

    auto result = parse_tool_call_result(*response);
    if (!result) {
        return std::unexpected(McpClientError::InvalidResponse);
    }

    return *result;
}

// List available resources
McpResult<ListResourcesResult> McpClient::list_resources() {
    if (state_ != ServerState::Ready) {
        return std::unexpected(McpClientError::NotConnected);
    }

    auto response = send_request_sync("resources/list", std::nullopt);
    if (!response) {
        return std::unexpected(response.error());
    }

    // Parse resources
    ListResourcesResult result;
    auto doc = parse(*response);
    if (doc) {
        auto root = doc->root();
        auto result_node = root.get("result");
        if (result_node.is_obj()) {
            auto resources_node = result_node.get("resources");
            if (resources_node.is_arr()) {
                resources_node.iter([&result](JsonVal res_val) {
                    if (res_val.is_obj()) {
                        McpResource resource;
                        resource.uri = std::string(res_val.get("uri").as_str());
                        resource.name = std::string(res_val.get("name").as_str());
                        resource.description = std::string(res_val.get("description").as_str());
                        resource.mime_type = std::string(res_val.get("mimeType").as_str());
                        result.resources.push_back(std::move(resource));
                    }
                });
            }
        }
    }

    cached_resources_ = result.resources;
    return result;
}

// Read a specific resource
McpResult<ResourceReadResult> McpClient::read_resource(std::string_view uri) {
    if (state_ != ServerState::Ready) {
        return std::unexpected(McpClientError::NotConnected);
    }

    JsonMutDoc doc;
    auto params = doc.object();
    params.add("uri", doc.string(uri));
    doc.set_root(params);

    auto response = send_request_sync("resources/read", doc.to_string());
    if (!response) {
        return std::unexpected(response.error());
    }

    ResourceReadResult result;
    auto resp_doc = parse(*response);
    if (resp_doc) {
        auto root = resp_doc->root();
        auto result_node = root.get("result");
        if (result_node.is_obj()) {
            auto contents_node = result_node.get("contents");
            if (contents_node.is_arr()) {
                contents_node.iter([&result](JsonVal content_val) {
                    if (content_val.is_obj()) {
                        ResourceContent content;
                        content.uri = std::string(content_val.get("uri").as_str());
                        content.mime_type = std::string(content_val.get("mimeType").as_str());
                        content.text = std::string(content_val.get("text").as_str());
                        content.blob = std::string(content_val.get("blob").as_str());
                        result.contents.push_back(std::move(content));
                    }
                });
            }
        }
    }

    return result;
}

// List available prompts
McpResult<ListPromptsResult> McpClient::list_prompts() {
    if (state_ != ServerState::Ready) {
        return std::unexpected(McpClientError::NotConnected);
    }

    auto response = send_request_sync("prompts/list", std::nullopt);
    if (!response) {
        return std::unexpected(response.error());
    }

    ListPromptsResult result;
    auto doc = parse(*response);
    if (doc) {
        auto root = doc->root();
        auto result_node = root.get("result");
        if (result_node.is_obj()) {
            auto prompts_node = result_node.get("prompts");
            if (prompts_node.is_arr()) {
                prompts_node.iter([&result](JsonVal prompt_val) {
                    if (prompt_val.is_obj()) {
                        McpPrompt prompt;
                        prompt.name = std::string(prompt_val.get("name").as_str());
                        prompt.description = std::string(prompt_val.get("description").as_str());
                        // Parse arguments
                        auto args_node = prompt_val.get("arguments");
                        if (args_node.is_arr()) {
                            args_node.iter([&prompt](JsonVal arg_val) {
                                if (arg_val.is_obj()) {
                                    McpPromptArgument arg;
                                    arg.name = std::string(arg_val.get("name").as_str());
                                    arg.description = std::string(arg_val.get("description").as_str());
                                    arg.required = arg_val.get("required").as_bool();
                                    prompt.arguments.push_back(std::move(arg));
                                }
                            });
                        }
                        result.prompts.push_back(std::move(prompt));
                    }
                });
            }
        }
    }

    cached_prompts_ = result.prompts;
    return result;
}

} // namespace cc::services::mcp
