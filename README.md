
[![M8ven Score](https://m8ven.ai/badge/mcp/pooriayousefi-mcpxx-gdiirb?v=6f67ce1af6957ae53c0e50b378d85e75)](https://m8ven.ai/mcp/pooriayousefi-mcpxx-gdiirb?s=readme)

# mcpxx

A modern, header-only **Model Context Protocol (MCP)** implementation written in **C++20**.

- Protocol version: **2026-07-28**
- Transports: **stdio** + **Streamable HTTP**
- Focus: easy creation of MCP tool servers (especially converting existing REST / OpenAI-compatible tool servers)

## Features

- Full support for the current MCP specification (stateless, `server/discover`, per-request `_meta`)
- Dual transport out of the box:
  - **stdio** (newline-delimited JSON-RPC) – recommended for local clients
  - **HTTP** (POST `/mcp` + convenience endpoints)
- Simple tool registration API
- Rich error type (`Expected<JSON, Error>`)
- Cross-platform (macOS, Linux, Windows)
- Header-only core library

## Project Layout

```
mcpxx/
├── bin/                          # compiled binaries
├── include/
│   └── pooriayousefi/
│       ├── expected.hpp
│       ├── process.hpp
│       └── mcp/
│           ├── protocol.hpp
│           ├── tool.hpp
│           ├── server.hpp
│           ├── client.hpp
│           ├── stdio_framing.hpp
│           ├── resource.hpp      # placeholder
│           └── prompt.hpp        # placeholder
├── src/
│   ├── mcp_tool_shell_server.cpp         # example tool server
│   ├── test_mcp_tool_shell_server.cpp    # full protocol test client
│   └── test_mcp_protocol.cpp             # unit tests
└── README.md
```

## Building

```bash
# Example tool server
clang++ -std=c++20 -O3 -Wall -Wextra -I include \
  src/mcp_tool_shell_server.cpp -o bin/mcp_tool_shell_server

# Full test client
clang++ -std=c++20 -O3 -Wall -Wextra -I include \
  src/test_mcp_tool_shell_server.cpp -o bin/test_mcp_tool_shell_server

# Protocol unit tests
clang++ -std=c++20 -O3 -Wall -Wextra -I include \
  src/test_mcp_protocol.cpp -o bin/test_mcp_protocol
```

(On Linux you may need `-lpthread`. On Windows with MinGW add `-lws2_32`.)

## Quick Start – Running the Example Server

### 1. STDIO mode (most common for MCP clients)

```bash
# The test client launches the server itself
MCPXX_SHELL_ENABLED=1 ./bin/test_mcp_tool_shell_server stdio
```

### 2. HTTP mode

```bash
# Terminal 1 – start the server
MCPXX_SHELL_ENABLED=1 ./bin/mcp_tool_shell_server http 127.0.0.1 8090

# Terminal 2 – run the test client
./bin/test_mcp_tool_shell_server http http://127.0.0.1:8090
```

> **Safety note**  
> The example server only executes shell/program commands when the environment variable  
> `MCPXX_SHELL_ENABLED=1` is set. This is intentional.

## How to Create Your Own MCP Tool Server

1. Create a new `.cpp` file (e.g. `src/mcp_tool_my_service.cpp`).
2. Include the server header and register tools:

```cpp
#include "pooriayousefi/mcp/server.hpp"
#include "pooriayousefi/expected.hpp"

using namespace pooriayousefi::mcp;

Expected<JSON, Error> my_tool_handler(const JSON& args)
{
    // your logic here
    return JSON{{"result", "hello"}};
}

int main(int argc, char* argv[])
{
    Server server{"my-service", "1.0.0"};

    server.add_tool(
        "my_tool",
        "Description of the tool",
        JSON::parse(R"({
            "type": "object",
            "properties": {
                "query": { "type": "string" }
            },
            "required": ["query"]
        })"),
        my_tool_handler
    );

    // Dual transport support
    if (argc >= 2 && std::string(argv[1]) == "http")
    {
        std::string host = argc >= 3 ? argv[2] : "127.0.0.1";
        int port = argc >= 4 ? std::stoi(argv[3]) : 8090;
        server.run_http(host, port);
    }
    else
    {
        server.run_stdio();
    }
}
```

3. Build and run exactly like the shell example.

Both transports are available with zero extra work.

## Using with llama.cpp / llama-ui

### Option A – STDIO (Recommended)

Create a file `mcp.json`:

```json
{
  "mcpServers": {
    "shell": {
      "command": "/ABSOLUTE/PATH/TO/mcpxx/bin/mcp_tool_shell_server",
      "args": [],
      "env": {
        "MCPXX_SHELL_ENABLED": "1"
      },
      "timeout_ms": 60000
    }
  }
}
```

Start `llama-server`:

```bash
llama-server \
  -m /path/to/your/model.gguf \
  --mcp-servers-config /path/to/mcp.json \
  --host 127.0.0.1 \
  --port 8080 \
  -ngl 99
```

(You can also pass the JSON inline with `--mcp-servers-json`.)

The tools will appear as:
- `shell_run_command`
- `shell_run_program`

### Option B – HTTP (for llama-ui Web UI)

1. Start the MCP server:

```bash
MCPXX_SHELL_ENABLED=1 ./bin/mcp_tool_shell_server http 127.0.0.1 8090
```

2. Start `llama-server` with the MCP proxy enabled:

```bash
llama-server \
  -m /path/to/your/model.gguf \
  --ui-mcp-proxy \
  --host 127.0.0.1 \
  --port 8080 \
  -ngl 99
```

3. In the Web UI:
   - Go to **MCP Servers** (or Settings → MCP)
   - Click **Add New Server**
   - Enter: `http://127.0.0.1:8090/mcp`
   - Enable the toggle **“Use llama-server proxy”**
   - Save and enable the server

## Convenience HTTP Endpoints

When running in HTTP mode the server also exposes:

| Endpoint          | Method | Description                          |
|-------------------|--------|--------------------------------------|
| `/mcp`            | POST   | Official MCP endpoint                |
| `/schema`         | GET    | OpenAI-style tools schema            |
| `/tools/call`     | POST   | Simple REST-style tool invocation    |

## Safety

The example `mcp_tool_shell_server` contains a denylist and requires an explicit environment variable before any command is executed.  
**Never** run it with `MCPXX_SHELL_ENABLED=1` on an untrusted machine or without additional sandboxing.

## License

Apache 2.0
```
