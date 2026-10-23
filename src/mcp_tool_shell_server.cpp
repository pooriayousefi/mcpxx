// ============================================================================
//  mcp_tool_shell_server.cpp — MCP Tool Server for controlled shell / program execution
//  Project: mcpxx
//
//  Tools:
//    run_command  — executes arbitrary shell commands via popen (supports pipes, redirects, &&)
//    run_program  — executes a program directly via process.hpp (no shell, injection-safe)
//
//  Safety:
//    - Denylist of catastrophic patterns
//    - Must set environment variable MCPXX_SHELL_ENABLED=1 to enable execution
//
//  Transports:
//    stdio  (default)  or  HTTP  (Streamable-style + convenience endpoints)
//
//  Build:
//
//    macOS / Linux:
//      clang++ -std=c++20 -O3 -I include examples/mcp_tool_shell_server.cpp -o bin/mcp_tool_shell_server
//
//    Linux (gcc):
//      g++ -std=c++20 -O3 -I include examples/mcp_tool_shell_server.cpp -o bin/mcp_tool_shell_server -lpthread
//
//    Windows (MinGW / clang):
//      clang++ -std=c++20 -O3 -I include examples/mcp_tool_shell_server.cpp -o bin/mcp_tool_shell_server.exe -lws2_32
//
//  Usage:
//      ./bin/mcp_tool_shell_server                  # stdio mode (default)
//      ./bin/mcp_tool_shell_server http 127.0.0.1 8090
// ============================================================================

#include "pooriayousefi/mcp/server.hpp"
#include "pooriayousefi/mcp/protocol.hpp"
#include "pooriayousefi/expected.hpp"
#include "pooriayousefi/process.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
    #define POPEN  _popen
    #define PCLOSE _pclose
#else
    #define POPEN  popen
    #define PCLOSE pclose
#endif

namespace
{
    using namespace pooriayousefi;
    using namespace pooriayousefi::mcp;

    constexpr std::size_t MAX_OUTPUT_LENGTH = 8000;
    constexpr const char* ENV_SHELL_ENABLED = "MCPXX_SHELL_ENABLED";

    // ------------------------------------------------------------------
    //  Helpers
    // ------------------------------------------------------------------
    bool shell_execution_enabled()
    {
        bool result = false;
        const char* flag = std::getenv(ENV_SHELL_ENABLED);

        if (flag != nullptr && std::string(flag) == "1")
        {
            result = true;
        }

        return result;
    }

