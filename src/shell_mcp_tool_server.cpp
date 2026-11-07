// ============================================================================
//  src/shell_mcp_tool_server.cpp — MCP Shell and Program Execution Server
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Provides shell-command and direct-program execution tools. Execution is
//  disabled unless MCPXX_SHELL_ENABLED=1 is set. Runs over stdio by default
//  or Streamable HTTP on port 8090 by default; treat access as privileged.
// ============================================================================

#include "pooriayousefi/utility/common.hpp"
#include "pooriayousefi/mcp/server.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
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

namespace pooriayousefi::mcp
{
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
    Expected<Json, Error> tool_run_command(const Json& args)
    {
        Expected<Json, Error> result{};

        if (!shell_execution_enabled())
        {
            result.set_error(Error{
                error_code::INTERNAL_ERROR,
                "Shell execution is disabled. Set MCPXX_SHELL_ENABLED=1 to enable it "
                "(only in a sandboxed environment)."
            });
        }
        else if (!args.contains("command") || !args["command"].is_string())
        {
            result.set_error(Error{
                error_code::INVALID_PARAMS,
                "Missing or non-string 'command' argument"
            });
        }
        else
        {
            const std::string command = args["command"].get<std::string>();

            if (is_denylisted(command))
            {
                result.set_error(Error{
                    error_code::INVALID_PARAMS,
                    "Command rejected: matches a denylisted catastrophic pattern"
                });
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
                    result.set_error(Error{
                        error_code::INTERNAL_ERROR,
                        "Failed to open pipe for command execution"
                    });
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

                    // NOTE: Json::object(...) is mandatory here. The single-pair
                    // braced form Json{{"output", output}} can be interpreted as
                    // an ARRAY (both elements are strings) by nlohmann.
                    result.set_value(Json::object({
                        {"output", output}
                    }));
                }
            }
        }

        return result;
    }

    // ------------------------------------------------------------------
    //  Tool: run_program
    // ------------------------------------------------------------------
    Expected<Json, Error> tool_run_program(const Json& args)
    {
        Expected<Json, Error> result{};

        if (!shell_execution_enabled())
        {
            result.set_error(Error{
                error_code::INTERNAL_ERROR,
                "Shell execution is disabled. Set MCPXX_SHELL_ENABLED=1 to enable it "
                "(only in a sandboxed environment)."
            });
        }
        else if (!args.contains("program") || !args["program"].is_string())
        {
            result.set_error(Error{
                error_code::INVALID_PARAMS,
                "Missing or non-string 'program' argument"
            });
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

                // NOTE: Json::object(...) is mandatory here (see run_command).
                result.set_value(Json::object({
                    {"output", output},
                    {"exit_code", exit_code}
                }));
            }
            catch (const std::exception& e)
            {
                result.set_error(Error{
                    error_code::INTERNAL_ERROR,
                    std::string("Failed to run program: ") + e.what()
                });
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
    server.set_instructions(
        "Controlled shell and program execution. run_command runs a shell "
        "command (pipes, redirects, &&) and returns combined stdout+stderr; "
        "run_program executes a binary directly without a shell (injection-safe). "
        "Both tools are disabled unless the environment variable "
        "MCPXX_SHELL_ENABLED=1 is set (use only in a sandboxed environment)."
    );

    // ----- run_command -------------------------------------------------
    Tool run_command_tool{};
    run_command_tool.name = "run_command";
    run_command_tool.description =
        "Executes an arbitrary shell command locally and returns combined stdout+stderr. "
        "Supports pipes, redirects, &&, etc. High-risk tool — protected by denylist and "
        "MCPXX_SHELL_ENABLED environment variable.";
    run_command_tool.input_schema = Json::parse(R"({
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
    })");
    run_command_tool.handler = tool_run_command;
    server.add_tool(std::move(run_command_tool));

    // ----- run_program -------------------------------------------------
    Tool run_program_tool{};
    run_program_tool.name = "run_program";
    run_program_tool.description =
        "Executes a program directly (no shell) with the given arguments. "
        "Shell-injection-safe. Use when you do not need shell features.";
    run_program_tool.input_schema = Json::parse(R"({
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
    })");
    run_program_tool.handler = tool_run_program;
    server.add_tool(std::move(run_program_tool));

    // ----- transport selection + safety notice --------------------------
    // run_cli contract (server.hpp):
    //   no arguments            -> stdio transport (newline-delimited Json-RPC)
    //   http [host] [port]      -> Streamable HTTP (default port below)
    //   --http [host] [port]    -> Streamable HTTP
    //   --help | -h             -> usage on stderr
    if (!shell_execution_enabled())
    {
        std::cerr << "WARNING: Shell execution is DISABLED. "
                  << "Set MCPXX_SHELL_ENABLED=1 to enable it.\n";
    }

    const int exit_code = server.run_cli(argc, argv, 8090);
    return exit_code;
}