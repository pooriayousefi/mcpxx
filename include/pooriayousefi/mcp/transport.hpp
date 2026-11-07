// ============================================================================
//  pooriayousefi/mcp/transport.hpp — MCP Stdio and Streamable HTTP Transports
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Provides server and client implementations for newline-delimited JSON-RPC
//  over stdio and MCP requests over the HTTP /mcp endpoint.
// ============================================================================

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <functional>
#include <memory>
#include <iostream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include "cpp-httplib/httplib.h"
#include "pooriayousefi/utility/common.hpp"
#include "pooriayousefi/mcp/protocol.hpp"
#include "pooriayousefi/process/process.hpp"

namespace pooriayousefi::mcp
{
    class HttpServerTransport
    {
    public:
        using RequestHandler = std::function<std::string(std::string_view request_body)>;

        HttpServerTransport() = default;
        ~HttpServerTransport()
        {
            stop();
        }

        HttpServerTransport(const HttpServerTransport&) = delete;
        HttpServerTransport& operator=(const HttpServerTransport&) = delete;
        HttpServerTransport(HttpServerTransport&&) noexcept = default;
        HttpServerTransport& operator=(HttpServerTransport&&) noexcept = default;

        void set_handler(RequestHandler handler)
        {
            handler_ = std::move(handler);
        }

        bool listen(std::string_view host, int port)
        {
            bound_host_ = std::string{host};
            prepare_server();
            const std::string host_copy{host};
            return server_->listen(host_copy.c_str(), port);
        }

        bool bind(std::string_view host, int port)
        {
            bound_host_ = std::string{host};
            prepare_server();
            return server_->bind_to_port(bound_host_, port);
        }

        bool listen_after_bind()
        {
            return server_ != nullptr && server_->listen_after_bind();
        }

        void stop()
        {
            if (server_ != nullptr)
            {
                server_->stop();
            }
        }

    private:
        std::unique_ptr<httplib::Server> server_{nullptr};
        RequestHandler handler_{};
        std::string bound_host_{};

        void prepare_server()
        {
            server_ = std::make_unique<httplib::Server>();
            server_->Post("/mcp", [this](const httplib::Request& req, httplib::Response& res)
            {
                handle_http_request(req, res);
            });
            server_->Get("/mcp", [this](const httplib::Request& req, httplib::Response& res)
            {
                handle_http_get(req, res);
            });
        }