    std::string to_lower(std::string s)
    {
        std::transform(
            s.begin(),
            s.end(),
            s.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            }
        );
        return s;
    }

    bool is_denylisted(const std::string& command)
    {
        constexpr std::array<std::string_view, 10> DENYLIST = {
            "rm -rf /",
            "rm -rf --no-preserve-root",
            "mkfs",
            "dd if=/dev/zero",
            "dd of=/dev/sd",
            ":(){ :|:& };:",
            "shutdown",
            "reboot",
            "halt",
            "init 0"
        };

        bool result = false;
        const std::string lowered = to_lower(command);

        for (const auto& pattern : DENYLIST)
        {
            if (lowered.find(pattern) != std::string::npos)
            {
                result = true;
                break;
            }
        }

        return result;
    }

    std::string escape_shell_arg(const std::string& arg)
    {
        std::string result;

#ifdef _WIN32
        result = "\"";
        for (char c : arg)
        {
            if (c == '"')
            {
                result += "\\\"";
            }
            else
            {
                result += c;
            }
        }
        result += "\"";
#else
        result = "'";
        for (char c : arg)
        {
            if (c == '\'')
            {
                result += "'\\''";
            }
            else
            {
                result += c;
            }
        }
        result += "'";
#endif

        return result;
    }

    // ------------------------------------------------------------------
    //  Tool: run_command
    // ------------------------------------------------------------------
    Expected<JSON, Error> tool_run_command(const JSON& args)
    {
        Expected<JSON, Error> result{};

        if (!shell_execution_enabled())
        {
            result = Error{
                error_code::INTERNAL_ERROR,
                "Shell execution is disabled. Set MCPXX_SHELL_ENABLED=1 to enable it "
                "(only in a sandboxed environment)."
            };
        }
        else if (!args.contains("command") || !args["command"].is_string())
        {
            result = Error{
                error_code::INVALID_PARAMS,
                "Missing or non-string 'command' argument"
            };
        }
        else
        {
            const std::string command = args["command"].get<std::string>();

            if (is_denylisted(command))
            {
                result = Error{
                    error_code::INVALID_PARAMS,
                    "Command rejected: matches a denylisted catastrophic pattern"
                };
            }
            else
            {
                const std::string input_str = args.value("input", std::string{});

                std::string safe_cmd;
                if (!input_str.empty())
                {
                    safe_cmd = "printf '%s' " + escape_shell_arg(input_str) + " | " + command + " 2>&1";
                }
                else
                {
                    safe_cmd = command + " 2>&1";
                }

                std::array<char, 4096> buffer{};
                std::unique_ptr<FILE, decltype(&PCLOSE)> pipe(
                    POPEN(safe_cmd.c_str(), "r"),
                    PCLOSE
                );

                if (!pipe)
                {
                    result = Error{
                        error_code::INTERNAL_ERROR,
                        "Failed to open pipe for command execution"
                    };
                }
                else
                {
                    std::string output;
                    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr)
                    {
                        output += buffer.data();
                    }

                    if (output.length() > MAX_OUTPUT_LENGTH)
                    {
                        output = output.substr(0, MAX_OUTPUT_LENGTH) + "\n...[Output truncated]...";
                    }

                    result = JSON{
                        {"output", output}
                    };
                }
            }
        }

        return result;
    }

    // ------------------------------------------------------------------
    //  Tool: run_program
    // ------------------------------------------------------------------
    Expected<JSON, Error> tool_run_program(const JSON& args)
    {
        Expected<JSON, Error> result{};

        if (!shell_execution_enabled())
        {
            result = Error{
                error_code::INTERNAL_ERROR,
                "Shell execution is disabled. Set MCPXX_SHELL_ENABLED=1 to enable it "
                "(only in a sandboxed environment)."
            };
        }
        else if (!args.contains("program") || !args["program"].is_string())
        {
            result = Error{
                error_code::INVALID_PARAMS,
                "Missing or non-string 'program' argument"
            };
        }
        else
        {
            const std::string program = args["program"].get<std::string>();
            std::vector<std::string> program_args;

            if (args.contains("args") && args["args"].is_array())
            {
                for (const auto& arg : args["args"])
                {
                    if (arg.is_string())
                    {
                        program_args.push_back(arg.get<std::string>());
                    }
                }
            }

            const std::string input_str = args.value("input", std::string{});

            try
            {
                process::Process process;
                process.start(program, program_args, true);   // merge stderr

                if (!input_str.empty())
                {
                    process.write_all_stdin(input_str);
                }
                process.close_stdin();

                std::string output = process.read_all_stdout();
                const int exit_code = process.wait();

                if (exit_code != 0)
                {
                    output += "\n[Exit code: " + std::to_string(exit_code) + "]";
                }

                if (output.length() > MAX_OUTPUT_LENGTH)
                {
                    output = output.substr(0, MAX_OUTPUT_LENGTH) + "\n...[Output truncated]...";
                }

                result = JSON{
                    {"output", output},
                    {"exit_code", exit_code}
                };
            }
            catch (const std::exception& e)
            {
                result = Error{
                    error_code::INTERNAL_ERROR,
                    std::string("Failed to run program: ") + e.what()
                };
            }
        }

        return result;
    }
}

// =====================================================================
//  main
// =====================================================================
int main(int argc, char* argv[])
{
    using namespace pooriayousefi::mcp;

    Server server{"mcp-tool-shell-server", "1.0.0"};

    // ----- run_command -------------------------------------------------
    server.add_tool(
        "run_command",
        "Executes an arbitrary shell command locally and returns combined stdout+stderr. "
        "Supports pipes, redirects, &&, etc. High-risk tool — protected by denylist and "
        "MCPXX_SHELL_ENABLED environment variable.",
        JSON::parse(R"({
            "type": "object",
            "properties": {
                "command": {
                    "type": "string",
                    "description": "The exact shell command to execute (e.g. 'ls -la' or 'python main.py') "
                },
                "input": {
                    "type": "string",
                    "description": "Optional standard input to feed to the command"
                }
            },
            "required": ["command"]
        })"),
        tool_run_command
    );

    // ----- run_program -------------------------------------------------
    server.add_tool(
        "run_program",
        "Executes a program directly (no shell) with the given arguments. "
        "Shell-injection-safe. Use when you do not need shell features.",
        JSON::parse(R"({
            "type": "object",
            "properties": {
                "program": {
                    "type": "string",
                    "description": "Executable name or path (e.g. 'python', '/usr/bin/git') "
                },
                "args": {
                    "type": "array",
                    "items": {
                        "type": "string"
                    },
                    "description": "Arguments to pass to the program"
                },
                "input": {
                    "type": "string",
                    "description": "Optional standard input to feed to the program"
                }
            },
            "required": ["program"]
        })"),
        tool_run_program
    );

    // Transport selection
    if (argc >= 2 && std::string(argv[1]) == "http")
    {
        std::string host = "127.0.0.1";
        int port = 8090;

        if (argc >= 3)
        {
            host = argv[2];
        }
        if (argc >= 4)
        {
            port = std::stoi(argv[3]);
        }

        if (!shell_execution_enabled())
        {
            std::cout << "WARNING: Shell execution is DISABLED. "
                      << "Set MCPXX_SHELL_ENABLED=1 to enable it.\n";
        }

        server.run_http(host, port);
    }
    else
    {
        if (!shell_execution_enabled())
        {
            std::cerr << "WARNING: Shell execution is DISABLED. "
                      << "Set MCPXX_SHELL_ENABLED=1 to enable it.\n";
        }

        server.run_stdio();
    }

    return 0;
}