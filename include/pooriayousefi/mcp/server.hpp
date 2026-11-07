// ============================================================================
//  pooriayousefi/mcp/server.hpp — MCP Tool Server
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Register tool definitions and handlers, then serve MCP requests over
//  stdio, Streamable HTTP, or both using Server::run_cli().
// ============================================================================

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <iostream>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <thread>
#include "nlohmann/json.hpp"
#include "pooriayousefi/utility/common.hpp"
#include "pooriayousefi/mcp/protocol.hpp"
#include "pooriayousefi/mcp/transport.hpp"

namespace pooriayousefi::mcp
{
    using pooriayousefi::Json;

    enum class error_code
    {
        INVALID_PARAMS = static_cast<int>(ErrorCode::INVALID_PARAMS),
        INTERNAL_ERROR = static_cast<int>(ErrorCode::INTERNAL_ERROR)
    };

    struct Error
    {
        error_code code{error_code::INTERNAL_ERROR};
        std::string message{};
    };

    using ToolHandler = std::function<Expected<Json, Error>(const Json&)>;

    struct Tool
    {
        std::string name{};
        std::string title{};
        std::string description{};
        Json input_schema{Json::object()};
        ToolHandler handler{};
    };

    class Server
    {
    public:
        using LegacyToolHandler = std::function<ToolCallResult(const nlohmann::json& arguments)>;

        Server() = default;
        Server(std::string name, std::string version)
            : name_{std::move(name)}, version_{std::move(version)}
        {
        }
        ~Server() = default;

        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;
        Server(Server&&) noexcept = delete;
        Server& operator=(Server&&) noexcept = delete;

        void register_tool(const ToolDefinition& definition, LegacyToolHandler handler)
        {
            std::unique_lock<std::shared_mutex> lock{mutex_};
            tools_[definition.name] = definition;
            handlers_[definition.name] = std::move(handler);
        }

        void add_tool(Tool tool)
        {
            ToolDefinition definition{};
            definition.name = tool.name;
            definition.description = tool.description;
            definition.input_schema = std::move(tool.input_schema);
            register_tool(definition, [handler = std::move(tool.handler)](const Json& arguments)
            {
                ToolCallResult result{};
                if (!handler)
                {
                    result.is_error = true;
                    result.content = "Tool handler is not configured.";
                    return result;
                }
                auto outcome = handler(arguments);
                if (outcome.has_error())
                {
                    result.is_error = true;
                    result.content = outcome.get_error().message;
                }
                else if (outcome.has_value())
                {
                    result.content = outcome.get_value().dump();
                }
                else
                {
                    result.is_error = true;
                    result.content = "Tool handler returned no result.";
                }
                return result;
            });
        }

        void set_instructions(std::string instructions)
        {
            instructions_ = std::move(instructions);
        }

