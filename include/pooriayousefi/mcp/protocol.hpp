// ============================================================================
//  protocol.hpp — MCP Protocol Helpers (C++20)
//  Project: mcpxx
//  Developed by: Pooria Yousefi
//  License: Apache 2.0
//
//  Build example (tests):
//    clang++ -std=c++20 -O3 -I include tests/protocol_tests.cpp -o bin/protocol_tests
// ============================================================================
#pragma once

#include "nlohmann/json.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace pooriayousefi::mcp
{
    using JSON = nlohmann::json;

    // ----------------------------------------------------------------
    //  Protocol constants
    // ----------------------------------------------------------------
    inline constexpr std::string_view PROTOCOL_VERSION = "2026-07-28";
    inline constexpr std::string_view JSONRPC_VERSION  = "2.0";

    inline constexpr std::string_view META_PROTOCOL_VERSION   = "io.modelcontextprotocol/protocolVersion";
    inline constexpr std::string_view META_CLIENT_INFO        = "io.modelcontextprotocol/clientInfo";
    inline constexpr std::string_view META_CLIENT_CAPABILITIES = "io.modelcontextprotocol/clientCapabilities";
    inline constexpr std::string_view META_SERVER_INFO        = "io.modelcontextprotocol/serverInfo";

    // ----------------------------------------------------------------
    //  Official JSON-RPC + MCP error codes
    // ----------------------------------------------------------------
    namespace error_code
    {
        inline constexpr int PARSE_ERROR                   = -32700;
        inline constexpr int INVALID_REQUEST               = -32600;
        inline constexpr int METHOD_NOT_FOUND              = -32601;
        inline constexpr int INVALID_PARAMS                = -32602;
        inline constexpr int INTERNAL_ERROR                = -32603;

        // MCP-specific (2026-07-28)
        inline constexpr int UNSUPPORTED_PROTOCOL_VERSION  = -32022;
    }

    // ----------------------------------------------------------------
    //  Error — rich, future-proof error type used throughout mcpxx
    // ----------------------------------------------------------------
    struct Error
    {
        int         code    = 0;
        std::string message{};
        JSON        data    = nullptr;   // optional extra payload from the server

        Error() = default;

        Error(int c, std::string msg)
            : code{c}
            , message{std::move(msg)}
        {
        }

        Error(int c, std::string msg, JSON d)
            : code{c}
            , message{std::move(msg)}
            , data{std::move(d)}
        {
        }

        // Convenience factories
        static Error from_json_rpc(const JSON& error_obj)
        {
            Error result{};

            if (error_obj.is_object())
            {
                if (error_obj.contains("code") && error_obj["code"].is_number_integer())
                {
                    result.code = error_obj["code"].get<int>();
                }
                if (error_obj.contains("message") && error_obj["message"].is_string())
                {
                    result.message = error_obj["message"].get<std::string>();
                }
                if (error_obj.contains("data"))
                {
                    result.data = error_obj["data"];
                }
            }
            else
            {
                result.code    = error_code::INTERNAL_ERROR;
                result.message = "Malformed error object received from peer";
            }

            return result;
        }

        static Error make(int c, std::string msg)
        {
            return Error{c, std::move(msg)};
        }

        static Error make(int c, std::string msg, JSON d)
        {
            return Error{c, std::move(msg), std::move(d)};
        }
    };

    // ----------------------------------------------------------------
    //  Client / Server identity
    // ----------------------------------------------------------------
    struct ClientInfo
    {
        std::string name{};
        std::string version{};
    };

    struct ServerInfo
    {
        std::string name{};
        std::string version{};
    };

    // ----------------------------------------------------------------
    //  Request metadata helpers
    // ----------------------------------------------------------------
    inline JSON make_request_meta(
        const ClientInfo& client,
        JSON capabilities = JSON::object()
    )
    {
        return {
            {std::string(META_PROTOCOL_VERSION), PROTOCOL_VERSION},
            {std::string(META_CLIENT_INFO), {
                {"name", client.name},
                {"version", client.version}
            }},
            {std::string(META_CLIENT_CAPABILITIES), std::move(capabilities)}
        };
    }

    // Creates a JSON-RPC request with MCP metadata.
    inline JSON make_request(
        const JSON& id,
        std::string method,
        JSON params,
        const ClientInfo& client,
        JSON capabilities = JSON::object()
    )
    {
        if (!params.is_object())
        {
            params = JSON::object();
        }
        params["_meta"] = make_request_meta(client, std::move(capabilities));

        return {
            {"jsonrpc", JSONRPC_VERSION},
            {"id", id},
            {"method", std::move(method)},
            {"params", std::move(params)}
        };
    }

    // Notifications have no ID and do not receive a response.
    inline JSON make_notification(
        std::string method,
        JSON params = JSON::object()
    )
    {
        if (!params.is_object())
        {
            params = JSON::object();
        }

        return {
            {"jsonrpc", JSONRPC_VERSION},
            {"method", std::move(method)},
            {"params", std::move(params)}
        };
    }

    inline JSON make_result(
        const JSON& id,
        JSON result
    )
    {
        return {
            {"jsonrpc", JSONRPC_VERSION},
            {"id", id},
            {"result", std::move(result)}
        };
    }

    inline JSON make_error(
        const JSON& id,
        int code,
        std::string message,
        JSON data = nullptr
    )
    {
        JSON error = {
            {"code", code},
            {"message", std::move(message)}
        };

        if (!data.is_null())
        {
            error["data"] = std::move(data);
        }

        return {
            {"jsonrpc", JSONRPC_VERSION},
            {"id", id},
            {"error", std::move(error)}
        };
    }

    // ----------------------------------------------------------------
    //  server/discover helpers (mandatory in 2026-07-28)
    // ----------------------------------------------------------------
    inline JSON make_discover_request(
        const JSON& id,
        const ClientInfo& client,
        JSON capabilities = JSON::object()
    )
    {
        return make_request(id, "server/discover", JSON::object(), client, std::move(capabilities));
    }

    // Typical successful discover result shape (for documentation / tests)
    inline JSON make_discover_result(
        const JSON& id,
        const ServerInfo& server,
        JSON supported_versions = JSON::array({std::string(PROTOCOL_VERSION)}),
        JSON capabilities = JSON::object()
    )
    {
        JSON result = {
            {"supportedVersions", std::move(supported_versions)},
            {"capabilities", std::move(capabilities)},
            {"serverInfo", {
                {"name", server.name},
                {"version", server.version}
            }}
        };

        return make_result(id, std::move(result));
    }

    // ----------------------------------------------------------------
    //  Validation helpers
    // ----------------------------------------------------------------
    inline bool is_valid_id(const JSON& id)
    {
        return id.is_string() || id.is_number_integer() || id.is_number_unsigned();
    }

    inline bool is_valid_request(const JSON& message)
    {
        bool result = false;

        if (!message.is_object())
        {
            result = false;
        }
        else if (
            !message.contains("jsonrpc") ||
            !message["jsonrpc"].is_string() ||
            message["jsonrpc"] != JSONRPC_VERSION
        )
        {
            result = false;
        }
        else if (
            !message.contains("method") ||
            !message["method"].is_string()
        )
        {
            result = false;
        }
        else if (
            message.contains("id") &&
            !is_valid_id(message["id"])
        )
        {
            result = false;
        }
        else if (
            message.contains("params") &&
            !message["params"].is_object() &&
            !message["params"].is_array()
        )
        {
            result = false;
        }
        else
        {
            // A request must have an ID. Without one, it is a notification.
            result = message.contains("id");
        }

        return result;
    }

    inline bool is_notification(const JSON& message)
    {
        return message.is_object()
            && message.value("jsonrpc", std::string{}) == JSONRPC_VERSION
            && message.contains("method")
            && message["method"].is_string()
            && !message.contains("id");
    }

    inline bool has_valid_mcp_meta(const JSON& message)
    {
        bool result = false;

        if (
            !message.is_object()
            || !message.contains("params")
            || !message["params"].is_object()
        )
        {
            result = false;
        }
        else
        {
            const auto& params = message["params"];

            if (
                !params.contains("_meta")
                || !params["_meta"].is_object()
            )
            {
                result = false;
            }
            else
            {
                const auto& meta = params["_meta"];

                result = meta.contains(std::string(META_PROTOCOL_VERSION))
                    && meta[std::string(META_PROTOCOL_VERSION)].is_string()
                    && meta[std::string(META_PROTOCOL_VERSION)] == PROTOCOL_VERSION
                    && meta.contains(std::string(META_CLIENT_CAPABILITIES))
                    && meta[std::string(META_CLIENT_CAPABILITIES)].is_object();
            }
        }

        return result;
    }
}