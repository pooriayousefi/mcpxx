// ============================================================================
//  pooriayousefi/mcp/protocol.hpp — JSON-RPC and MCP Protocol Types
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Declares the message, tool, and result data types shared by the client,
//  server, and transports. The advertised protocol version is defined here.
// ============================================================================

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include "pooriayousefi/utility/common.hpp"
#include "nlohmann/json.hpp"

namespace pooriayousefi::mcp
{
    inline constexpr std::string_view MCP_PROTOCOL_VERSION{"2026-07-28"};

    enum class ErrorCode
    {
        PARSE_ERROR      = -32700,
        INVALID_REQUEST  = -32600,
        METHOD_NOT_FOUND = -32601,
        INVALID_PARAMS   = -32602,
        INTERNAL_ERROR   = -32603,
        HEADER_MISMATCH  = -32020,
        UNSUPPORTED_PROTOCOL_VERSION = -32022
    };

    inline constexpr std::string_view MCP_PROTOCOL_VERSION_META{"io.modelcontextprotocol/protocolVersion"};
    inline constexpr std::string_view MCP_CLIENT_INFO_META{"io.modelcontextprotocol/clientInfo"};
    inline constexpr std::string_view MCP_CLIENT_CAPABILITIES_META{"io.modelcontextprotocol/clientCapabilities"};

    [[nodiscard]] inline Json make_request_metadata()
    {
        return Json{
            {std::string{MCP_PROTOCOL_VERSION_META}, std::string{MCP_PROTOCOL_VERSION}},
            {std::string{MCP_CLIENT_INFO_META}, {{"name", "sauron"}, {"version", "1.0.0"}}},
            {std::string{MCP_CLIENT_CAPABILITIES_META}, Json::object()}
        };
    }

    [[nodiscard]] inline bool is_valid_request_id(const Json& id)
    {
        return id.is_string() || id.is_number_integer();
    }

    struct JsonRpcError
    {
        int code{0};
        std::string message{};
        std::optional<Json> data{};

        [[nodiscard]] Json to_json() const
        {
            Json j{};
            j["code"] = code;
            j["message"] = message;
            if (data.has_value())
            {
                j["data"] = data.value();
            }
            return j;
        }

        static JsonRpcError from_json(const Json& j)
        {
            JsonRpcError err{};
            if (j.contains("code") && j["code"].is_number())
            {
                err.code = j["code"].get<int>();
            }
            if (j.contains("message") && j["message"].is_string())
            {
                err.message = j["message"].get<std::string>();
            }
            if (j.contains("data"))
            {
                err.data = j["data"];
            }
            return err;
        }
    };

    struct JsonRpcMessage
    {
        std::string jsonrpc{"2.0"};
        std::string method{};
        Json params = Json(Json::value_t::discarded);
        Json id = Json(Json::value_t::discarded);
        Json result = Json(Json::value_t::discarded);
        std::optional<JsonRpcError> error{std::nullopt};

        [[nodiscard]] bool is_request() const
        {
            return jsonrpc == "2.0" && !method.empty() &&
                   !id.is_discarded() && is_valid_request_id(id);
        }

        [[nodiscard]] bool is_notification() const
        {
            return jsonrpc == "2.0" && !method.empty() && id.is_discarded();
        }

        [[nodiscard]] bool is_response() const
        {
            const bool has_result = !result.is_discarded();
            return jsonrpc == "2.0" && method.empty() &&
                   is_valid_request_id(id) && (has_result != error.has_value());
        }

        [[nodiscard]] Json to_json() const
        {
            Json j{};
            j["jsonrpc"] = jsonrpc;
            if (!method.empty())
            {
                j["method"] = method;
            }
            if (!params.is_discarded())
            {
                j["params"] = params;
            }
            if (!id.is_discarded())
            {
                j["id"] = id;
            }
            if (!result.is_discarded())
            {
                j["result"] = result;
            }
            if (error.has_value())
            {
                j["error"] = error.value().to_json();
            }
            return j;
        }

        static JsonRpcMessage from_json(const Json& j)
        {
            JsonRpcMessage msg{};
            if (j.contains("jsonrpc") && j["jsonrpc"].is_string())
            {
                msg.jsonrpc = j["jsonrpc"].get<std::string>();
            }
            if (j.contains("method") && j["method"].is_string())
            {
                msg.method = j["method"].get<std::string>();
            }
            if (j.contains("params"))
            {
                msg.params = j["params"];
            }
            if (j.contains("id"))
            {
                msg.id = j["id"];
            }
            if (j.contains("result"))
            {
                msg.result = j["result"];
            }
            if (j.contains("error") && j["error"].is_object())
            {
                msg.error = JsonRpcError::from_json(j["error"]);
            }
            return msg;
        }
    };

    struct ToolDefinition
    {
        std::string name{};
        std::string description{};
        Json input_schema{Json::object()};

        [[nodiscard]] Json to_json() const
        {
            Json j{};
            j["name"] = name;
            j["description"] = description;
            j["inputSchema"] = input_schema;
            return j;
        }

        static ToolDefinition from_json(const Json& j)
        {
            ToolDefinition tool{};
            if (j.contains("name") && j["name"].is_string())
            {
                tool.name = j["name"].get<std::string>();
            }
            if (j.contains("description") && j["description"].is_string())
            {
                tool.description = j["description"].get<std::string>();
            }
            if (j.contains("inputSchema") && j["inputSchema"].is_object())
            {
                tool.input_schema = j["inputSchema"];
            }
            return tool;
        }
    };

    struct ToolCallResult
    {
        std::string content{};
        bool is_error{false};

        [[nodiscard]] Json to_json() const
        {
            Json j{};
            j["resultType"] = "complete";
            Json content_arr = Json::array();
            Json content_item = Json::object();
            content_item["type"] = "text";
            content_item["text"] = content;
            content_arr.push_back(content_item);
            j["content"] = content_arr;
            j["isError"] = is_error;
            return j;
        }

        static ToolCallResult from_json(const Json& j)
        {
            ToolCallResult res{};
            if (j.contains("isError") && j["isError"].is_boolean())
            {
                res.is_error = j["isError"].get<bool>();
            }
            if (j.contains("content") && j["content"].is_array() && !j["content"].empty())
            {
                const auto& item = j["content"][0];
                if (item.contains("text") && item["text"].is_string())
                {
                    res.content = item["text"].get<std::string>();
                }
            }
            return res;
        }
    };
} // namespace pooriayousefi::mcp