        [[nodiscard]] std::string handle_message(std::string_view raw_request)
        {
            auto parse_result = nlohmann::json::parse(raw_request, nullptr, false);
            if (parse_result.is_discarded())
            {
                return make_error_response(ErrorCode::PARSE_ERROR, "Parse error: invalid JSON");
            }

            if (!parse_result.is_object() || !parse_result.contains("jsonrpc") ||
                !parse_result["jsonrpc"].is_string() || parse_result["jsonrpc"] != "2.0" ||
                !parse_result.contains("method") || !parse_result["method"].is_string() ||
                parse_result["method"].get_ref<const std::string&>().empty())
            {
                return make_error_response(ErrorCode::INVALID_REQUEST, "Invalid JSON-RPC request");
            }

            const bool has_id = parse_result.contains("id");
            const Json id = has_id ? parse_result["id"] : Json(Json::value_t::discarded);
            if (has_id && !is_valid_request_id(id))
            {
                return make_error_response(ErrorCode::INVALID_REQUEST, "Request ID must be a string or integer");
            }

            Json params = parse_result.value("params", Json::object());
            if (!params.is_object())
            {
                return has_id
                    ? make_error_response(ErrorCode::INVALID_PARAMS, "Request params must be an object", id)
                    : std::string{};
            }

            if (has_id)
            {
                const Json meta = params.value("_meta", Json::object());
                if (!meta.is_object() ||
                    !meta.contains(std::string{MCP_PROTOCOL_VERSION_META}) ||
                    !meta[std::string{MCP_PROTOCOL_VERSION_META}].is_string() ||
                    !meta.contains(std::string{MCP_CLIENT_CAPABILITIES_META}) ||
                    !meta[std::string{MCP_CLIENT_CAPABILITIES_META}].is_object())
                {
                    return make_error_response(ErrorCode::INVALID_REQUEST,
                                               "Request _meta is missing required MCP metadata", id);
                }
                if (meta[std::string{MCP_PROTOCOL_VERSION_META}] != MCP_PROTOCOL_VERSION)
                {
                    return make_error_response(
                        ErrorCode::UNSUPPORTED_PROTOCOL_VERSION,
                        "Unsupported protocol version",
                        id,
                        Json{{"supportedVersions", Json::array({MCP_PROTOCOL_VERSION})}});
                }
            }
            else
            {
                return {};
            }

            const std::string& method = parse_result["method"].get_ref<const std::string&>();
            Json result{};
            if (method == "tools/list")
            {
                result = handle_tools_list();
            }
            else if (method == "tools/call")
            {
                if (!params.contains("name") || !params["name"].is_string() ||
                    (params.contains("arguments") && !params["arguments"].is_object()))
                {
                    return make_error_response(ErrorCode::INVALID_PARAMS,
                                               "tools/call requires a name and object arguments", id);
                }
                result = handle_tools_call(params);
            }
            else if (method == "ping")
            {
                result = nlohmann::json{{"resultType", "complete"}};
            }
            else
            {
                return make_error_response(ErrorCode::METHOD_NOT_FOUND, "Method not found: " + method, id);
            }

            Json response{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
            return response.dump();
        }

        bool start_http(std::string_view host, int port)
        {
            http_transport_.set_handler([this](std::string_view body)
            {
                return handle_message(body);
            });

            return http_transport_.listen(host, port);
        }

        int run_stdio()
        {
            std::string line{};
            while (std::getline(std::cin, line))
            {
                const std::string response = handle_message(line);
                if (!response.empty())
                {
                    std::cout << response << '\n' << std::flush;
                }
            }
            return 0;
        }

        int run_cli(int argc, char* argv[], int default_port)
        {
            std::string mode = argc > 1 ? argv[1] : "stdio";
            if (mode == "--http")
            {
                mode = "http";
            }
            if (mode == "stdio")
            {
                return run_stdio();
            }

            std::string host{"127.0.0.1"};
            int port = default_port;
            int port_index = 2;
            if (argc > 2 && mode != "http" && mode != "both")
            {
                host = mode;
                port_index = 2;
            }
            else if (argc > 2 && (mode == "http" || mode == "both"))
            {
                host = argv[2];
                port_index = 3;
            }
            if (argc > port_index)
            {
                try
                {
                    port = std::stoi(argv[port_index]);
                }
                catch (const std::exception&)
                {
                    std::cerr << "Invalid HTTP port: " << argv[port_index] << '\n';
                    return 2;
                }
            }
            if (port < 1 || port > 65535)
            {
                std::cerr << "HTTP port must be between 1 and 65535.\n";
                return 2;
            }

            if (mode == "http")
            {
                return start_http(host, port) ? 0 : 1;
            }
            if (mode == "both")
            {
                http_transport_.set_handler([this](std::string_view body)
                {
                    return handle_message(body);
                });
                if (!http_transport_.bind(host, port))
                {
                    std::cerr << "Failed to bind MCP HTTP transport on "
                              << host << ":" << port << '\n';
                    return 1;
                }
                int http_exit_code{0};
                std::thread http_thread([this, &http_exit_code]
                {
                    if (!http_transport_.listen_after_bind())
                    {
                        http_exit_code = 1;
                    }
                });
                const int stdio_exit_code = run_stdio();
                stop_http();
                http_thread.join();
                return http_exit_code != 0 ? http_exit_code : stdio_exit_code;
            }
            std::cerr << "Usage: " << argv[0] << " [stdio|http [host] [port]|both [host] [port]]\n";
            return 2;
        }

        void stop_http()
        {
            http_transport_.stop();
        }

    private:
        mutable std::shared_mutex mutex_{};
        std::unordered_map<std::string, ToolDefinition> tools_{};
        std::unordered_map<std::string, LegacyToolHandler> handlers_{};
        HttpServerTransport http_transport_{};
        std::string name_{};
        std::string version_{};
        std::string instructions_{};

        [[nodiscard]] nlohmann::json handle_tools_list()
        {
            std::shared_lock<std::shared_mutex> lock{mutex_};
            nlohmann::json j{{"resultType", "complete"}};
            nlohmann::json tools_arr = nlohmann::json::array();
            std::vector<std::string> tool_names{};
            tool_names.reserve(tools_.size());
            for (const auto& pair : tools_)
            {
                tool_names.push_back(pair.first);
            }
            std::sort(tool_names.begin(), tool_names.end());
            for (const auto& name : tool_names)
            {
                tools_arr.push_back(tools_.at(name).to_json());
            }
            j["tools"] = tools_arr;
            return j;
        }

        [[nodiscard]] nlohmann::json handle_tools_call(const nlohmann::json& params)
        {
            std::string tool_name{};
            nlohmann::json args = nlohmann::json::object();

            if (params.contains("name") && params["name"].is_string())
            {
                tool_name = params["name"].get<std::string>();
            }
            if (params.contains("arguments"))
            {
                args = params["arguments"];
            }

            LegacyToolHandler handler{nullptr};
            {
                std::shared_lock<std::shared_mutex> lock{mutex_};
                auto it = handlers_.find(tool_name);
                if (it != handlers_.end())
                {
                    handler = it->second;
                }
            }

            ToolCallResult call_res{};
            if (handler)
            {
                try
                {
                    call_res = handler(args);
                }
                catch (const std::exception& e)
                {
                    call_res.is_error = true;
                    call_res.content = std::string("Exception in tool execution: ") + e.what();
                }
                catch (...)
                {
                    call_res.is_error = true;
                    call_res.content = "Unknown exception in tool execution";
                }
            }
            else
            {
                call_res.is_error = true;
                call_res.content = "Tool not found: " + tool_name;
            }

            return call_res.to_json();
        }

        [[nodiscard]] static std::string make_error_response(
            ErrorCode code,
            std::string message,
            std::optional<Json> id = std::nullopt,
            std::optional<Json> data = std::nullopt)
        {
            Json error{{"code", static_cast<int>(code)}, {"message", std::move(message)}};
            if (data)
            {
                error["data"] = *data;
            }
            Json response{{"jsonrpc", "2.0"}, {"error", std::move(error)}};
            if (id && is_valid_request_id(*id))
            {
                response["id"] = *id;
            }
            return response.dump();
        }
    };
}