        void handle_http_request(const httplib::Request& req, httplib::Response& res)
        {
            const std::string origin = req.get_header_value("Origin");
            if (!origin.empty() && !is_valid_origin(origin))
            {
                set_json_error(res, 403, std::nullopt, "Invalid Origin header");
                return;
            }

            if (!has_media_type(req.get_header_value("Accept"), "application/json") ||
                !has_media_type(req.get_header_value("Accept"), "text/event-stream"))
            {
                set_json_error(res, 406, std::nullopt, "Accept header must allow application/json and text/event-stream");
                return;
            }

            if (!is_json_content_type(req.get_header_value("Content-Type")))
            {
                set_json_error(res, 415, std::nullopt, "Content-Type must be application/json");
                return;
            }

            const Json body = Json::parse(req.body, nullptr, false);
            if (body.is_discarded())
            {
                set_json_error(res, 400, ErrorCode::PARSE_ERROR, "Parse error: invalid JSON");
                return;
            }

            if (!body.is_object() || !body.contains("jsonrpc") ||
                !body["jsonrpc"].is_string() || body["jsonrpc"] != "2.0" ||
                !body.contains("method") || !body["method"].is_string() ||
                body["method"].get_ref<const std::string&>().empty())
            {
                set_json_error(res, 400, ErrorCode::INVALID_REQUEST, "Invalid JSON-RPC message");
                return;
            }

            const bool has_id = body.contains("id");
            if (has_id && !is_valid_request_id(body["id"]))
            {
                set_json_error(res, 400, ErrorCode::INVALID_REQUEST, "Request ID must be a string or integer", body["id"]);
                return;
            }

            const std::string& method = body["method"].get_ref<const std::string&>();
            const Json params = body.value("params", Json::object());
            if (!params.is_object())
            {
                set_json_error(res, 400, ErrorCode::INVALID_REQUEST, "Request params must be an object",
                               has_id ? std::optional<Json>{body["id"]} : std::nullopt);
                return;
            }

            const std::string method_header = req.get_header_value("Mcp-Method");
            if (method_header.empty() || method_header != method)
            {
                set_json_error(res, 400, ErrorCode::HEADER_MISMATCH,
                               "Mcp-Method header does not match request body",
                               has_id ? std::optional<Json>{body["id"]} : std::nullopt);
                return;
            }

            if (has_id)
            {
                const Json meta = params.value("_meta", Json::object());
                if (!meta.is_object() ||
                    !meta.contains(std::string{MCP_PROTOCOL_VERSION_META}) ||
                    !meta[std::string{MCP_PROTOCOL_VERSION_META}].is_string() ||
                    !meta.contains(std::string{MCP_CLIENT_CAPABILITIES_META}) ||
                    !meta[std::string{MCP_CLIENT_CAPABILITIES_META}].is_object())
                {
                    set_json_error(res, 400, ErrorCode::INVALID_REQUEST, "Request _meta is missing required MCP metadata", body["id"]);
                    return;
                }

                const std::string version_header = req.get_header_value("MCP-Protocol-Version");
                if (version_header.empty() ||
                    version_header != meta[std::string{MCP_PROTOCOL_VERSION_META}].get_ref<const std::string&>())
                {
                    set_json_error(res, 400, ErrorCode::HEADER_MISMATCH,
                                   "MCP-Protocol-Version header does not match request _meta", body["id"]);
                    return;
                }
                if (version_header != MCP_PROTOCOL_VERSION)
                {
                    set_json_error(res, 400, ErrorCode::UNSUPPORTED_PROTOCOL_VERSION,
                                   "Unsupported protocol version",
                                   body["id"],
                                   Json{{"supportedVersions", Json::array({MCP_PROTOCOL_VERSION})}});
                    return;
                }

            }
            else if (!req.has_header("MCP-Protocol-Version") ||
                     req.get_header_value("MCP-Protocol-Version") != MCP_PROTOCOL_VERSION)
            {
                set_json_error(res, 400, ErrorCode::HEADER_MISMATCH,
                               "MCP-Protocol-Version header is missing or unsupported");
                return;
            }

            std::optional<std::string> expected_name{};
            if (method == "tools/call" || method == "resources/read" || method == "prompts/get")
            {
                const std::string source_key = method == "resources/read" ? "uri" : "name";
                if (!params.contains(source_key) || !params[source_key].is_string() ||
                    params[source_key].get_ref<const std::string&>().empty())
                {
                    set_json_error(res, 400, ErrorCode::INVALID_PARAMS,
                                   "Request params are missing a valid '" + source_key + "'",
                                   has_id ? std::optional<Json>{body["id"]} : std::nullopt);
                    return;
                }
                expected_name = params[source_key].get<std::string>();
            }

            const bool has_name_header = req.has_header("Mcp-Name");
            if ((expected_name && (!has_name_header || req.get_header_value("Mcp-Name") != *expected_name)) ||
                (!expected_name && has_name_header))
            {
                set_json_error(res, 400, ErrorCode::HEADER_MISMATCH,
                               "Mcp-Name header does not match request body",
                               has_id ? std::optional<Json>{body["id"]} : std::nullopt);
                return;
            }

            if (!has_id)
            {
                res.status = 202;
                res.body.clear();
                return;
            }

            if (!handler_)
            {
                set_json_error(res, 500, ErrorCode::INTERNAL_ERROR, "No handler registered", body["id"]);
                return;
            }

            const std::string response_body = handler_(req.body);
            const Json response = Json::parse(response_body, nullptr, false);
            if (response.is_object() && response.contains("error") &&
                response["error"].is_object() && response["error"].value("code", 0) ==
                    static_cast<int>(ErrorCode::METHOD_NOT_FOUND))
            {
                res.status = 404;
            }
            else
            {
                res.status = 200;
            }
            res.set_content(response_body, "application/json");
        }

