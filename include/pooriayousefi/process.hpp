// ============================================================================
//  process.hpp — Cross-Platform Process Management (C++20)
//  Developed by: Pooria Yousefi
//  License: Apache 2.0
//
//  Changes from original:
//    - C++20 compatible (removed std::expected, std::format)
//    - Custom Expected<Result, Error> using std::variant
//    - redirect_error_stream parameter (merges stderr into stdout)
//    - read_all_stdout() / write_all_stdin() convenience helpers
//    - Expected now supports SAME-TYPE instantiations (e.g.
//      Expected<std::string, std::string>): the error constructor is
//      constrained away when Result == Error, and the explicit
//      expected_success / expected_error tags build values unambiguously
//      for any instantiation.
// ============================================================================
#pragma once

#include <array>
#include <cerrno>
#include <concepts>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <csignal>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#include "pooriayousefi/expected.hpp"

namespace pooriayousefi::process
{
    // ----------------------------------------------------------------
    //  Platform-specific type aliases
    // ----------------------------------------------------------------
#ifdef _WIN32
    using native_handle_type = HANDLE;
    using process_id_type   = DWORD;
#else
    using native_handle_type = int;
    using process_id_type   = pid_t;
#endif

    // ----------------------------------------------------------------
    //  FileDescriptor — RAII wrapper for native OS handle / fd
    // ----------------------------------------------------------------

    struct FileDescriptor
    {
#ifdef _WIN32
        HANDLE handle{nullptr};

        FileDescriptor() = default;
        explicit FileDescriptor(HANDLE h) : handle{h} {}
#else
        int fd{-1};

        FileDescriptor() = default;
        explicit FileDescriptor(int descriptor) : fd{descriptor} {}
#endif

        ~FileDescriptor()
        {
            reset();
        }

        FileDescriptor(const FileDescriptor &) = delete;
        FileDescriptor &operator=(const FileDescriptor &) = delete;

        FileDescriptor(FileDescriptor &&other) noexcept
#ifdef _WIN32
            : handle{std::exchange(other.handle, nullptr)}
#else
            : fd{std::exchange(other.fd, -1)}
#endif
        {
        }

        FileDescriptor &operator=(FileDescriptor &&other) noexcept
        {
            if (this != &other)
            {
                reset();
#ifdef _WIN32
                handle = std::exchange(other.handle, nullptr);
#else
                fd = std::exchange(other.fd, -1);
#endif
            }
            return *this;
        }

        void reset() noexcept
        {
#ifdef _WIN32
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
                handle = nullptr;
            }
#else
            if (fd != -1)
            {
                close(fd);
                fd = -1;
            }
#endif
        }

        [[nodiscard]] native_handle_type get() const noexcept
        {
#ifdef _WIN32
            return handle;
#else
            return fd;
#endif
        }
    };

    // ----------------------------------------------------------------
    //  Windows-only detail helpers
    // ----------------------------------------------------------------
#ifdef _WIN32
    namespace detail
    {
        [[nodiscard]] inline std::string get_last_error_string(
            DWORD error = GetLastError()
        )
        {
            if (error == 0)
            {
                return "No error";
            }

            LPSTR buffer = nullptr;
            const DWORD size = FormatMessageA(
                FORMAT_MESSAGE_ALLOCATE_BUFFER |
                FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr,
                error,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                reinterpret_cast<LPSTR>(&buffer),
                0,
                nullptr
            );

            std::string message;
            if (size > 0 && buffer != nullptr)
            {
                message.assign(buffer, size);
                while (!message.empty() &&
                       (message.back() == '\r' ||
                        message.back() == '\n' ||
                        message.back() == ' '))
                {
                    message.pop_back();
                }
            }
            else
            {
                message = "Unknown error (" + std::to_string(error) + ")";
            }

            if (buffer != nullptr)
            {
                LocalFree(buffer);
            }

            return message;
        }

        inline void append_quoted(std::string &out, std::string_view arg)
        {
            out.push_back('"');
            for (char c : arg)
            {
                if (c == '"')
                {
                    out.push_back('\\');
                }
                out.push_back(c);
            }
            out.push_back('"');
        }
    }
