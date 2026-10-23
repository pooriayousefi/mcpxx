// ============================================================================
//  server.hpp — MCP Server (stdio + Streamable HTTP) — C++20
//  Project: mcpxx
//  Developed by: Pooria Yousefi
//  License: Apache 2.0
//
//  Example usage:
//
//    mcp::Server server{"shell-mcp-server", "1.0.0"};
//
//    server.add_tool(
//        "run_command",
//        "Execute a shell command",
//        JSON::parse(R"({ "type": "object", "properties": { "command": { "type": "string" } }, "required": ["command"] })"),
//        [](const JSON& args) -> mcp::Expected<JSON, mcp::Error> {
//            // your logic
//            return JSON{{"output", "hello"}};
//        }
//    );
//
//    server.run_stdio();          // or
//    server.run_http("0.0.0.0", 8080);
// ============================================================================
#pragma once

#include "pooriayousefi/expected.hpp"
#include "pooriayousefi/mcp/protocol.hpp"
#include "pooriayousefi/mcp/tool.hpp"
#include "pooriayousefi/mcp/resource.hpp"
#include "pooriayousefi/mcp/prompt.hpp"

#include "cpp-httplib/httplib.h"

#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pooriayousefi::mcp
{
    class Server
    {
    public:
        Server(std::string name, std::string version)
            : server_info_{std::move(name), std::move(version)}
        {
        }

        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;

        // ----------------------------------------------------------------
        //  Registration
        // ----------------------------------------------------------------
        void add_tool(
            std::string name,
            std::string description,
            JSON input_schema,
            ToolHandler handler
        )
        {
            // Important: make a copy of the name for the map key
            // before moving it into the Tool object.
            std::string key = name;

            tools_.emplace(
                std::move(key),
                Tool{
                    std::move(name),
                    std::move(description),
                    std::move(input_schema),
                    std::move(handler)
                }
            );
        }

        // Placeholder registration APIs (for future expansion)
        void add_resource(Resource resource)
        {
            resources_.push_back(std::move(resource));
        }

        void add_prompt(Prompt prompt)
        {
            prompts_.push_back(std::move(prompt));
        }

        // ----------------------------------------------------------------
        //  Transports
        // ----------------------------------------------------------------
        void run_stdio()
        {
            // Simple synchronous stdio loop (newline-delimited)
            std::string line;
            while (std::getline(std::cin, line))
            {
                if (line.empty())
                {
                    continue;
                }

                try
                {
                    JSON message = JSON::parse(line);
                    JSON response = handle_message(message);

                    if (!response.is_null())
                    {
                        std::cout << response.dump() << std::endl;
                    }
                }
                catch (const JSON::parse_error& e)
                {
                    JSON error_response = make_error(
                        nullptr,
                        error_code::PARSE_ERROR,
                        std::string("Parse error: ") + e.what()
                    );
                    std::cout << error_response.dump() << std::endl;
                }
                catch (...)
                {
                    JSON error_response = make_error(
                        nullptr,
                        error_code::INTERNAL_ERROR,
                        "Internal server error"
                    );
                    std::cout << error_response.dump() << std::endl;
                }
            }
        }

        void run_http(std::string host, int port)
        {
            httplib::Server http;

            // Convenience OpenAI-style schema endpoint (as requested)
            http.Get("/schema", [this](const httplib::Request&, httplib::Response& res) {
                res.set_content(build_openai_style_schema().dump(2), "application/json");
            });

            // Main MCP endpoint (Streamable HTTP style — simplified for v1)
            http.Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
                try
                {
                    JSON message = JSON::parse(req.body);
                    JSON response = handle_message(message);

                    if (response.is_null())
                    {
                        res.status = 204;
                    }
                    else
                    {
                        res.set_content(response.dump(), "application/json");
                    }
                }
                catch (const JSON::parse_error& e)
                {
                    res.status = 400;
                    res.set_content(
                        make_error(nullptr, error_code::PARSE_ERROR, e.what()).dump(),
                        "application/json"
                    );
                }
                catch (...)
                {
                    res.status = 500;
                    res.set_content(
                        make_error(nullptr, error_code::INTERNAL_ERROR, "Internal error").dump(),
                        "application/json"
                    );
                }
            });

            // Also accept tool calls on a REST-like path for easier migration
            http.Post("/tools/call", [this](const httplib::Request& req, httplib::Response& res) {
                try
                {
                    JSON body = JSON::parse(req.body);
                    std::string tool_name = body.value("name", "");
                    JSON arguments = body.value("arguments", JSON::object());

                    auto call_result = call_tool(tool_name, arguments);

                    if (call_result.has_value())
                    {
                        res.set_content(
                            JSON{{"result", call_result.value()}, {"is_error", false}}.dump(),
                            "application/json"
                        );
                    }
                    else
                    {
                        res.set_content(
                            JSON{
                                {"error", call_result.error().message},
                                {"is_error", true},
                                {"code", call_result.error().code}
                            }.dump(),
                            "application/json"
                        );
                    }
                }
                catch (...)
                {
                    res.status = 500;
                    res.set_content(
                        JSON{{"error", "Internal error"}, {"is_error", true}}.dump(),
                        "application/json"
                    );
                }
            });

            std::cout << "MCP server \"" << server_info_.name
                      << "\" listening on http://" << host << ":" << port << std::endl;
            std::cout << "  - Official MCP endpoint : POST /mcp" << std::endl;
            std::cout << "  - Convenience schema    : GET  /schema" << std::endl;
            std::cout << "  - REST-style tool call  : POST /tools/call" << std::endl;

            http.listen(host, port);
        }

    private:
        ServerInfo server_info_;
        std::unordered_map<std::string, Tool> tools_;
        std::vector<Resource> resources_;
        std::vector<Prompt> prompts_;

        // ----------------------------------------------------------------
        //  Message dispatcher
        // ----------------------------------------------------------------
        JSON handle_message(const JSON& message)
        {
            JSON result = nullptr;

            if (!message.is_object())
            {
                result = make_error(nullptr, error_code::INVALID_REQUEST, "Message must be an object");
            }
            else if (is_notification(message))
            {
                // Notifications are ignored for now (no response)
                result = nullptr;
            }
            else if (!is_valid_request(message))
            {
                JSON id = message.contains("id") ? message["id"] : nullptr;
                result = make_error(id, error_code::INVALID_REQUEST, "Invalid request");
            }
            else
            {
                const JSON& id = message["id"];
                const std::string method = message["method"].get<std::string>();
                JSON params = message.value("params", JSON::object());

                if (method == "server/discover")
                {
                    result = handle_discover(id);
                }
                else if (method == "tools/list")
                {
                    result = handle_tools_list(id);
                }
                else if (method == "tools/call")
                {
                    result = handle_tools_call(id, params);
                }
                else
                {
                    result = make_error(id, error_code::METHOD_NOT_FOUND, "Method not found: " + method);
                }
            }

            return result;
        }

        // ----------------------------------------------------------------
        //  Method handlers
        // ----------------------------------------------------------------
        JSON handle_discover(const JSON& id)
        {
            JSON capabilities = JSON::object();

            if (!tools_.empty())
            {
                capabilities["tools"] = JSON::object();
            }
            if (!resources_.empty())
            {
                capabilities["resources"] = JSON::object();
            }
            if (!prompts_.empty())
            {
                capabilities["prompts"] = JSON::object();
            }

            JSON result_body = {
                {"supportedVersions", JSON::array({std::string(PROTOCOL_VERSION)})},
                {"capabilities", std::move(capabilities)},
                {"serverInfo", {
                    {"name", server_info_.name},
                    {"version", server_info_.version}
                }}
            };

            return make_result(id, std::move(result_body));
        }

        JSON handle_tools_list(const JSON& id)
        {
            JSON tools_array = JSON::array();

            for (const auto& [name, tool] : tools_)
            {
                // Defensive: some older builds accidentally stored the schema
                // as a one-element array. Unwrap it so we always emit a plain object.
                JSON schema = tool.input_schema;
                if (schema.is_array() && !schema.empty() && schema[0].is_object())
                {
                    schema = schema[0];
                }

                tools_array.push_back({
                    {"name", tool.name},
                    {"description", tool.description},
                    {"inputSchema", std::move(schema)}
                });
            }

            return make_result(id, {{"tools", std::move(tools_array)}});
        }

        JSON handle_tools_call(const JSON& id, const JSON& params)
        {
            JSON result = nullptr;

            if (!params.contains("name") || !params["name"].is_string())
            {
                result = make_error(id, error_code::INVALID_PARAMS, "Missing or invalid 'name'");
            }
            else
            {
                const std::string tool_name = params["name"].get<std::string>();
                JSON arguments = params.value("arguments", JSON::object());

                auto call_result = call_tool(tool_name, arguments);

                if (call_result.has_value())
                {
                    // MCP tools/call result format
                    result = make_result(id, {
                        {"content", JSON::array({
                            {
                                {"type", "text"},
                                {"text", call_result.value().dump()}
                            }
                        })},
                        {"isError", false}
                    });
                }
                else
                {
                    result = make_error(
                        id,
                        call_result.error().code,
                        call_result.error().message,
                        call_result.error().data
                    );
                }
            }

            return result;
        }

        Expected<JSON, Error> call_tool(const std::string& name, const JSON& arguments)
        {
            Expected<JSON, Error> result{};

            auto it = tools_.find(name);
            if (it == tools_.end())
            {
                result = Error{
                    error_code::METHOD_NOT_FOUND,
                    "Unknown tool: " + name
                };
            }
            else if (!it->second.handler)
            {
                result = Error{
                    error_code::INTERNAL_ERROR,
                    "Tool has no handler: " + name
                };
            }
            else
            {
                try
                {
                    result = it->second.handler(arguments);
                }
                catch (const std::exception& e)
                {
                    result = Error{
                        error_code::INTERNAL_ERROR,
                        std::string("Tool threw exception: ") + e.what()
                    };
                }
                catch (...)
                {
                    result = Error{
                        error_code::INTERNAL_ERROR,
                        "Tool threw unknown exception"
                    };
                }
            }

            return result;
        }

        // ----------------------------------------------------------------
        //  Convenience helpers
        // ----------------------------------------------------------------
        JSON build_openai_style_schema() const
        {
            JSON schema = JSON::array();

            for (const auto& [name, tool] : tools_)
            {
                JSON parameters = tool.input_schema;
                if (parameters.is_array() && !parameters.empty() && parameters[0].is_object())
                {
                    parameters = parameters[0];
                }

                schema.push_back({
                    {"type", "function"},
                    {"function", {
                        {"name", tool.name},
                        {"description", tool.description},
                        {"parameters", std::move(parameters)}
                    }}
                });
            }

            return schema;
        }
    };
}