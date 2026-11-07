// ============================================================================
//  pooriayousefi/process/process.hpp — Cross-Platform Process Management
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Start child processes with explicit arguments and environment settings,
//  and communicate through their standard streams. Platform-specific process
//  creation and pipe handling are implemented behind the Process interface.
// ============================================================================

#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <concepts>
#include <cstring>
#include <mutex>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <unordered_map>
#include <variant>
#include <vector>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <csignal>
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#include "pooriayousefi/utility/expected.hpp"

#ifndef _WIN32
// posix_spawnp() receives the environment explicitly. Not every libc exposes
// the POSIX "environ" global from <unistd.h>, so it is declared here at the
// global scope where the linker symbol lives.
extern char **environ;
#endif

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

        // Fix #3: canonical MSVC command-line encoding. Backslashes that
        // precede a quote (including the closing quote) are doubled, and a
        // literal quote is emitted as \". Arguments ending in '\' are paths
        // in practice and corrupted argv before this fix.
        inline void append_quoted(std::string &out, std::string_view arg)
        {
            const bool needs_quotes = arg.empty() ||
                arg.find_first_of(" \t\"") != std::string_view::npos;

            if (!needs_quotes)
            {
                out.append(arg);
                return;
            }

            out.push_back('"');

            std::size_t backslashes = 0;
            for (const char c : arg)
            {
                if (c == '\\')
                {
                    backslashes++;
                    continue;
                }
                if (c == '"')
                {
                    // 2n backslashes for the originals + 1 for the \" escape
                    out.append(backslashes * 2 + 1, '\\');
                    out.push_back('"');
                }
                else
                {
                    out.append(backslashes, '\\');
                    out.push_back(c);
                }
                backslashes = 0;
            }

            // Trailing backslashes sit directly before the closing quote.
            out.append(backslashes * 2, '\\');
            out.push_back('"');
        }
    }
#endif

    // ----------------------------------------------------------------
    //  POSIX-only detail helpers
    // ----------------------------------------------------------------
#ifndef _WIN32
    namespace detail
    {
        // Fix #1: writing to a pipe whose read end is closed raises SIGPIPE,
        // whose default disposition terminates this process. The library
        // contract promises that write_stdin() reports EPIPE instead, so the
        // signal is ignored once per process. Host and spawned children both
        // inherit the ignored disposition across exec; platforms that need
        // their own SIGPIPE policy should install it after start().
        inline void ignore_sigpipe()
        {
            std::signal(SIGPIPE, SIG_IGN);
        }

        inline void ignore_sigpipe_once()
        {
            static std::once_flag flag{};
            std::call_once(flag, ignore_sigpipe);
        }

        // Fix #5 (companion): creates a pipe with FD_CLOEXEC on both ends so
        // no pipe end leaks into the spawned child — the POSIX counterpart of
        // the Windows handle-list fix. Without it, the child would hold its
        // own stdin write-end forever and its stdout would never reach EOF.
        // dup2() clears FD_CLOEXEC on the child's fds 0/1/2.
        inline void make_pipe(int fds[2])
        {
            int pipe_result = -1;
            do
            {
                pipe_result = ::pipe(fds);
            } while (pipe_result != 0 && errno == EINTR);
            if (pipe_result != 0)
            {
                throw std::runtime_error{
                    "pipe() failed: " + std::string(std::strerror(errno))
                };
            }

            for (int descriptor : {fds[0], fds[1]})
            {
                const int flags = fcntl(descriptor, F_GETFD);
                if (flags < 0 ||
                    fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0)
                {
                    ::close(fds[0]);
                    ::close(fds[1]);
                    throw std::runtime_error{
                        "fcntl(FD_CLOEXEC) failed: " + std::string(std::strerror(errno))
                    };
                }
            }
        }

        // RAII owner for posix_spawn_file_actions_t so every exit path
        // (including throws) releases the spawned actions.
        class PosixSpawnActions
        {
        private:
            posix_spawn_file_actions_t value_{};
            bool ready_{false};

        public:
            PosixSpawnActions()
            {
                ready_ = (posix_spawn_file_actions_init(&value_) == 0);
            }

            ~PosixSpawnActions()
            {
                if (ready_)
                {
                    posix_spawn_file_actions_destroy(&value_);
                }
            }

            PosixSpawnActions(const PosixSpawnActions &) = delete;
            PosixSpawnActions &operator=(const PosixSpawnActions &) = delete;

            [[nodiscard]] posix_spawn_file_actions_t &get() noexcept
            {
                return value_;
            }

            [[nodiscard]] bool ready() const noexcept
            {
                return ready_;
            }
        };
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
            start(executable, arguments, redirect_error_stream, {});
        }

        void start(
            std::string_view executable,
            std::span<const std::string> arguments,
            bool redirect_error_stream,
            const std::unordered_map<std::string, std::string>& environment
        )
        {
            for (const auto& [key, value] : environment)
            {
                if (key.empty() || key.find('=') != std::string::npos ||
                    key.find('\0') != std::string::npos ||
                    value.find('\0') != std::string::npos)
                {
                    throw std::invalid_argument{
                        "Environment variables must have valid names and values without NUL bytes."
                    };
                }
            }
            if (running())
            {
                throw std::logic_error{"Process is already running."};
            }

            reset();

#ifdef _WIN32
            start_windows(executable, arguments, redirect_error_stream, environment);
#else
            start_posix(executable, arguments, redirect_error_stream, environment);
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
            Expected<std::size_t, std::error_code> result{};
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
                result.set_value(static_cast<std::size_t>(bytes_read));
            }
            else
            {
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF)
                {
                    result.set_value(static_cast<std::size_t>(0));
                }
                else
                {
                    result.set_error(std::error_code{static_cast<int>(err), std::system_category()});
                }
            }
