// ============================================================================
//  prompt.hpp — MCP Prompt definition (placeholder for future work)
//  Project: mcpxx
//  Developed by: Pooria Yousefi
//  License: Apache 2.0
// ============================================================================
#pragma once

#include "pooriayousefi/mcp/protocol.hpp"

#include <functional>
#include <string>
#include <vector>

namespace pooriayousefi::mcp
{
    // Placeholder — will be expanded later
    struct Prompt
    {
        std::string name{};
        std::string description{};
        JSON        arguments_schema = JSON::object();

        Prompt() = default;
    };
}