        void handle_http_get(const httplib::Request& req, httplib::Response& res)
        {
            const std::string origin = req.get_header_value("Origin");
            if (!origin.empty() && !is_valid_origin(origin))
            {
                set_json_error(res, 403, std::nullopt, "Invalid Origin header");
                return;
            }

            res.status = 405;
            res.set_header("Allow", "POST");
            res.body.clear();
        }

        static std::string lowercase(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return value;
        }

        static bool is_json_content_type(std::string content_type)
        {
            const auto semicolon = content_type.find(';');
            if (semicolon != std::string::npos)
            {
                content_type.resize(semicolon);
            }
            while (!content_type.empty() && std::isspace(static_cast<unsigned char>(content_type.back())))
            {
                content_type.pop_back();
            }
            return lowercase(std::move(content_type)) == "application/json";
        }

        static bool has_media_type(const std::string& accept, std::string_view media_type)
        {
            std::size_t start{0};
            while (start < accept.size())
            {
                const auto comma = accept.find(',', start);
                std::string token = accept.substr(start, comma == std::string::npos ? comma : comma - start);
                const auto semicolon = token.find(';');
                if (semicolon != std::string::npos)
                {
                    token.resize(semicolon);
                }
                const auto first = token.find_first_not_of(" \t");
                const auto last = token.find_last_not_of(" \t");
                if (first != std::string::npos &&
                    lowercase(token.substr(first, last - first + 1)) == media_type)
                {
                    return true;
                }
                if (comma == std::string::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            return false;
        }

        static bool is_loopback_host(std::string host)
        {
            host = lowercase(std::move(host));
            if (host == "localhost" || host == "[::1]" || host == "::1")
            {
                return true;
            }
            if (host.starts_with("127."))
            {
                std::size_t start{0};
                unsigned component_index{0};
                while (start < host.size())
                {
                    const auto dot = host.find('.', start);
                    const auto end = dot == std::string::npos ? host.size() : dot;
                    if (end == start || component_index == 4)
                    {
                        return false;
                    }
                    unsigned value{0};
                    for (std::size_t index = start; index < end; ++index)
                    {
                        const unsigned char ch = static_cast<unsigned char>(host[index]);
                        if (!std::isdigit(ch))
                        {
                            return false;
                        }
                        value = value * 10 + static_cast<unsigned>(ch - '0');
                        if (value > 255)
                        {
                            return false;
                        }
                    }
                    if (component_index == 0 && value != 127)
                    {
                        return false;
                    }
                    ++component_index;
                    if (dot == std::string::npos)
                    {
                        break;
                    }
                    start = dot + 1;
                }
                return component_index == 4;
            }
            return false;
        }

        bool is_valid_origin(const std::string& origin) const
        {
            const auto scheme_end = origin.find("://");
            if (scheme_end == std::string::npos)
            {
                return false;
            }
            const std::string scheme = lowercase(origin.substr(0, scheme_end));
            if (scheme != "http" && scheme != "https")
            {
                return false;
            }
            const std::string authority = origin.substr(scheme_end + 3);
            if (authority.empty() || authority.find_first_of("/?#@ \t\r\n") != std::string::npos)
            {
                return false;
            }
            std::string hostname = authority;
            std::size_t port_separator{std::string::npos};
            if (hostname.front() == '[')
            {
                const auto closing_bracket = hostname.find(']');
                if (closing_bracket == std::string::npos ||
                    (closing_bracket + 1 < hostname.size() &&
                     hostname[closing_bracket + 1] != ':'))
                {
                    return false;
                }
                if (closing_bracket + 1 < hostname.size())
                {
                    port_separator = closing_bracket + 1;
                }
                hostname.resize(closing_bracket + 1);
            }
            else if ((port_separator = hostname.find(':')) != std::string::npos)
            {
                if (hostname.find(':', port_separator + 1) != std::string::npos)
                {
                    return false;
                }
                hostname.resize(port_separator);
            }
            if (port_separator != std::string::npos)
            {
                const std::size_t port_start = port_separator + 1;
                if (port_start >= authority.size())
                {
                    return false;
                }
                unsigned port{0};
                for (std::size_t index = port_start; index < authority.size(); ++index)
                {
                    const unsigned char ch = static_cast<unsigned char>(authority[index]);
                    if (!std::isdigit(ch))
                    {
                        return false;
                    }
                    const unsigned digit = static_cast<unsigned>(ch - '0');
                    if (port > (65535 - digit) / 10)
                    {
                        return false;
                    }
                    port = port * 10 + digit;
                }
                if (port == 0)
                {
                    return false;
                }
            }
            const std::string normalized_hostname = lowercase(std::move(hostname));
            return is_loopback_host(normalized_hostname) ||
                   (!bound_host_.empty() &&
                    normalized_hostname == lowercase(bound_host_) &&
                    normalized_hostname != "0.0.0.0" &&
                    normalized_hostname != "::");
        }

        static void set_json_error(
            httplib::Response& response,
            int status,
            std::optional<ErrorCode> code,
            std::string message,
            std::optional<Json> id = std::nullopt,
            std::optional<Json> data = std::nullopt)
        {
            Json body{{"jsonrpc", "2.0"}, {"error", {{"code", code ? static_cast<int>(*code) : static_cast<int>(ErrorCode::INVALID_REQUEST)},
                                                         {"message", std::move(message)}}}};
            if (id && is_valid_request_id(*id))
            {
                body["id"] = *id;
            }
            if (data)
            {
                body["error"]["data"] = *data;
            }
            response.status = status;
            response.set_content(body.dump(), "application/json");
        }
    };

    class HttpClientTransport
    {
    public:
        HttpClientTransport(std::string_view host, int port)
            : host_{host}, port_{port}
        {
            client_ = std::make_unique<httplib::Client>(host_, port_);
        }

        ~HttpClientTransport() = default;

        HttpClientTransport(const HttpClientTransport&) = delete;
        HttpClientTransport& operator=(const HttpClientTransport&) = delete;
        HttpClientTransport(HttpClientTransport&&) noexcept = default;
        HttpClientTransport& operator=(HttpClientTransport&&) noexcept = default;

        [[nodiscard]] Expected<std::string, std::error_code>
        send_request(std::string_view json_rpc_payload, std::string_view method = "", std::string_view name = "") noexcept
        {
            Expected<std::string, std::error_code> result{};

            if (method.empty())
            {
                result.set_error(std::make_error_code(std::errc::invalid_argument));
                return result;
            }

            httplib::Headers headers{};
            headers.emplace("Content-Type", "application/json");
            headers.emplace("MCP-Protocol-Version", std::string{MCP_PROTOCOL_VERSION});
            headers.emplace("Accept", "application/json, text/event-stream");

            if (!method.empty())
            {
                headers.emplace("Mcp-Method", std::string{method});
            }
            if (!name.empty())
            {
                headers.emplace("Mcp-Name", std::string{name});
            }

            auto res = client_->Post("/mcp", headers, std::string{json_rpc_payload}, "application/json");
            if (res)
            {
                if (res->status == 200)
                {
                    std::string content_type = res->get_header_value("Content-Type");
                    std::transform(content_type.begin(), content_type.end(), content_type.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    if (content_type.starts_with("application/json"))
                    {
                        result.set_value(res->body);
                    }
                    else if (content_type.starts_with("text/event-stream"))
                    {
                        auto sse_result = extract_sse_response(res->body, json_rpc_payload);
                        if (sse_result.has_value())
                        {
                            result.set_value(sse_result.get_value());
                        }
                        else
                        {
                            result.set_error(sse_result.get_error());
                        }
                    }
                    else
                    {
                        result.set_error(std::make_error_code(std::errc::protocol_error));
                    }
                }
                else if (res->status == 202 && res->body.empty())
                {
                    result.set_value(res->body);
                }
                else
                {
                    std::cerr << "[HttpClientTransport] Server returned HTTP error status: " << res->status
                              << " | Body: " << res->body << "\n";
                    result.set_error(std::make_error_code(std::errc::bad_message));
                }
            }
            else
            {
                auto err = res.error();
                std::cerr << "[HttpClientTransport] Connection failed, httplib error code: "
                          << static_cast<int>(err) << "\n";
                result.set_error(std::make_error_code(std::errc::connection_refused));
            }

            return result;
        }

    private:
        std::string host_{};
        int port_{8080};
        std::unique_ptr<httplib::Client> client_{nullptr};

        [[nodiscard]] static Expected<std::string, std::error_code> extract_sse_response(
            const std::string& event_stream,
            std::string_view request_body)
        {
            Expected<std::string, std::error_code> result{};
            const Json request = Json::parse(request_body, nullptr, false);
            if (request.is_discarded() || !request.is_object() || !request.contains("id"))
            {
                result.set_error(std::make_error_code(std::errc::protocol_error));
                return result;
            }

            const Json expected_id = request["id"];
            std::istringstream input{event_stream};
            std::string line{};
            std::string data{};
            bool found{false};
            while (std::getline(input, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                if (line.empty())
                {
                    found = parse_sse_data(data, expected_id, result) || found;
                    data.clear();
                }
                else if (line.starts_with("data:"))
                {
                    std::string_view value{line};
                    value.remove_prefix(5);
                    if (!value.empty() && value.front() == ' ')
                    {
                        value.remove_prefix(1);
                    }
                    if (!data.empty())
                    {
                        data.push_back('\n');
                    }
                    data.append(value);
                }
            }
            found = parse_sse_data(data, expected_id, result) || found;
            if (!found)
            {
                result.set_error(std::make_error_code(std::errc::protocol_error));
            }
            return result;
        }

        [[nodiscard]] static bool parse_sse_data(
            const std::string& data,
            const Json& expected_id,
            Expected<std::string, std::error_code>& result)
        {
            if (data.empty())
            {
                return false;
            }
            const Json message = Json::parse(data, nullptr, false);
            if (message.is_discarded() || !message.is_object() ||
                !message.contains("id") || message["id"] != expected_id)
            {
                return false;
            }
            result.set_value(message.dump());
            return true;
        }
    };

    class StdioClientTransport
    {
    public:
        StdioClientTransport() = default;
        ~StdioClientTransport()
        {
            stop();
        }

        StdioClientTransport(const StdioClientTransport&) = delete;
        StdioClientTransport& operator=(const StdioClientTransport&) = delete;
        StdioClientTransport(StdioClientTransport&&) noexcept = delete;
        StdioClientTransport& operator=(StdioClientTransport&&) noexcept = delete;

        void start(
            std::string_view executable,
            std::span<const std::string> arguments = {},
            const std::unordered_map<std::string, std::string>& environment = {})
        {
            process_.start(executable, arguments, false, environment);
        }

        void stop() noexcept
        {
            if (process_.running())
            {
                process_.terminate();
            }
        }

        [[nodiscard]] Expected<std::string, std::error_code>
        send_receive(std::string_view json_rpc_payload) noexcept
        {
            Expected<std::string, std::error_code> result{};

            std::lock_guard<std::mutex> lock{mutex_};
            const std::string request = std::string{json_rpc_payload} + "\n";
            const bool sent = process_.write_all_stdin(request);
            if (!sent)
            {
                result.set_error(std::make_error_code(std::errc::broken_pipe));
            }
            else
            {
                while (true)
                {
                    const auto newline = pending_output_.find('\n');
                    if (newline != std::string::npos)
                    {
                        std::string response = pending_output_.substr(0, newline);
                        pending_output_.erase(0, newline + 1);
                        if (!response.empty() && response.back() == '\r')
                        {
                            response.pop_back();
                        }
                        result.set_value(std::move(response));
                        break;
                    }

                    std::array<std::byte, 4096> buffer{};
                    const auto read = process_.read_stdout(buffer);
                    if (read.has_error())
                    {
                        result.set_error(read.get_error());
                        break;
                    }
                    if (read.get_value() == 0)
                    {
                        result.set_error(std::make_error_code(std::errc::broken_pipe));
                        break;
                    }
                    pending_output_.append(
                        reinterpret_cast<const char*>(buffer.data()),
                        read.get_value());
                }
            }

            return result;
        }

    private:
        process::Process process_{};
        std::mutex mutex_{};
        std::string pending_output_{};
    };
} // namespace pooriayousefi::mcp