#else
            // Fix #2: retry on EINTR so a signal never truncates the stream.
            ssize_t bytes_read = -1;
            do
            {
                bytes_read = ::read(stdout_read_.get(), buffer.data(), buffer.size());
            } while (bytes_read < 0 && errno == EINTR);

            if (bytes_read >= 0)
            {
                result.set_value(static_cast<std::size_t>(bytes_read));
            }
            else
            {
                result.set_error(std::error_code{errno, std::system_category()});
            }
#endif
            return result;
        }

        Expected<std::size_t, std::error_code> write_stdin(std::span<const std::byte> buffer) noexcept
        {
            Expected<std::size_t, std::error_code> result{};
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
                result.set_value(static_cast<std::size_t>(bytes_written));
            }
            else
            {
                result.set_error(std::error_code{static_cast<int>(GetLastError()), std::system_category()});
            }
#else
            // Fix #1 + #2: SIGPIPE is ignored, so a dead child reports EPIPE
            // here instead of killing this process; EINTR retries preserve
            // partial-write progress.
            ssize_t bytes_written = -1;
            do
            {
                bytes_written = ::write(stdin_write_.get(), buffer.data(), buffer.size());
            } while (bytes_written < 0 && errno == EINTR);

            if (bytes_written >= 0)
            {
                result.set_value(static_cast<std::size_t>(bytes_written));
            }
            else
            {
                result.set_error(std::error_code{errno, std::system_category()});
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
                if (!r.has_value() || r.get_value() == 0)
                {
                    break;
                }
                result.append(reinterpret_cast<const char *>(buffer.data()), r.get_value());
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
                if (!r.has_value() || r.get_value() == 0)
                {
                    return false;
                }
                bytes += r.get_value();
                remaining -= r.get_value();
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

        // Stop the child without closing stdout so a concurrent drain can reach EOF safely.
        void terminate_and_wait_keep_stdout() noexcept
        {
#ifdef _WIN32
            if (process_handle_.get() != nullptr)
            {
                TerminateProcess(process_handle_.get(), 1);
                if (WaitForSingleObject(process_handle_.get(), INFINITE) == WAIT_OBJECT_0)
                {
                    DWORD exit_code = 0;
                    if (GetExitCodeProcess(process_handle_.get(), &exit_code))
                    {
                        exit_code_ = static_cast<int>(exit_code);
                    }
                }
                pid_ = invalid_pid();
                stdin_write_.reset();
            }
#else
            if (pid_ > 0)
            {
                kill(pid_, SIGTERM);
                int status{};
                pid_t wait_result = -1;
                do
                {
                    wait_result = waitpid(pid_, &status, 0);
                } while (wait_result < 0 && errno == EINTR);
                if (wait_result > 0)
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
                pid_ = invalid_pid();
                stdin_write_.reset();
            }
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
                // Fix #2: an interrupted wait no longer skips the reap.
                pid_t wait_result = -1;
                do
                {
                    wait_result = waitpid(pid_, &status, 0);
                } while (wait_result < 0 && errno == EINTR);
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
            bool redirect_error_stream,
            const std::unordered_map<std::string, std::string>& environment
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

            // Fix #4: hand the child ONLY the handles it actually uses. With a
            // blanket bInheritHandles = TRUE, every concurrently running
            // Process instance's pipe write-ends leak into this child too, and
            // those pipes then never reach EOF (read_all_stdout() hangs).
            const HANDLE stderr_handle = GetStdHandle(STD_ERROR_HANDLE);

            std::array<HANDLE, 3> inheritable_handles{};
            std::size_t handle_count = 0;
            inheritable_handles[handle_count++] = stdin_read.get();
            inheritable_handles[handle_count++] = stdout_write.get();

            if (!redirect_error_stream && stderr_handle != nullptr &&
                stderr_handle != INVALID_HANDLE_VALUE)
            {
                DWORD handle_flags = 0;
                if (GetHandleInformation(stderr_handle, &handle_flags) &&
                    (handle_flags & HANDLE_FLAG_INHERIT) != 0)
                {
                    inheritable_handles[handle_count++] = stderr_handle;
                }
            }

            SIZE_T attribute_size = 0;
            // Measuring call: returns FALSE by design and fills attribute_size.
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);

            std::vector<char> attribute_buffer(attribute_size);
            LPPROC_THREAD_ATTRIBUTE_LIST attribute_list =
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());

            if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_size))
            {
                throw std::runtime_error{
                    "InitializeProcThreadAttributeList() failed: " + detail::get_last_error_string()
                };
            }

            if (!UpdateProcThreadAttribute(
                    attribute_list,
                    0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    inheritable_handles.data(),
                    handle_count * sizeof(HANDLE),
                    nullptr,
                    nullptr
                ))
            {
                DeleteProcThreadAttributeList(attribute_list);
                throw std::runtime_error{
                    "UpdateProcThreadAttribute() failed: " + detail::get_last_error_string()
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

            STARTUPINFOEXA si{};
            si.StartupInfo.cb         = sizeof(si);
            si.StartupInfo.hStdInput  = stdin_read.get();
            si.StartupInfo.hStdOutput = stdout_write.get();
            // redirect_error_stream: merge stderr into stdout pipe
            if (redirect_error_stream)
            {
                si.StartupInfo.hStdError = stdout_write.get();
            }
            else
            {
                si.StartupInfo.hStdError = stderr_handle;
            }
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.lpAttributeList     = attribute_list;

            PROCESS_INFORMATION pi{};

            std::vector<char> mutable_cmd(command_line.begin(), command_line.end());
            mutable_cmd.push_back('\0');

            std::vector<char> environment_block{};
            LPCH inherited_environment = GetEnvironmentStringsA();
            if (inherited_environment == nullptr)
            {
                throw std::runtime_error{
                    "GetEnvironmentStringsA() failed: " + detail::get_last_error_string()
                };
            }
            std::map<std::string, std::string> merged_environment{};
            for (const char* entry = inherited_environment; *entry != '\0'; entry += std::strlen(entry) + 1)
            {
                const std::string_view item{entry};
                const std::size_t separator = item.find('=', item.front() == '=' ? 1 : 0);
                if (separator != std::string_view::npos)
                {
                    std::string key{item.substr(0, separator)};
                    std::string normalized_key = key;
                    std::transform(normalized_key.begin(), normalized_key.end(), normalized_key.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                    merged_environment[normalized_key] = key + "=" + std::string{item.substr(separator + 1)};
                }
            }
            FreeEnvironmentStringsA(inherited_environment);
            for (const auto& [key, value] : environment)
            {
                if (key.empty() || key.find('=') != std::string::npos)
                {
                    throw std::invalid_argument{"Environment variable names must be non-empty and cannot contain '='."};
                }
                std::string normalized_key = key;
                std::transform(normalized_key.begin(), normalized_key.end(), normalized_key.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                merged_environment[normalized_key] = key + "=" + value;
            }
            for (const auto& [key, entry] : merged_environment)
            {
                static_cast<void>(key);
                environment_block.insert(environment_block.end(), entry.begin(), entry.end());
                environment_block.push_back('\0');
            }
            environment_block.push_back('\0');

            const BOOL ok = CreateProcessA(
                nullptr,
                mutable_cmd.data(),
                nullptr,
                nullptr,
                TRUE,
                EXTENDED_STARTUPINFO_PRESENT,
                environment_block.data(),
                nullptr,
                &si.StartupInfo,
                &pi
            );

            DeleteProcThreadAttributeList(attribute_list);

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
            bool redirect_error_stream,
            const std::unordered_map<std::string, std::string>& environment
        )
        {
            // Fix #1: from now on, writing into a dead child's stdin reports
            // EPIPE instead of raising SIGPIPE and terminating this process.
            detail::ignore_sigpipe_once();

            int stdin_pipe[2]  = {-1, -1};
            int stdout_pipe[2] = {-1, -1};

            detail::make_pipe(stdin_pipe);
            FileDescriptor stdin_read{stdin_pipe[0]};
            FileDescriptor stdin_write{stdin_pipe[1]};

            detail::make_pipe(stdout_pipe);
            FileDescriptor stdout_read{stdout_pipe[0]};
            FileDescriptor stdout_write{stdout_pipe[1]};

            // If fd 0/1/2 is closed in this process, pipe() may return it, and
            // the dup2-based stdio wiring below would silently misbehave.
            if (stdin_read.get() < 3 || stdin_write.get() < 3 ||
                stdout_read.get() < 3 || stdout_write.get() < 3)
            {
                throw std::runtime_error{
                    "pipe() returned a descriptor below 3 (stdin/stdout/stderr are closed in this process); refusing to spawn."
                };
            }

            // Fix #5: posix_spawnp replaces fork + exec. No application code
            // runs between spawn and exec, so a heap lock held by another
            // thread of this process can no longer deadlock the child, and
            // exec failures (ENOENT etc.) are reported back to start() instead
            // of silently producing exit code 127. posix_spawnp keeps the
            // execvp PATH-search semantics.
            detail::PosixSpawnActions spawn_actions{};
            if (!spawn_actions.ready())
            {
                throw std::runtime_error{
                    "posix_spawn_file_actions_init() failed."
                };
            }

            // Child stdio wiring. Everything else was opened with FD_CLOEXEC
            // and disappears at exec; dup2() clears the flag on fds 0/1/2.
            int action_result =
                posix_spawn_file_actions_adddup2(&spawn_actions.get(), stdin_read.get(), STDIN_FILENO);
            if (action_result == 0)
            {
                action_result =
                    posix_spawn_file_actions_adddup2(&spawn_actions.get(), stdout_write.get(), STDOUT_FILENO);
            }
            if (action_result == 0 && redirect_error_stream)
            {
                // redirect_error_stream: merge stderr into stdout pipe
                action_result =
                    posix_spawn_file_actions_adddup2(&spawn_actions.get(), stdout_write.get(), STDERR_FILENO);
            }
            if (action_result != 0)
            {
                throw std::runtime_error{
                    "posix_spawn_file_actions_adddup2() failed: " + std::string(std::strerror(action_result))
                };
            }

            const std::string executable_copy{executable};
            std::vector<char *> argv{};
            argv.reserve(arguments.size() + 2);
            argv.push_back(const_cast<char *>(executable_copy.c_str()));

            for (const auto &argument : arguments)
            {
                argv.push_back(const_cast<char *>(argument.c_str()));
            }

            argv.push_back(nullptr);

            std::unordered_map<std::string, std::string> merged_environment{};
            for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
            {
                const std::string_view item{*entry};
                const std::size_t separator = item.find('=');
                if (separator != std::string_view::npos)
                {
                    merged_environment[std::string{item.substr(0, separator)}] =
                        std::string{item.substr(separator + 1)};
                }
            }
            for (const auto& [key, value] : environment)
            {
                if (key.empty() || key.find('=') != std::string::npos)
                {
                    throw std::invalid_argument{"Environment variable names must be non-empty and cannot contain '='."};
                }
                merged_environment[key] = value;
            }
            std::vector<std::string> environment_strings{};
            environment_strings.reserve(merged_environment.size());
            for (const auto& [key, value] : merged_environment)
            {
                environment_strings.push_back(key + "=" + value);
            }
            std::vector<char*> environment_entries{};
            environment_entries.reserve(environment_strings.size() + 1);
            for (auto& entry : environment_strings)
            {
                environment_entries.push_back(entry.data());
            }
            environment_entries.push_back(nullptr);

            pid_t child = -1;
            const int spawn_result = posix_spawnp(
                &child,
                executable_copy.c_str(),
                &spawn_actions.get(),
                nullptr,
                argv.data(),
                environment_entries.data()
            );

            if (spawn_result != 0)
            {
                throw std::runtime_error{
                    "posix_spawnp() failed: " + std::string(std::strerror(spawn_result))
                };
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
                // Fix #2: EINTR no longer reports a live process as dead.
                pid_t wait_result = -1;
                do
                {
                    wait_result = waitpid(pid_, &status, WNOHANG);
                } while (wait_result < 0 && errno == EINTR);

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

            // Fix #2: retry on EINTR instead of failing the wait.
            pid_t wait_result = -1;
            do
            {
                wait_result = waitpid(pid_, &status, 0);
            } while (wait_result < 0 && errno == EINTR);

            if (wait_result < 0)
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
            // Fix #2: retry on EINTR instead of skipping the reap.
            pid_t wait_result = -1;
            do
            {
                wait_result = waitpid(pid_, &status, 0);
            } while (wait_result < 0 && errno == EINTR);

            if (wait_result > 0)
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