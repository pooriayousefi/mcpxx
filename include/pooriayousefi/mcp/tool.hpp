// ============================================================================
//  tool.hpp — MCP Tool definition (C++20)
//  Project: mcpxx
//  Developed by: Pooria Yousefi
//  License: Apache 2.0
// ============================================================================
#pragma once

#include "pooriayousefi/mcp/protocol.hpp"
#include "pooriayousefi/expected.hpp"

#include <functional>
#include <string>

namespace pooriayousefi::mcp
{
    using ToolHandler = std::function<Expected<JSON, Error>(const JSON& arguments)>;

    struct Tool
    {
        std::string name{};
        std::string description{};
        JSON        input_schema = JSON::object();
        ToolHandler handler{};

        Tool() = default;

        Tool(
            std::string tool_name,
            std::string tool_description,
            JSON schema,
            ToolHandler tool_handler
        )
            : name{std::move(tool_name)}
            , description{std::move(tool_description)}
            , input_schema{std::move(schema)}
            , handler{std::move(tool_handler)}
        {
        }
    };
}