// ============================================================================
//  pooriayousefi/mcp/openai_bridge.hpp — MCP/OpenAI Tool JSON Helpers
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Convert MCP tool definitions to OpenAI function-tool JSON and construct
//  OpenAI-compatible chat-completion response objects.
// ============================================================================

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "nlohmann/json.hpp"
#include "pooriayousefi/utility/common.hpp"
#include "pooriayousefi/mcp/protocol.hpp"

namespace pooriayousefi::mcp
{
    class OpenAIBridge
    {
    public:
        OpenAIBridge() = default;
        ~OpenAIBridge() = default;

        OpenAIBridge(const OpenAIBridge&) = delete;
        OpenAIBridge& operator=(const OpenAIBridge&) = delete;
        OpenAIBridge(OpenAIBridge&&) noexcept = default;
        OpenAIBridge& operator=(OpenAIBridge&&) noexcept = default;

        [[nodiscard]] static Json translate_tools_to_openai(const std::vector<ToolDefinition>& mcp_tools)
        {
            Json openai_tools = Json::array();
            for (const auto& tool : mcp_tools)
            {
                Json item = Json::object();
                item["type"] = "function";

                Json function_obj = Json::object();
                function_obj["name"] = tool.name;
                function_obj["description"] = tool.description;
                function_obj["parameters"] = tool.input_schema;

                item["function"] = function_obj;
                openai_tools.push_back(item);
            }
            return openai_tools;
        }

        [[nodiscard]] static Json create_openai_chat_response(
            std::string_view model,
            std::string_view content,
            const std::string& tool_name = "",
            const Json& tool_arguments = Json::object(),
            std::string_view call_id = "call_mcp_001")
        {
            Json response = Json::object();
            response["id"] = "chatcmpl-mcpxx-" + std::string(model);
            response["object"] = "chat.completion";
            response["created"] = 1753690000;
            response["model"] = std::string(model);

            Json choices = Json::array();
            Json choice = Json::object();
            choice["index"] = 0;

            Json message = Json::object();
            message["role"] = "assistant";

            if (!tool_name.empty())
            {
                message["content"] = nullptr;
                Json tool_calls = Json::array();
                Json tool_call = Json::object();
                tool_call["id"] = std::string(call_id);
                tool_call["type"] = "function";

                Json function_call = Json::object();
                function_call["name"] = tool_name;
                function_call["arguments"] = tool_arguments.dump();

                tool_call["function"] = function_call;
                tool_calls.push_back(tool_call);
                message["tool_calls"] = tool_calls;
                choice["finish_reason"] = "tool_calls";
            }
            else
            {
                message["content"] = std::string(content);
                choice["finish_reason"] = "stop";
            }

            choice["message"] = message;
            choices.push_back(choice);
            response["choices"] = choices;

            return response;
        }
    };
} // namespace pooriayousefi::mcp