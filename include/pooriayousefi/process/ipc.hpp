// ============================================================================
//  pooriayousefi/process/ipc.hpp — Length-Prefixed Process IPC
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Builds a blocking message channel over Process standard streams. Each
//  message is encoded as a 32-bit little-endian payload length followed by
//  the payload bytes; send and receive operations return Expected results.
// ============================================================================

#pragma once

#include "pooriayousefi/utility/expected.hpp"
#include "pooriayousefi/process/process.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace pooriayousefi::process
{
    // ----------------------------------------------------------------
    //  Internal helpers for little-endian size encoding
    // ----------------------------------------------------------------
    namespace detail
    {
        inline void store_le32(std::uint8_t* dst, std::uint32_t value) noexcept
        {
            dst[0] = static_cast<std::uint8_t>( value        & 0xFFu);
            dst[1] = static_cast<std::uint8_t>((value >>  8) & 0xFFu);
            dst[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
            dst[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
        }

        [[nodiscard]] inline std::uint32_t load_le32(const std::uint8_t* src) noexcept
        {
            return  (static_cast<std::uint32_t>(src[0])      )
                  | (static_cast<std::uint32_t>(src[1]) <<  8)
                  | (static_cast<std::uint32_t>(src[2]) << 16)
                  | (static_cast<std::uint32_t>(src[3]) << 24);
        }
    }

    // ----------------------------------------------------------------
    //  IPC — length-prefixed bidirectional message channel
    // ----------------------------------------------------------------

    class IPC final
    {
    public:
        using Arguments = Process::Arguments;

        IPC() = default;

        IPC(const IPC&)            = delete;
        IPC& operator=(const IPC&) = delete;

        IPC(IPC&&) noexcept            = default;
        IPC& operator=(IPC&&) noexcept = default;

        ~IPC() = default;

        // ---- Lifecycle ------------------------------------------------

        /// Launch the child process. Same signature family as Process::start.
        void start(
            std::string_view executable,
            std::span<const std::string> arguments = {},
            bool redirect_error_stream = false)
        {
            process_.start(executable, arguments, redirect_error_stream);
        }

        void start(
            std::string_view executable,
            std::initializer_list<std::string_view> arguments,
            bool redirect_error_stream = false)
        {
            process_.start(executable, arguments, redirect_error_stream);
        }

        // ---- Message send / receive -----------------------------------

        /// Send a binary message (length-prefixed).
        /// Returns an empty Expected on success, or an error_code on failure.
        [[nodiscard]] Expected<void, std::error_code>
        send(std::span<const std::byte> payload) noexcept
        {
            Expected<void, std::error_code> result{};

            if (payload.size() > static_cast<std::size_t>(UINT32_MAX))
            {
                result.set_error(std::make_error_code(std::errc::message_size));
                return result;
            }

            // 1. Write the 4-byte little-endian size header
            std::uint8_t header[4];
            detail::store_le32(header, static_cast<std::uint32_t>(payload.size()));

            if (!write_exact(std::span<const std::byte>{
                    reinterpret_cast<const std::byte*>(header), 4}))
            {
                result.set_error(std::make_error_code(std::errc::broken_pipe));
                return result;
            }

            // 2. Write the payload (may be empty)
            if (!payload.empty())
            {
                if (!write_exact(payload))
                {
                    result.set_error(std::make_error_code(std::errc::broken_pipe));
                    return result;
                }
            }

            result.set_value();
            return result;
        }

        /// Convenience overload for text.
        [[nodiscard]] Expected<void, std::error_code>
        send(std::string_view text) noexcept
        {
            return send(std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(text.data()),
                text.size()});
        }

        /// Receive one length-prefixed binary message.
        /// On success the Expected holds the payload (may be empty).
        /// On failure (broken pipe / premature EOF / I/O error) it holds an error_code.
        [[nodiscard]] Expected<std::vector<std::byte>, std::error_code>
        receive() noexcept
        {
            Expected<std::vector<std::byte>, std::error_code> result{};

            // 1. Read the 4-byte size header
            std::uint8_t header[4]{};
            if (!read_exact(std::span<std::byte>{
                    reinterpret_cast<std::byte*>(header), 4}))
            {
                result.set_error(std::make_error_code(std::errc::broken_pipe));
                return result;
            }

            const std::uint32_t size = detail::load_le32(header);

            // 2. Read exactly 'size' payload bytes
            std::vector<std::byte> payload(size);
            if (size > 0)
            {
                if (!read_exact(std::span<std::byte>{payload.data(), payload.size()}))
                {
                    result.set_error(std::make_error_code(std::errc::broken_pipe));
                    return result;
                }
            }

            result.set_value(std::move(payload));
            return result;
        }

        /// Convenience: receive and interpret the payload as a UTF-8 / ASCII string.
        [[nodiscard]] Expected<std::string, std::error_code>
        receive_string() noexcept
        {
            Expected<std::string, std::error_code> result{};

            auto binary = receive();
            if (!binary.has_value())
            {
                result.set_error(binary.get_error());
                return result;
            }

            const auto& bytes = binary.get_value();
            result.set_value(std::string{
                reinterpret_cast<const char*>(bytes.data()),
                bytes.size()});
            return result;
        }

        /// Close the write side of the channel (child sees EOF on its stdin).
        void close_send() noexcept
        {
            process_.close_stdin();
        }

        // ---- Process status & lifecycle (delegated) -------------------

        [[nodiscard]] bool running() noexcept
        {
            return process_.running();
        }

        int wait()
        {
            return process_.wait();
        }

        void terminate() noexcept
        {
            process_.terminate();
        }

        [[nodiscard]] int exit_code() const noexcept
        {
            return process_.exit_code();
        }

        [[nodiscard]] process_id_type pid() const noexcept
        {
            return process_.pid();
        }

        /// Access the underlying Process if you need the low-level pipes.
        [[nodiscard]] Process&       process()       noexcept { return process_; }
        [[nodiscard]] const Process& process() const noexcept { return process_; }

    private:
        Process process_;

        // ---- Exact read / write helpers (loop until complete or error)

        [[nodiscard]] bool write_exact(std::span<const std::byte> data) noexcept
        {
            while (!data.empty())
            {
                auto r = process_.write_stdin(data);
                if (!r.has_value() || r.get_value() == 0)
                {
                    return false;
                }
                data = data.subspan(r.get_value());
            }
            return true;
        }

        [[nodiscard]] bool read_exact(std::span<std::byte> data) noexcept
        {
            while (!data.empty())
            {
                auto r = process_.read_stdout(data);
                if (!r.has_value() || r.get_value() == 0)
                {
                    return false;   // EOF or error before we got everything
                }
                data = data.subspan(r.get_value());
            }
            return true;
        }
    };

} // namespace pooriayousefi::process