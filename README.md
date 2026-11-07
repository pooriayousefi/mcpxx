[![M8ven Score](https://m8ven.ai/badge/mcp/pooriayousefi/mcpxx)](https://m8ven.ai/mcp/pooriayousefi/mcpxx?s=readme)

# mcpxx

mcpxx is a C++20 implementation of the Model Context Protocol (MCP), with
header-only client and server APIs and nine standalone tool servers. It
supports stdio and Streamable HTTP transports and vendors its JSON and HTTP
header dependencies under `include/`.

The protocol version advertised by this revision is `2026-07-28`.

## Features

- Register MCP tools with a name, description, JSON input schema, and handler.
- Serve `tools/list` and `tools/call` over newline-delimited JSON-RPC on stdio
  or Streamable HTTP at `/mcp`.
- Connect to an MCP server over HTTP or launch a stdio server with `Client`.
- Translate MCP tool definitions and responses with `OpenAIBridge`.
- Use a cross-platform process wrapper, length-prefixed process IPC, and an
  `Expected<Value, Error>` result type.
- Build the included tool servers on macOS, Linux, and Windows with a C++20
  compiler.

## Repository layout

```text
include/
  cpp-httplib/httplib.h              Vendored HTTP library
  nlohmann/json.hpp                  Vendored JSON library
  pooriayousefi/
    mcp/
      client.hpp                     MCP client
      openai_bridge.hpp              OpenAI-compatible tool JSON helpers
      protocol.hpp                   JSON-RPC and MCP data types
      server.hpp                     MCP server and tool registration
      transport.hpp                  Stdio and Streamable HTTP transports
    process/
      ipc.hpp                        Length-prefixed process IPC
      process.hpp                    Cross-platform process management
    utility/
      common.hpp                     Shared aliases
      expected.hpp                   Expected result type
src/
  directory_mcp_tool_server.cpp
  file_mcp_tool_server.cpp
  git_mcp_tool_server.cpp
  memory_mcp_tool_server.cpp
  random_number_mcp_tool_server.cpp
  shell_mcp_tool_server.cpp
  statistics_mcp_tool_server.cpp
  time_mcp_tool_server.cpp
  utility_mcp_tool_server.cpp
```

The `bin/` directory is for locally built executables and is excluded from
version control. There is no project-level build system; compile each server
from its corresponding source file.

## Build

From the repository root, build one server with Clang:

```sh
mkdir -p bin
clang++ -std=c++20 -O2 -pthread -I include \
  src/time_mcp_tool_server.cpp -o bin/time_mcp_tool_server
```

Replace the source and output names to build another server. GCC can be used
in the same way. On Windows with MinGW, add `-lws2_32`; use the compiler's
corresponding C++20 and threading options.

## Tool servers

Each executable runs on stdio by default. The port shown below is used only
when the server is started in HTTP mode.

| Server | Default HTTP port | Tools |
|---|---:|---|
| `shell` | 8090 | `run_command`, `run_program` |
| `time` | 8091 | `get_current_time`, `get_local_time`, `get_unix_timestamp` |
| `directory` | 8094 | `create_directory`, `list_directory`, `iterate_directories_recursively`, `delete_directory`, `get_last_modified_file` |
| `file` | 8093 | `read_file`, `read_file_base64`, `get_file_info`, `hash_file`, `search_in_file`, `write_file`, `replace_in_file`, `truncate_file`, `move_file`, `delete_file` |
| `git` | 8099 | `git_init`, `git_status`, `git_diff`, `git_log`, `git_show`, `git_add`, `git_commit`, `git_branch`, `git_current_branch`, `git_checkout`, `git_create_branch`, `git_reset`, `git_stash`, `git_remote`, `git_fetch`, `git_pull`, `git_push`, `git_clone` |
| `random_number` | 8095 | 17 random-distribution tools |
| `statistics` | 8096 | 27 statistical and arithmetic tools |
| `memory` | 8097 | `recall_memory`, `list_keys`, `save_memory` |
| `utility` | 8098 | 14 string, JSON, and UUID tools |

The file, directory, git, and shell servers operate on the host environment.
The memory server persists its in-process key/value store to a JSON file.
Statistics, random-number, time, and utility operations do not require access
to a remote service.

## Run a server

Start a server over stdio (the default):

```sh
./bin/time_mcp_tool_server
```

Start the same server over HTTP:

```sh
./bin/time_mcp_tool_server http 127.0.0.1 8091
```

The command-line interface also accepts `both` to serve stdio and HTTP
simultaneously:

```sh
./bin/time_mcp_tool_server both 127.0.0.1 8091
```

The host and port can be supplied for HTTP mode; the compiled-in default port
is used when the port is omitted. Bind to a suitably restricted interface:
tool servers can access local resources, and the shell and git servers can
perform consequential operations.

### Shell server

Shell execution is disabled unless `MCPXX_SHELL_ENABLED=1` is set in the
server's environment. The `run_command` tool executes shell commands; do not
enable it for untrusted clients or expose it on an untrusted network.

```sh
MCPXX_SHELL_ENABLED=1 ./bin/shell_mcp_tool_server
```

## Create a tool server

Include the server and result headers, define a handler, register a `Tool`, and
use `run_cli` to select the transport:

```cpp
#include "pooriayousefi/mcp/server.hpp"
#include "pooriayousefi/utility/expected.hpp"

using pooriayousefi::Expected;
using pooriayousefi::Json;
using namespace pooriayousefi::mcp;

Expected<Json, Error> echo(const Json& args)
{
    Expected<Json, Error> result{};
    if (!args.contains("text") || !args["text"].is_string())
    {
        result.set_error(Error{
            error_code::INVALID_PARAMS,
            "Missing or non-string 'text' argument"
        });
        return result;
    }

    result.set_value(Json{{"text", args["text"]}});
    return result;
}

int main(int argc, char* argv[])
{
    Server server{"example", "1.0.0"};

    Tool tool{};
    tool.name = "echo";
    tool.description = "Returns the supplied text.";
    tool.input_schema = Json{
        {"type", "object"},
        {"properties", Json{{"text", Json{{"type", "string"}}}}},
        {"required", Json::array({"text"})}
    };
    tool.handler = echo;
    server.add_tool(std::move(tool));

    return server.run_cli(argc, argv, 8090);
}
```

`Expected<Value, Error>` begins empty; set its state with `set_value()` or
`set_error()`. Inspect the state with `has_value()`, `has_error()`, or its
explicit boolean conversion, and read it with `get_value()` or `get_error()`.

## Client

`Client` can connect to an HTTP server or start a stdio server process. Both
`list_tools()` and `call_tool()` return `Expected` results:

```cpp
#include "pooriayousefi/mcp/client.hpp"

pooriayousefi::mcp::Client client{"127.0.0.1", 8091};
auto tools = client.list_tools();
auto result = client.call_tool(
    "get_current_time",
    nlohmann::json::object());
```

For stdio, construct the client with an executable path, its argument vector,
and optionally an environment-variable map.

## Dependencies and license

The repository includes `nlohmann/json` (MIT) and `cpp-httplib` (MIT) headers
under `include/`. mcpxx is distributed under the Apache License, Version 2.0;
see [LICENSE](./LICENSE).