#endif

    // ----------------------------------------------------------------
    //  Process — Cross-platform process management (C++20)
    // ----------------------------------------------------------------

    class Process final
    {
    public:
        using Arguments = std::vector<std::string>;

        Process() = default;

        Process(const Process &) = delete;
        Process &operator=(const Process &) = delete;

        Process(Process &&other) noexcept
            : pid_{std::exchange(other.pid_, invalid_pid())}
            , stdin_write_{std::exchange(other.stdin_write_, FileDescriptor{})}
            , stdout_read_{std::exchange(other.stdout_read_, FileDescriptor{})}
            , exit_code_{std::exchange(other.exit_code_, -1)}
#ifdef _WIN32
            , process_handle_{std::exchange(other.process_handle_, FileDescriptor{})}
#endif
        {
        }

        Process &operator=(Process &&other) noexcept
        {
            if (this != &other)
            {
                reset();
                pid_          = std::exchange(other.pid_, invalid_pid());
                stdin_write_  = std::exchange(other.stdin_write_, FileDescriptor{});
                stdout_read_  = std::exchange(other.stdout_read_, FileDescriptor{});
                exit_code_    = std::exchange(other.exit_code_, -1);
#ifdef _WIN32
                process_handle_ = std::exchange(other.process_handle_, FileDescriptor{});
#endif
            }
            return *this;
        }

        ~Process()
        {
            reset();
        }

        // ---- Lifecycle ------------------------------------------------

        void start(
            std::string_view executable,
            std::span<const std::string> arguments = {},
            bool redirect_error_stream = false
        )
        {
            if (running())
            {
                throw std::logic_error{"Process is already running."};
            }

            reset();

#ifdef _WIN32
            start_windows(executable, arguments, redirect_error_stream);
#else
            start_posix(executable, arguments, redirect_error_stream);
#endif
        }

        void start(
            std::string_view executable,
            std::initializer_list<std::string_view> arguments,
            bool redirect_error_stream = false
        )
        {
            std::vector<std::string> args{arguments.begin(), arguments.end()};
            start(executable, std::span<const std::string>{args}, redirect_error_stream);
        }

        // ---- Native handle access ------------------------------------

        [[nodiscard]] native_handle_type get_stdin_write() const noexcept
        {
            return stdin_write_.get();
        }

        [[nodiscard]] native_handle_type get_stdout_read() const noexcept
        {
            return stdout_read_.get();
        }

        void close_stdin() noexcept
        {
            stdin_write_.reset();
        }

        // ---- Blocking I/O --------------------------------------------

        Expected<std::size_t, std::error_code> read_stdout(std::span<std::byte> buffer) noexcept
        {
            Expected<std::size_t, std::error_code> result;
#ifdef _WIN32
            DWORD bytes_read = 0;
            BOOL success = ReadFile(
                stdout_read_.get(),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytes_read,
                nullptr
            );
            if (success)
            {
                result = static_cast<std::size_t>(bytes_read);
            }
            else
            {
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF)
                {
                    result = static_cast<std::size_t>(0);
                }
                else
                {
                    result = std::error_code{static_cast<int>(err), std::system_category()};
                }
            }
#else
            ssize_t bytes_read = ::read(stdout_read_.get(), buffer.data(), buffer.size());
            if (bytes_read >= 0)
            {
                result = static_cast<std::size_t>(bytes_read);
            }
            else
            {
                result = std::error_code{errno, std::system_category()};
            }
#endif
            return result;
        }

        Expected<std::size_t, std::error_code> write_stdin(std::span<const std::byte> buffer) noexcept
        {
            Expected<std::size_t, std::error_code> result;
#ifdef _WIN32
            DWORD bytes_written = 0;
            BOOL success = WriteFile(
                stdin_write_.get(),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytes_written,
                nullptr
            );
            if (success)
            {
                result = static_cast<std::size_t>(bytes_written);
            }
            else
            {
                result = std::error_code{static_cast<int>(GetLastError()), std::system_category()};
            }
#else
            ssize_t bytes_written = ::write(stdin_write_.get(), buffer.data(), buffer.size());
            if (bytes_written >= 0)
            {
                result = static_cast<std::size_t>(bytes_written);
            }
            else
            {
                result = std::error_code{errno, std::system_category()};
            }
#endif
            return result;
        }

        // ---- Convenience helpers (blocking, read/write all) ----------

        std::string read_all_stdout()
        {
            std::string result;
            std::array<std::byte, 4096> buffer{};
            while (true)
            {
                auto r = read_stdout(buffer);
                if (!r.has_value() || r.value() == 0)
                {
                    break;
                }
                result.append(reinterpret_cast<const char *>(buffer.data()), r.value());
            }
            return result;
        }

        bool write_all_stdin(std::string_view data)
        {
            const auto *bytes = reinterpret_cast<const std::byte *>(data.data());
            std::size_t remaining = data.size();
            while (remaining > 0)
            {
                auto r = write_stdin(std::span<const std::byte>{bytes, remaining});
                if (!r.has_value() || r.value() == 0)
                {
                    return false;
                }
                bytes += r.value();
                remaining -= r.value();
            }
            return true;
        }

        // ---- Status & lifecycle queries ------------------------------

        bool running() noexcept
        {
#ifdef _WIN32
            return running_windows();
#else
            return running_posix();
#endif
        }

        int wait()
        {
#ifdef _WIN32
            return wait_windows();
#else
            return wait_posix();
#endif
        }

        void terminate() noexcept
        {
#ifdef _WIN32
            terminate_windows();
#else
            terminate_posix();
#endif
        }

        [[nodiscard]] int exit_code() const noexcept
        {
            return exit_code_;
        }

        [[nodiscard]] process_id_type pid() const noexcept
        {
            return pid_;
        }

    private:
        static constexpr process_id_type invalid_pid() noexcept
        {
#ifdef _WIN32
            return 0;
#else
            return -1;
#endif
        }

        process_id_type pid_{invalid_pid()};
        FileDescriptor  stdin_write_{};
        FileDescriptor  stdout_read_{};
        int             exit_code_{-1};
