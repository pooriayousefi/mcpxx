// ============================================================================
//  client.hpp — MCP Client (stdio transport) — C++20
//  Project: mcpxx
//  Developed by: Pooria Yousefi
//  License: Apache 2.0
//
//  Header-only client that launches an MCP server as a subprocess
//  and speaks the 2026-07-28 newline-delimited JSON-RPC protocol.
// ============================================================================
#pragma once

#include "pooriayousefi/expected.hpp"
#include "pooriayousefi/mcp/protocol.hpp"
#include "pooriayousefi/mcp/stdio_framing.hpp"
#include "pooriayousefi/process.hpp"

#include <atomic>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pooriayousefi::mcp
{
    // ----------------------------------------------------------------
    //  Client — high-level MCP client over stdio
    // ----------------------------------------------------------------
    class Client
    {
    public:
        using Result = Expected<JSON, Error>;

        Client() = default;

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        Client(Client&& other) noexcept
            : process_{std::move(other.process_)}
            , client_info_{std::move(other.client_info_)}
            , next_id_{other.next_id_.load()}
        {
        }

        Client& operator=(Client&& other) noexcept
        {
            if (this != &other)
            {
                process_     = std::move(other.process_);
                client_info_ = std::move(other.client_info_);
                next_id_.store(other.next_id_.load());
            }
            return *this;
        }

        ~Client()
        {
            close();
        }

        // ----------------------------------------------------------------
        //  Lifecycle
        // ----------------------------------------------------------------
        void set_client_info(ClientInfo info)
        {
            client_info_ = std::move(info);
        }

        const ClientInfo& client_info() const noexcept
        {
            return client_info_;
        }

        // Launch the MCP server process.
        // redirect_error_stream = true merges the server's stderr into stdout
        // (useful for debugging, but not recommended for production).
        bool start(
            std::string_view executable,
            std::span<const std::string> arguments = {},
            bool redirect_error_stream = false
        )
        {
            bool result = false;

            try
            {
                process_.start(executable, arguments, redirect_error_stream);
                result = process_.running();
            }
            catch (...)
            {
                result = false;
            }

            return result;
        }

        bool start(
            std::string_view executable,
            std::initializer_list<std::string_view> arguments,
            bool redirect_error_stream = false
        )
        {
            std::vector<std::string> args{arguments.begin(), arguments.end()};
            return start(executable, std::span<const std::string>{args}, redirect_error_stream);
        }

        bool running() noexcept
        {
            return process_.running();
        }

        void close() noexcept
        {
            process_.close_stdin();
            // The Process destructor will terminate the child if still running.
        }

        // ----------------------------------------------------------------
        //  High-level RPC helpers
        // ----------------------------------------------------------------

        // Mandatory in 2026-07-28. Returns the server's discover result.
        Result discover(JSON capabilities = JSON::object())
        {
            Result result{};

            const JSON id = next_request_id();
            const JSON request = make_discover_request(id, client_info_, std::move(capabilities));

            result = STDIOFraming::request_response(process_, request);

            return result;
        }

        // Generic request helper.
        Result request(
            std::string method,
            JSON params = JSON::object(),
            JSON capabilities = JSON::object()
        )
        {
            Result result{};

            const JSON id = next_request_id();
            const JSON req = make_request(
                id,
                std::move(method),
                std::move(params),
                client_info_,
                std::move(capabilities)
            );

            result = STDIOFraming::request_response(process_, req);

            return result;
        }

        // Send a notification (no response expected).
        bool notify(
            std::string method,
            JSON params = JSON::object()
        )
        {
            bool result = false;

            const JSON notification = make_notification(std::move(method), std::move(params));
            result = STDIOFraming::write_message(process_, notification);

            return result;
        }

    private:
        process::Process process_{};
        ClientInfo       client_info_{"mcpxx-client", "0.1.0"};
        std::atomic<std::int64_t> next_id_{1};

        JSON next_request_id()
        {
            return next_id_.fetch_add(1);
        }
    };
}