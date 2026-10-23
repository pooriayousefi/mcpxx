// ============================================================================
//  resource.hpp — MCP Resource definition (placeholder for future work)
//  Project: mcpxx
//  Developed by: Pooria Yousefi
//  License: Apache 2.0
// ============================================================================
#pragma once

#include "pooriayousefi/mcp/protocol.hpp"

#include <functional>
#include <string>

namespace pooriayousefi::mcp
{
    // Placeholder — will be expanded later
    struct Resource
    {
        std::string uri{};
        std::string name{};
        std::string description{};
        std::string mime_type{};

        Resource() = default;
    };
}