#ifdef _WIN32
        FileDescriptor process_handle_{};
#endif

        void reset() noexcept
        {
#ifdef _WIN32
            if (process_handle_.get() != nullptr)
            {
                TerminateProcess(process_handle_.get(), 1);
                WaitForSingleObject(process_handle_.get(), INFINITE);
            }
            process_handle_.reset();
            pid_ = invalid_pid();
#else
            if (pid_ > 0)
            {
                kill(pid_, SIGTERM);
                int status{};
                waitpid(pid_, &status, 0);
            }
            pid_ = invalid_pid();
#endif
            stdin_write_.reset();
            stdout_read_.reset();
        }

        // ================================================================
        //  Windows implementation
        // ================================================================
#ifdef _WIN32
        void start_windows(
            std::string_view executable,
            std::span<const std::string> arguments,
            bool redirect_error_stream
        )
        {
            SECURITY_ATTRIBUTES sa{};
            sa.nLength              = sizeof(sa);
            sa.bInheritHandle       = TRUE;
            sa.lpSecurityDescriptor = nullptr;

            HANDLE child_stdin_read  = nullptr;
            HANDLE child_stdin_write = nullptr;
            HANDLE child_stdout_read = nullptr;
            HANDLE child_stdout_write = nullptr;

            if (!CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0))
            {
                throw std::runtime_error{
                    "CreatePipe() failed: " + detail::get_last_error_string()
                };
            }

            FileDescriptor stdin_read{child_stdin_read};
            FileDescriptor stdin_write{child_stdin_write};

            if (!SetHandleInformation(stdin_write.get(), HANDLE_FLAG_INHERIT, 0))
            {
                throw std::runtime_error{
                    "SetHandleInformation() failed: " + detail::get_last_error_string()
                };
            }

            if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0))
            {
                throw std::runtime_error{
                    "CreatePipe() failed: " + detail::get_last_error_string()
                };
            }

            FileDescriptor stdout_read{child_stdout_read};
            FileDescriptor stdout_write{child_stdout_write};

            if (!SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0))
            {
                throw std::runtime_error{
                    "SetHandleInformation() failed: " + detail::get_last_error_string()
                };
            }

            std::string command_line;
            command_line.reserve(executable.size() + 1);
            detail::append_quoted(command_line, executable);

            for (const auto &argument : arguments)
            {
                command_line.push_back(' ');
                detail::append_quoted(command_line, argument);
            }

            STARTUPINFOA si{};
            si.cb         = sizeof(si);
            si.hStdInput  = stdin_read.get();
            si.hStdOutput = stdout_write.get();
            // redirect_error_stream: merge stderr into stdout pipe
            if (redirect_error_stream)
            {
                si.hStdError = stdout_write.get();
            }
            else
            {
                si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            }
            si.dwFlags = STARTF_USESTDHANDLES;

            PROCESS_INFORMATION pi{};

            std::vector<char> mutable_cmd(command_line.begin(), command_line.end());
            mutable_cmd.push_back('\0');

            const BOOL ok = CreateProcessA(
                nullptr,
                mutable_cmd.data(),
                nullptr,
                nullptr,
                TRUE,
                0,
                nullptr,
                nullptr,
                &si,
                &pi
            );

            if (!ok)
            {
                throw std::runtime_error{
                    "CreateProcessA() failed: " + detail::get_last_error_string()
                };
            }

            stdin_read.reset();
            stdout_write.reset();
            CloseHandle(pi.hThread);

            stdin_write_    = std::move(stdin_write);
            stdout_read_     = std::move(stdout_read);
            process_handle_ = FileDescriptor{pi.hProcess};
            pid_            = pi.dwProcessId;
            exit_code_      = -1;
        }

        bool running_windows() noexcept
        {
            bool result = false;

            if (process_handle_.get() != nullptr)
            {
                DWORD exit_code = 0;
                if (GetExitCodeProcess(process_handle_.get(), &exit_code))
                {
                    if (exit_code == STILL_ACTIVE)
                    {
                        result = true;
                    }
                    else
                    {
                        exit_code_ = static_cast<int>(exit_code);
                        process_handle_.reset();
                        stdin_write_.reset();
                        stdout_read_.reset();
                        pid_ = invalid_pid();
                    }
                }
            }

            return result;
        }

        int wait_windows()
        {
            if (process_handle_.get() == nullptr)
            {
                if (exit_code_ != -1)
                {
                    return exit_code_;
                }
                throw std::logic_error{"No process is running."};
            }

            const DWORD wait_result =
                WaitForSingleObject(process_handle_.get(), INFINITE);

            if (wait_result != WAIT_OBJECT_0)
            {
                throw std::runtime_error{
                    "WaitForSingleObject() failed: " + detail::get_last_error_string()
                };
            }

            DWORD exit_code = 0;
            if (!GetExitCodeProcess(process_handle_.get(), &exit_code))
            {
                throw std::runtime_error{
                    "GetExitCodeProcess() failed: " + detail::get_last_error_string()
                };
            }

            exit_code_ = static_cast<int>(exit_code);
            process_handle_.reset();
            stdin_write_.reset();
            stdout_read_.reset();
            pid_ = invalid_pid();

            return exit_code_;
        }

        void terminate_windows() noexcept
        {
            if (process_handle_.get() == nullptr)
            {
                return;
            }

            TerminateProcess(process_handle_.get(), 1);

            if (WaitForSingleObject(process_handle_.get(), INFINITE) == WAIT_OBJECT_0)
            {
                DWORD exit_code = 0;
                if (GetExitCodeProcess(process_handle_.get(), &exit_code))
                {
                    exit_code_ = static_cast<int>(exit_code);
                }
            }

            reset();
        }
