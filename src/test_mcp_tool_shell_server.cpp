// ============================================================================
//  test_mcp_tool_shell_server.cpp — Comprehensive MCP Client Test
//  Project: mcpxx
//
//  Tests both stdio and HTTP transports against mcp_tool_shell_server.
//  Demonstrates the full protocol flow:
//    - server/discover
//    - tools/list
//    - tools/call (success + error cases)
//
//  Build:
//    clang++ -std=c++20 -O3 -I include src/test_mcp_tool_shell_server.cpp -o bin/test_mcp_tool_shell_server
//
//  Usage:
//    # stdio (launches the server automatically)
//    MCPXX_SHELL_ENABLED=1 ./bin/test_mcp_tool_shell_server stdio
//
//    # HTTP (server must already be running)
//    ./bin/test_mcp_tool_shell_server http http://127.0.0.1:8090
// ============================================================================

#include "pooriayousefi/mcp/client.hpp"
#include "pooriayousefi/mcp/protocol.hpp"
#include "pooriayousefi/expected.hpp"

#include "cpp-httplib/httplib.h"
#include "nlohmann/json.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>

using namespace pooriayousefi;
using namespace pooriayousefi::mcp;

// ------------------------------------------------------------------
//  Helpers
// ------------------------------------------------------------------
void print_separator(const std::string& title)
{
    std::cout << "\n============================================================\n";
    std::cout << "  " << title << "\n";
    std::cout << "============================================================\n";
}

void print_json(const JSON& j, const std::string& label = "")
{
    if (!label.empty())
    {
        std::cout << label << ":\n";
    }
    std::cout << j.dump(2) << "\n";
}

void print_error(const Error& err)
{
    std::cout << "[ERROR] code=" << err.code
              << "  message=" << err.message << "\n";
    if (!err.data.is_null())
    {
        std::cout << "  data=" << err.data.dump() << "\n";
    }
}

// ------------------------------------------------------------------
//  HTTP helper (simple, for testing only)
// ------------------------------------------------------------------
Expected<JSON, Error> http_request(
    const std::string& base_url,
    const std::string& method,
    const JSON& params = JSON::object()
)
{
    Expected<JSON, Error> result{};

    try
    {
        // Very simple URL parsing: expect "http://host:port"
        std::string host = "127.0.0.1";
        int port = 8090;

        if (base_url.starts_with("http://"))
        {
            std::string rest = base_url.substr(7);
            auto colon = rest.find(':');
            if (colon != std::string::npos)
            {
                host = rest.substr(0, colon);
                port = std::stoi(rest.substr(colon + 1));
            }
            else
            {
                host = rest;
            }
        }

        httplib::Client http(host, port);
        http.set_connection_timeout(5, 0);
        http.set_read_timeout(10, 0);

        JSON request = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", method},
            {"params", params}
        };

        // Add minimal _meta so the server accepts it
        request["params"]["_meta"] = {
            {"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
            {"io.modelcontextprotocol/clientInfo", {
                {"name", "mcp-shell-client-tests"},
                {"version", "0.1.0"}
            }},
            {"io.modelcontextprotocol/clientCapabilities", JSON::object()}
        };

        auto res = http.Post("/mcp", request.dump(), "application/json");

        if (!res)
        {
            result = Error{
                error_code::INTERNAL_ERROR,
                "HTTP request failed (connection error)"
            };
        }
        else if (res->status != 200 && res->status != 204)
        {
            result = Error{
                error_code::INTERNAL_ERROR,
                "HTTP status " + std::to_string(res->status) + ": " + res->body
            };
        }
        else if (res->status == 204)
        {
            result = JSON::object(); // notification / no content
        }
        else
        {
            JSON body = JSON::parse(res->body);

            if (body.contains("error"))
            {
                result = Error::from_json_rpc(body["error"]);
            }
            else if (body.contains("result"))
            {
                result = std::move(body["result"]);
            }
            else
            {
                result = Error{
                    error_code::INVALID_REQUEST,
                    "Response contains neither result nor error"
                };
            }
        }
    }
    catch (const std::exception& e)
    {
        result = Error{
            error_code::INTERNAL_ERROR,
            std::string("HTTP helper exception: ") + e.what()
        };
    }

    return result;
}

