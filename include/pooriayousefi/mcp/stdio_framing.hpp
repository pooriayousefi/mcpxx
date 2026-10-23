// ============================================================================
//  stdio_framing.hpp — Newline-delimited JSON framing for MCP stdio transport
//  Project: mcpxx
//  Developed by: Pooria Yousefi
//  License: Apache 2.0
//
//  According to MCP 2026-07-28 the stdio transport uses one JSON-RPC
//  message per line (no Content-Length header).
// ============================================================================
#pragma once

#include "pooriayousefi/expected.hpp"
#include "pooriayousefi/mcp/protocol.hpp"
#include "pooriayousefi/process.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace pooriayousefi::mcp
{
    // ----------------------------------------------------------------
    //  STDIOFraming — pure framing utilities (no ownership of Process)
    // ----------------------------------------------------------------
    class STDIOFraming
    {
    public:
        // Write one complete JSON-RPC message followed by a newline.
        // Returns true on success.
        static bool write_message(
            process::Process& process,
            const JSON& message
        )
        {
            bool result = false;

            try
            {
                const std::string line = message.dump() + "\n";
                result = process.write_all_stdin(line);
            }
            catch (...)
            {
                result = false;
            }

            return result;
        }

        // Read one complete line (message) from the process stdout.
        // Returns the raw line without the trailing newline,
        // or an empty string on EOF / error.
        static std::string read_message(process::Process& process)
        {
            std::string result{};
            std::string accumulated{};
            std::array<std::byte, 4096> buffer{};

            while (true)
            {
                auto read_result = process.read_stdout(buffer);

                if (!read_result.has_value() || read_result.value() == 0)
                {
                    // EOF or error — return whatever we have so far
                    break;
                }

                const std::size_t bytes = read_result.value();
                const char* data = reinterpret_cast<const char*>(buffer.data());

                for (std::size_t i = 0; i < bytes; ++i)
                {
                    if (data[i] == '\n')
                    {
                        result = accumulated;
                        // We stop at the first complete line.
                        // Remaining data in the buffer is intentionally
                        // discarded for this simple synchronous reader.
                        // A production async reader would keep a leftover buffer.
                        return result;
                    }
                    else
                    {
                        accumulated.push_back(data[i]);
                    }
                }
            }

            // If we reach here we hit EOF without a newline
            result = accumulated;
            return result;
        }

        // Higher-level helper: write a request and wait for the matching response.
        // This is a simple synchronous request/response helper.
        static Expected<JSON, Error> request_response(
            process::Process& process,
            const JSON& request
        )
        {
            Expected<JSON, Error> result{};

            if (!write_message(process, request))
            {
                result = Error{
                    error_code::INTERNAL_ERROR,
                    "Failed to write request to server stdin"
                };
            }
            else
            {
                const std::string line = read_message(process);

                if (line.empty())
                {
                    result = Error{
                        error_code::INTERNAL_ERROR,
                        "Empty or truncated response from server"
                    };
                }
                else
                {
                    try
                    {
                        JSON response = JSON::parse(line);

                        if (response.contains("error") && response["error"].is_object())
                        {
                            result = Error::from_json_rpc(response["error"]);
                        }
                        else if (response.contains("result"))
                        {
                            result = std::move(response["result"]);
                        }
                        else
                        {
                            result = Error{
                                error_code::INVALID_REQUEST,
                                "Response contains neither result nor error"
                            };
                        }
                    }
                    catch (const JSON::parse_error& e)
                    {
                        result = Error{
                            error_code::PARSE_ERROR,
                            std::string("JSON parse error: ") + e.what()
                        };
                    }
                    catch (...)
                    {
                        result = Error{
                            error_code::INTERNAL_ERROR,
                            "Unknown error while parsing server response"
                        };
                    }
                }
            }

            return result;
        }
    };
}