#endif // _WIN32

        // ================================================================
        //  POSIX implementation
        // ================================================================
#ifndef _WIN32
        void start_posix(
            std::string_view executable,
            std::span<const std::string> arguments,
            bool redirect_error_stream
        )
        {
            int stdin_pipe[2]  = {-1, -1};
            int stdout_pipe[2] = {-1, -1};

            if (pipe(stdin_pipe) != 0)
            {
                throw std::runtime_error{
                    "pipe() failed: " + std::string(std::strerror(errno))
                };
            }

            FileDescriptor stdin_read{stdin_pipe[0]};
            FileDescriptor stdin_write{stdin_pipe[1]};

            if (pipe(stdout_pipe) != 0)
            {
                throw std::runtime_error{
                    "pipe() failed: " + std::string(std::strerror(errno))
                };
            }

            FileDescriptor stdout_read{stdout_pipe[0]};
            FileDescriptor stdout_write{stdout_pipe[1]};

            pid_t child = fork();

            if (child < 0)
            {
                throw std::runtime_error{
                    "fork() failed: " + std::string(std::strerror(errno))
                };
            }

            if (child == 0)
            {
                // ---- Child process ----

                if (dup2(stdin_read.get(), STDIN_FILENO) < 0 ||
                    dup2(stdout_write.get(), STDOUT_FILENO) < 0)
                {
                    _exit(126);
                }

                // redirect_error_stream: merge stderr into stdout pipe
                if (redirect_error_stream)
                {
                    if (dup2(stdout_write.get(), STDERR_FILENO) < 0)
                    {
                        _exit(126);
                    }
                }

                stdin_read.reset();
                stdin_write.reset();
                stdout_read.reset();
                stdout_write.reset();

                const std::string executable_copy{executable};
                std::vector<char *> argv{};
                argv.reserve(arguments.size() + 2);
                argv.push_back(const_cast<char *>(executable_copy.c_str()));

                for (const auto &argument : arguments)
                {
                    argv.push_back(const_cast<char *>(argument.c_str()));
                }

                argv.push_back(nullptr);

                execvp(executable_copy.c_str(), argv.data());

                _exit(127);
            }

            // ---- Parent process ----
            stdin_read.reset();
            stdout_write.reset();

            stdin_write_ = std::move(stdin_write);
            stdout_read_ = std::move(stdout_read);
            pid_         = child;
            exit_code_   = -1;
        }

        bool running_posix() noexcept
        {
            bool result = false;

            if (pid_ > 0)
            {
                int status{};
                const auto wait_result = waitpid(pid_, &status, WNOHANG);

                if (wait_result == 0)
                {
                    result = true;
                }
                else if (wait_result > 0)
                {
                    if (WIFEXITED(status))
                    {
                        exit_code_ = WEXITSTATUS(status);
                    }
                    else if (WIFSIGNALED(status))
                    {
                        exit_code_ = 128 + WTERMSIG(status);
                    }
                    else
                    {
                        exit_code_ = -1;
                    }

                    pid_ = invalid_pid();
                    stdin_write_.reset();
                    stdout_read_.reset();
                }
            }

            return result;
        }

        int wait_posix()
        {
            if (pid_ <= 0)
            {
                if (exit_code_ != -1)
                {
                    return exit_code_;
                }
                throw std::logic_error{"No process is running."};
            }

            int status{};

            const auto result = waitpid(pid_, &status, 0);

            if (result < 0)
            {
                throw std::runtime_error{
                    "waitpid() failed: " + std::string(std::strerror(errno))
                };
            }

            if (WIFEXITED(status))
            {
                exit_code_ = WEXITSTATUS(status);
            }
            else if (WIFSIGNALED(status))
            {
                exit_code_ = 128 + WTERMSIG(status);
            }
            else
            {
                exit_code_ = -1;
            }

            pid_ = invalid_pid();
            stdin_write_.reset();
            stdout_read_.reset();

            return exit_code_;
        }

        void terminate_posix() noexcept
        {
            if (pid_ <= 0)
            {
                return;
            }

            kill(pid_, SIGTERM);

            int status{};
            if (waitpid(pid_, &status, 0) > 0)
            {
                if (WIFEXITED(status))
                {
                    exit_code_ = WEXITSTATUS(status);
                }
                else if (WIFSIGNALED(status))
                {
                    exit_code_ = 128 + WTERMSIG(status);
                }
            }

            reset();
        }
#endif // !_WIN32
    };
}