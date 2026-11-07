// ============================================================================
//  pooriayousefi/mcp/client.hpp — MCP Client for stdio and Streamable HTTP
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Connect to an HTTP endpoint or launch a stdio server process, then list
//  its tools or call a tool. Operations report transport/protocol failures
//  through Expected<T, std::error_code>.
// ============================================================================

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <system_error>
#include <unordered_map>
#include "nlohmann/json.hpp"
#include "pooriayousefi/utility/common.hpp"
#include "pooriayousefi/utility/expected.hpp"
#include "pooriayousefi/mcp/protocol.hpp"
#include "pooriayousefi/mcp/transport.hpp"

namespace pooriayousefi::mcp
{
    class Client
    {
    public:
        Client(std::string_view host, int port)
            : http_client_{std::make_unique<HttpClientTransport>(host, port)}
        {
        }

        Client(
            std::string executable,
            std::vector<std::string> arguments,
            std::unordered_map<std::string, std::string> environment = {})
            : stdio_client_{std::make_unique<StdioClientTransport>()}
        {
            stdio_client_->start(executable, arguments, environment);
        }

        ~Client() = default;

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;
        Client(Client&&) noexcept = default;
        Client& operator=(Client&&) noexcept = default;

        [[nodiscard]] Expected<std::vector<ToolDefinition>, std::error_code>
        list_tools() noexcept
        {
            Expected<std::vector<ToolDefinition>, std::error_code> result{};

            JsonRpcMessage req{};
            req.method = "tools/list";
            req.id = 1;
            req.params = Json::object();
            req.params["_meta"] = make_request_metadata();

            auto resp_res = send_request(req.to_json().dump(), "tools/list", "");
            if (resp_res.has_error())
            {
                result.set_error(resp_res.get_error());
                return result;
            }

            auto json_parse = nlohmann::json::parse(resp_res.get_value(), nullptr, false);
            if (json_parse.is_discarded())
            {
                result.set_error(std::make_error_code(std::errc::protocol_error));
                return result;
            }

            JsonRpcMessage resp = JsonRpcMessage::from_json(json_parse);
            if (!resp.is_response() || resp.id != req.id)
            {
                result.set_error(std::make_error_code(std::errc::protocol_error));
                return result;
            }
            if (resp.error.has_value())
            {
                result.set_error(std::make_error_code(std::errc::protocol_error));
                return result;
            }
            if (!resp.result.is_object() ||
                !resp.result.contains("resultType") ||
                !resp.result["resultType"].is_string() ||
                resp.result["resultType"] != "complete" ||
                !resp.result.contains("tools") || !resp.result["tools"].is_array())
            {
                result.set_error(std::make_error_code(std::errc::protocol_error));
                return result;
            }

            std::vector<ToolDefinition> tools{};
            for (const auto& item : resp.result["tools"])
            {
                tools.push_back(ToolDefinition::from_json(item));
            }

            result.set_value(std::move(tools));
            return result;
        }

        [[nodiscard]] Expected<ToolCallResult, std::error_code>
        call_tool(std::string_view tool_name, const nlohmann::json& arguments) noexcept
        {
            Expected<ToolCallResult, std::error_code> result{};

            JsonRpcMessage req{};
            req.method = "tools/call";
            req.id = 2;
            nlohmann::json params{};
            params["name"] = std::string{tool_name};
            params["arguments"] = arguments;
            params["_meta"] = make_request_metadata();
            req.params = params;

            auto resp_res = send_request(req.to_json().dump(), "tools/call", tool_name);
            if (resp_res.has_error())
            {
                result.set_error(resp_res.get_error());
                return result;
            }

            auto json_parse = nlohmann::json::parse(resp_res.get_value(), nullptr, false);
            if (json_parse.is_discarded())
            {
                result.set_error(std::make_error_code(std::errc::protocol_error));
                return result;
            }

            JsonRpcMessage resp = JsonRpcMessage::from_json(json_parse);
            if (!resp.is_response() || resp.id != req.id)
            {
                result.set_error(std::make_error_code(std::errc::protocol_error));
                return result;
            }
            if (resp.error.has_value())
            {
                ToolCallResult err_res{};
                err_res.is_error = true;
                err_res.content = resp.error.value().message;
                result.set_value(err_res);
                return result;
            }

            if (!resp.result.is_object() ||
                !resp.result.contains("resultType") ||
                !resp.result["resultType"].is_string() ||
                resp.result["resultType"] != "complete" ||
                !resp.result.contains("content") || !resp.result["content"].is_array())
            {
                result.set_error(std::make_error_code(std::errc::protocol_error));
                return result;
            }

            ToolCallResult call_res = ToolCallResult::from_json(resp.result);
            result.set_value(std::move(call_res));
            return result;
        }

    private:
        std::unique_ptr<HttpClientTransport> http_client_{nullptr};
        std::unique_ptr<StdioClientTransport> stdio_client_{nullptr};

        [[nodiscard]] Expected<std::string, std::error_code> send_request(
            std::string_view payload,
            std::string_view method,
            std::string_view name) noexcept
        {
            if (stdio_client_)
            {
                return stdio_client_->send_receive(payload);
            }
            if (http_client_)
            {
                return http_client_->send_request(payload, method, name);
            }

            Expected<std::string, std::error_code> result{};
            result.set_error(std::make_error_code(std::errc::not_connected));
            return result;
        }
    };
}