// ------------------------------------------------------------------
//  Full protocol test sequence
// ------------------------------------------------------------------
void run_protocol_test_stdio()
{
    print_separator("STDIO TRANSPORT TEST");

    Client client;
    client.set_client_info(ClientInfo{"mcp-shell-client-tests", "0.1.0"});

    std::cout << "Launching mcp_tool_shell_server via stdio...\n";

    // Adjust the path if your binary is elsewhere
    bool started = client.start("./bin/mcp_tool_shell_server");
    if (!started)
    {
        std::cout << "Failed to start server process.\n";
        return;
    }
    std::cout << "Server process started.\n";

    // 1. server/discover
    print_separator("1. server/discover");
    auto discover = client.discover();
    if (discover.has_value())
    {
        print_json(discover.value(), "Discover result");
    }
    else
    {
        print_error(discover.error());
    }

    // 2. tools/list
    print_separator("2. tools/list");
    auto tools = client.request("tools/list");
    if (tools.has_value())
    {
        print_json(tools.value(), "Tools list");
    }
    else
    {
        print_error(tools.error());
    }

    // 3. tools/call — success case (run_command)
    print_separator("3. tools/call  (run_command — success)");
    auto call1 = client.request(
        "tools/call",
        {
            {"name", "run_command"},
            {"arguments", {
                {"command", "echo Hello from MCP stdio && date"}
            }}
        }
    );
    if (call1.has_value())
    {
        print_json(call1.value(), "Call result");
    }
    else
    {
        print_error(call1.error());
    }

    // 4. tools/call — success case (run_program)
    print_separator("4. tools/call  (run_program — success)");
    auto call2 = client.request(
        "tools/call",
        {
            {"name", "run_program"},
            {"arguments", {
                {"program", "echo"},
                {"args", JSON::array({"Hello", "from", "run_program"})}
            }}
        }
    );
    if (call2.has_value())
    {
        print_json(call2.value(), "Call result");
    }
    else
    {
        print_error(call2.error());
    }

    // 5. tools/call — denylisted command (should fail)
    print_separator("5. tools/call  (run_command — denylisted, should fail)");
    auto call3 = client.request(
        "tools/call",
        {
            {"name", "run_command"},
            {"arguments", {
                {"command", "rm -rf /"}
            }}
        }
    );
    if (call3.has_value())
    {
        print_json(call3.value(), "Unexpected success");
    }
    else
    {
        print_error(call3.error());
    }

    // 6. tools/call — unknown tool (should fail)
    print_separator("6. tools/call  (unknown tool, should fail)");
    auto call4 = client.request(
        "tools/call",
        {
            {"name", "non_existent_tool"},
            {"arguments", JSON::object()}
        }
    );
    if (call4.has_value())
    {
        print_json(call4.value(), "Unexpected success");
    }
    else
    {
        print_error(call4.error());
    }

    client.close();
    std::cout << "\nstdio test finished.\n";
}

void run_protocol_test_http(const std::string& base_url)
{
    print_separator("HTTP TRANSPORT TEST  (" + base_url + ")");

    // 1. server/discover
    print_separator("1. server/discover");
    auto discover = http_request(base_url, "server/discover");
    if (discover.has_value())
    {
        print_json(discover.value(), "Discover result");
    }
    else
    {
        print_error(discover.error());
    }

    // 2. tools/list
    print_separator("2. tools/list");
    auto tools = http_request(base_url, "tools/list");
    if (tools.has_value())
    {
        print_json(tools.value(), "Tools list");
    }
    else
    {
        print_error(tools.error());
    }

    // 3. tools/call — success
    print_separator("3. tools/call  (run_command — success)");
    auto call1 = http_request(
        base_url,
        "tools/call",
        {
            {"name", "run_command"},
            {"arguments", {
                {"command", "echo Hello from MCP HTTP && uname -a"}
            }}
        }
    );
    if (call1.has_value())
    {
        print_json(call1.value(), "Call result");
    }
    else
    {
        print_error(call1.error());
    }

    // 4. tools/call — denylisted
    print_separator("4. tools/call  (denylisted command, should fail)");
    auto call2 = http_request(
        base_url,
        "tools/call",
        {
            {"name", "run_command"},
            {"arguments", {
                {"command", "rm -rf /"}
            }}
        }
    );
    if (call2.has_value())
    {
        print_json(call2.value(), "Unexpected success");
    }
    else
    {
        print_error(call2.error());
    }

    // 5. Convenience REST-style endpoint
    print_separator("5. Convenience /tools/call (OpenAI-style)");
    {
        httplib::Client http("127.0.0.1", 8090); // adjust if needed
        JSON body = {
            {"name", "run_command"},
            {"arguments", {
                {"command", "echo REST-style call works"}
            }}
        };
        auto res = http.Post("/tools/call", body.dump(), "application/json");
        if (res && res->status == 200)
        {
            std::cout << res->body << "\n";
        }
        else
        {
            std::cout << "REST-style call failed\n";
        }
    }

    std::cout << "\nHTTP test finished.\n";
}

// ------------------------------------------------------------------
//  main
// ------------------------------------------------------------------
int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cout << "Usage:\n"
                  << "  " << argv[0] << " stdio\n"
                  << "  " << argv[0] << " http <base_url>\n\n"
                  << "Examples:\n"
                  << "  MCPXX_SHELL_ENABLED=1 " << argv[0] << " stdio\n"
                  << "  " << argv[0] << " http http://127.0.0.1:8090\n";
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "stdio")
    {
        run_protocol_test_stdio();
    }
    else if (mode == "http")
    {
        std::string url = "http://127.0.0.1:8090";
        if (argc >= 3)
        {
            url = argv[2];
        }
        run_protocol_test_http(url);
    }
    else
    {
        std::cout << "Unknown mode: " << mode << "\n";
        return 1;
    }

    return 0;
}