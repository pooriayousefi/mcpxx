// ============================================================================
//  pooriayousefi/utility/common.hpp — Shared JSON and String Aliases
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Defines common namespace-level aliases used by the MCP and process APIs.
// ============================================================================

#pragma once

#include <string>
#include "nlohmann/json.hpp"

namespace pooriayousefi
{
    using Json = nlohmann::json;
    using Url = std::string;
    using Id = std::string;
}