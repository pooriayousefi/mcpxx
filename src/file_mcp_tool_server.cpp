// ============================================================================
//  src/file_mcp_tool_server.cpp — MCP File Operations Server
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Provides file reading, metadata, hashing, searching, writing, replacement,
//  truncation, moving, and deletion tools. Runs over stdio by default or
//  Streamable HTTP on port 8093 by default; operations access host files.
// ============================================================================

#include <iostream>
#include <string>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <system_error>
#include <vector>
#include <utility>
#include <nlohmann/json.hpp>
#include "pooriayousefi/utility/common.hpp"
#include "pooriayousefi/utility/expected.hpp"
#include "pooriayousefi/mcp/protocol.hpp"
#include "pooriayousefi/mcp/server.hpp"

#ifdef _WIN32
    #include <fcntl.h>
    #include <io.h>
    #include <share.h>
#else
    #include <unistd.h>
#endif

namespace pooriayousefi::mcp
{
    namespace fs = std::filesystem;

    [[nodiscard]] Expected<std::string, std::string> require_string_arg(const Json& args, const char* key)
    {
        Expected<std::string, std::string> result{};

        if (!args.contains(key) || !args[key].is_string())
        {
            result.set_error(std::string("Missing or non-string '") + key + "' in arguments");
        }
        else
        {
            result.set_value(args[key].get<std::string>());
        }

        return result;
    }

    [[nodiscard]] Expected<std::int64_t, std::string> require_int_arg(const Json& args, const char* key)
    {
        Expected<std::int64_t, std::string> result{};

        if (!args.contains(key) || !args[key].is_number_integer())
        {
            result.set_error(std::string("Missing or non-integer '") + key + "' in arguments");
        }
        else
        {
            result.set_value(args[key].get<std::int64_t>());
        }

        return result;
    }

    [[nodiscard]] Expected<std::int64_t, std::string> optional_int_arg(const Json& args, const char* key, std::int64_t default_value)
    {
        Expected<std::int64_t, std::string> result{};

        if (!args.contains(key))
        {
            result.set_value(default_value);
        }
        else
        {
            if (!args[key].is_number_integer())
            {
                result.set_error(std::string("Optional argument '") + key + "' must be an integer.");
            }
            else
            {
                result.set_value(args[key].get<std::int64_t>());
            }
        }

        return result;
    }

    [[nodiscard]] Expected<bool, std::string> optional_bool_arg(const Json& args, const char* key, bool default_value)
    {
        Expected<bool, std::string> result{};

        if (!args.contains(key))
        {
            result.set_value(default_value);
        }
        else
        {
            if (!args[key].is_boolean())
            {
                result.set_error(std::string("Optional argument '") + key + "' must be a boolean.");
            }
            else
            {
                result.set_value(args[key].get<bool>());
            }
        }

        return result;
    }

    [[nodiscard]] Expected<fs::path, std::string> require_path_arg(const Json& args, const char* key)
    {
        Expected<fs::path, std::string> result{};

        auto text_or_error = require_string_arg(args, key);
        if (!text_or_error.has_value())
        {
            result.set_error(text_or_error.get_error());
        }
        else
        {
            if (text_or_error.get_value().empty())
            {
                result.set_error(std::string("'") + key + "' must not be empty.");
            }
            else
            {
                result.set_value(fs::path(text_or_error.get_value()));
            }
        }

        return result;
    }

    [[nodiscard]] Expected<std::regex, std::string> compile_pattern(const std::string& pattern)
    {
        Expected<std::regex, std::string> result{};

        if (pattern.empty())
        {
            result.set_error("'pattern' must not be empty.");
        }
        else
        {
            try
            {
                std::regex compiled{pattern, std::regex::ECMAScript};

                if (std::regex_search("", compiled))
                {
                    result.set_error("'pattern' can match an empty string and is rejected.");
                }
                else
                {
                    result.set_value(std::move(compiled));
                }
            }
            catch (const std::regex_error& e)
            {
                result.set_error(std::string("Invalid 'pattern': ") + e.what());
            }
        }

        return result;
    }

    struct FileSegment
    {
        std::string data{};
        std::int64_t size_bytes{0};
        std::int64_t returned_bytes{0};
        bool truncated{false};
    };

    [[nodiscard]] Expected<FileSegment, std::string> read_file_segment(const fs::path& path, std::int64_t offset, std::int64_t limit)
    {
        Expected<FileSegment, std::string> result{};

        std::error_code ec{};
        if (!fs::is_regular_file(path, ec))
        {
            result.set_error("Path is not a regular file: '" + path.string() + "'");
        }
        else
        {
            const std::uintmax_t unsigned_file_size = fs::file_size(path, ec);
            if (ec || unsigned_file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max()))
            {
                result.set_error("Stat failed for '" + path.string() + "': " +
                                 (ec ? ec.message() : "file is too large"));
            }
            else
            {
                const std::int64_t file_size = static_cast<std::int64_t>(unsigned_file_size);
                if (offset > file_size)
                {
                    FileSegment segment{"", file_size, 0, false};
                    result.set_value(std::move(segment));
                }
                else
                {
                    std::int64_t available = file_size - offset;
                    std::int64_t want = available < limit ? available : limit;

                    std::ifstream file(path, std::ios::binary);
                    if (!file)
                    {
                        result.set_error("Open failed for '" + path.string() + "': cannot open for reading");
                    }
                    else
                    {
                        file.seekg(static_cast<std::streamoff>(offset));

                        std::string buffer(static_cast<std::size_t>(want), '\0');
                        file.read(buffer.data(), static_cast<std::streamsize>(want));
                        std::streamsize got = file.gcount();
                        buffer.resize(static_cast<std::size_t>(got));

                        FileSegment segment{};
                        segment.data = std::move(buffer);
                        segment.size_bytes = file_size;
                        segment.returned_bytes = static_cast<std::int64_t>(got);
                        segment.truncated = (offset + static_cast<std::int64_t>(got)) < file_size;

                        result.set_value(std::move(segment));
                    }
                }
            }
        }

        return result;
    }

    struct ReplaceResult
    {
        std::string text{};
        std::int64_t count{0};
        bool exceeds_limit{false};
    };

    [[nodiscard]] ReplaceResult replace_pattern_literally(
        const std::string& text,
        const std::regex& pattern,
        const std::string& replacement,
        std::int64_t limit,
        std::size_t max_output_size
    )
    {
        std::string output{};
        std::int64_t count{0};
        bool exceeds_limit{false};

        const auto append = [&output, max_output_size](std::string_view text)
        {
            if (text.size() > max_output_size - output.size())
            {
                return false;
            }
            output.append(text);
            return true;
        };

        auto matches_begin = std::sregex_iterator(text.begin(), text.end(), pattern);
        auto matches_end = std::sregex_iterator();

        std::size_t last_end{0};
        for (auto it = matches_begin; it != matches_end && count < limit; ++it)
        {
            const std::smatch& match = *it;

            const std::size_t match_start = static_cast<std::size_t>(match.position());
            if (!append(std::string_view{text}.substr(last_end, match_start - last_end)) ||
                !append(replacement))
            {
                exceeds_limit = true;
                break;
            }
            last_end = static_cast<std::size_t>(match.position() + match.length());
            ++count;
        }

        if (!exceeds_limit && !append(std::string_view{text}.substr(last_end)))
        {
            exceeds_limit = true;
        }

        ReplaceResult result{std::move(output), count, exceeds_limit};
        return result;
    }

    [[nodiscard]] std::string base64_encode(const std::string& raw)
    {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string encoded{};
        encoded.reserve(((raw.size() + 2) / 3) * 4);

        std::size_t index{0};
        while (index < raw.size())
        {
            std::uint32_t byte_a = static_cast<std::uint32_t>(static_cast<unsigned char>(raw[index]));
            std::uint32_t byte_b{0};
            std::uint32_t byte_c{0};
            std::size_t remainder = raw.size() - index;

            if (remainder > 1)
            {
                byte_b = static_cast<std::uint32_t>(static_cast<unsigned char>(raw[index + 1]));
            }
            if (remainder > 2)
            {
                byte_c = static_cast<std::uint32_t>(static_cast<unsigned char>(raw[index + 2]));
            }

            std::uint32_t triple = (byte_a << 16) | (byte_b << 8) | byte_c;

            encoded.push_back(table[(triple >> 18) & 0x3F]);
            encoded.push_back(table[(triple >> 12) & 0x3F]);

            if (remainder > 1)
            {
                encoded.push_back(table[(triple >> 6) & 0x3F]);
            }
            else
            {
                encoded.push_back('=');
            }

            if (remainder > 2)
            {
                encoded.push_back(table[triple & 0x3F]);
            }
            else
            {
                encoded.push_back('=');
            }

            index += 3;
        }

        return encoded;
    }

    [[nodiscard]] std::string to_hex64(std::uint64_t value)
    {
        static const char* digits = "0123456789abcdef";

        std::string out(16, '0');
        for (int i = 15; i >= 0; --i)
        {
            out[static_cast<std::size_t>(i)] = digits[value & 0xF];
            value >>= 4;
        }

        return out;
    }

    [[nodiscard]] std::uint64_t fnv1a_update(std::uint64_t hash, const char* data, std::size_t length)
    {
        constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;
        std::uint64_t result_hash{hash};
        for (std::size_t i = 0; i < length; ++i)
        {
            result_hash ^= static_cast<unsigned char>(data[i]);
            result_hash *= FNV_PRIME;
        }

        return result_hash;
    }

    // ------------------------------------------------------------------ tools

    [[nodiscard]] ToolCallResult tool_write_file(const Json& args)
    {
        ToolCallResult result{};

        auto path_res = require_path_arg(args, "path");
        auto content_res = require_string_arg(args, "content");
        auto append_res = optional_bool_arg(args, "append", false);

        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            if (content_res.has_error())
            {
                result.is_error = true;
                result.content = content_res.get_error();
            }
            else
            {
                if (append_res.has_error())
                {
                    result.is_error = true;
                    result.content = append_res.get_error();
                }
                else
                {
                    constexpr std::int64_t MAX_WRITE_BYTES = 8'388'608;
                    if (static_cast<std::int64_t>(content_res.get_value().size()) > MAX_WRITE_BYTES)
                    {
                        result.is_error = true;
                        result.content = "'content' exceeds the write size limit.";
                    }
                    else
                    {
                        const fs::path& path = path_res.get_value();

                        std::ios::openmode mode = std::ios::binary | std::ios::trunc;
                        if (append_res.get_value())
                        {
                            mode = std::ios::binary | std::ios::app;
                        }

                        std::ofstream file(path, mode);
                        if (!file)
                        {
                            result.is_error = true;
                            result.content = "Write failed for '" + path.string() + "': cannot open for writing";
                        }
                        else
                        {
                            file.write(content_res.get_value().data(), static_cast<std::streamsize>(content_res.get_value().size()));
                            file.flush();

                            if (!file)
                            {
                                result.is_error = true;
                                result.content = "Write failed for '" + path.string() + "': write rejected";
                            }
                            else
                            {
                                Json resp = Json::object();
                                resp["path"] = path.string();
                                resp["bytes_written"] = static_cast<std::int64_t>(content_res.get_value().size());
                                resp["appended"] = append_res.get_value();

                                result.is_error = false;
                                result.content = resp.dump();
                            }
                        }
                    }
                }
            }
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_read_file(const Json& args)
    {
        ToolCallResult result{};
        constexpr std::int64_t MAX_TEXT_READ_BYTES = 1'048'576;

        auto path_res = require_path_arg(args, "path");
        auto offset_res = optional_int_arg(args, "offset", 0);
        auto length_res = optional_int_arg(args, "length", MAX_TEXT_READ_BYTES);

        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            if (offset_res.has_error())
            {
                result.is_error = true;
                result.content = offset_res.get_error();
            }
            else
            {
                if (offset_res.get_value() < 0)
                {
                    result.is_error = true;
                    result.content = "'offset' must not be negative.";
                }
                else
                {
                    if (length_res.has_error())
                    {
                        result.is_error = true;
                        result.content = length_res.get_error();
                    }
                    else
                    {
                        if (length_res.get_value() < 0)
                        {
                            result.is_error = true;
                            result.content = "'length' must not be negative.";
                        }
                        else
                        {
                            std::int64_t limit = length_res.get_value();
                            if (limit > MAX_TEXT_READ_BYTES)
                            {
                                limit = MAX_TEXT_READ_BYTES;
                            }

                            auto segment_res = read_file_segment(path_res.get_value(), offset_res.get_value(), limit);
                            if (segment_res.has_error())
                            {
                                result.is_error = true;
                                result.content = segment_res.get_error();
                            }
                            else
                            {
                                FileSegment segment = segment_res.get_value();
                                Json resp = Json::object();
                                resp["path"] = path_res.get_value().string();
                                resp["size_bytes"] = segment.size_bytes;
                                resp["offset"] = offset_res.get_value();
                                resp["returned_bytes"] = segment.returned_bytes;
                                resp["truncated"] = segment.truncated;
                                resp["content"] = std::move(segment.data);

                                result.is_error = false;
                                result.content = resp.dump();
                            }
                        }
                    }
                }
            }
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_read_file_base64(const Json& args)
    {
        ToolCallResult result{};
        constexpr std::int64_t MAX_BINARY_READ_BYTES = 3'145'728;

        auto path_res = require_path_arg(args, "path");
        auto offset_res = optional_int_arg(args, "offset", 0);
        auto length_res = optional_int_arg(args, "length", MAX_BINARY_READ_BYTES);

        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            if (offset_res.has_error())
            {
                result.is_error = true;
                result.content = offset_res.get_error();
            }
            else
            {
                if (offset_res.get_value() < 0)
                {
                    result.is_error = true;
                    result.content = "'offset' must not be negative.";
                }
                else
                {
                    if (length_res.has_error())
                    {
                        result.is_error = true;
                        result.content = length_res.get_error();
                    }
                    else
                    {
                        if (length_res.get_value() < 0)
                        {
                            result.is_error = true;
                            result.content = "'length' must not be negative.";
                        }
                        else
                        {
                            std::int64_t limit = length_res.get_value();
                            if (limit > MAX_BINARY_READ_BYTES)
                            {
                                limit = MAX_BINARY_READ_BYTES;
                            }

                            auto segment_res = read_file_segment(path_res.get_value(), offset_res.get_value(), limit);
                            if (segment_res.has_error())
                            {
                                result.is_error = true;
                                result.content = segment_res.get_error();
                            }
                            else
                            {
                                FileSegment segment = segment_res.get_value();
                                std::string encoded = base64_encode(segment.data);

                                Json resp = Json::object();
                                resp["path"] = path_res.get_value().string();
                                resp["size_bytes"] = segment.size_bytes;
                                resp["offset"] = offset_res.get_value();
                                resp["raw_bytes"] = segment.returned_bytes;
                                resp["encoded_bytes"] = static_cast<std::int64_t>(encoded.size());
                                resp["truncated"] = segment.truncated;
                                resp["content_base64"] = std::move(encoded);

                                result.is_error = false;
                                result.content = resp.dump();
                            }
                        }
                    }
                }
            }
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_get_file_info(const Json& args)
    {
        ToolCallResult result{};

        auto path_res = require_path_arg(args, "path");
        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            const fs::path& path = path_res.get_value();
            std::error_code ec{};
            fs::file_status status = fs::status(path, ec);

            if (ec)
            {
                result.is_error = true;
                result.content = "Stat failed for '" + path.string() + "': " + ec.message();
                return result;
            }

            bool exists = fs::exists(status);
            bool is_regular = fs::is_regular_file(status);
            bool is_directory = fs::is_directory(status);
            std::int64_t size_bytes{0};

            if (is_regular)
            {
                size_bytes = static_cast<std::int64_t>(fs::file_size(path, ec));
                if (ec)
                {
                    result.is_error = true;
                    result.content = "Stat failed for '" + path.string() + "': " + ec.message();
                    return result;
                }
            }

            Json resp = Json::object();
            resp["path"] = path.string();
            resp["exists"] = exists;
            resp["is_regular_file"] = is_regular;
            resp["is_directory"] = is_directory;
            resp["size_bytes"] = size_bytes;

            result.is_error = false;
            result.content = resp.dump();
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_hash_file(const Json& args)
    {
        ToolCallResult result{};

        auto path_res = require_path_arg(args, "path");
        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            const fs::path& path = path_res.get_value();
            std::error_code ec{};
            if (!fs::is_regular_file(path, ec))
            {
                result.is_error = true;
                result.content = "Path is not a regular file: '" + path.string() + "'";
            }
            else
            {
                std::ifstream file(path, std::ios::binary);
                if (!file)
                {
                    result.is_error = true;
                    result.content = "Open failed for '" + path.string() + "': cannot open for reading";
                }
                else
                {
                    constexpr std::uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
                    constexpr std::size_t STREAM_CHUNK_SIZE = 65'536;

                    std::uint64_t hash = FNV_OFFSET_BASIS;
                    std::uint64_t bytes_hashed{0};
                    std::vector<char> buffer(STREAM_CHUNK_SIZE);

                    while (file)
                    {
                        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                        std::streamsize got = file.gcount();
                        if (got > 0)
                        {
                            hash = fnv1a_update(hash, buffer.data(), static_cast<std::size_t>(got));
                            bytes_hashed += static_cast<std::uint64_t>(got);
                        }
                    }

                    if (file.bad())
                    {
                        result.is_error = true;
                        result.content = "Read failed for '" + path.string() + "'";
                    }
                    else
                    {
                        Json resp = Json::object();
                        resp["path"] = path.string();
                        resp["algorithm"] = "fnv1a-64";
                        resp["hash_hex"] = to_hex64(hash);
                        resp["bytes_hashed"] = static_cast<std::int64_t>(bytes_hashed);

                        result.is_error = false;
                        result.content = resp.dump();
                    }
                }
            }
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_search_in_file(const Json& args)
    {
        ToolCallResult result{};
        constexpr std::int64_t DEFAULT_MAX_MATCHES = 100;
        constexpr std::int64_t HARD_MAX_MATCHES = 1'000;
        constexpr std::int64_t MAX_SEARCH_FILE_BYTES = 16'777'216;
        constexpr std::int64_t MAX_SNIPPET_CHARS = 512;

        auto path_res = require_path_arg(args, "path");
        auto pattern_res = require_string_arg(args, "pattern");
        auto max_res = optional_int_arg(args, "max_matches", DEFAULT_MAX_MATCHES);

        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            if (pattern_res.has_error())
            {
                result.is_error = true;
                result.content = pattern_res.get_error();
            }
            else
            {
                if (max_res.has_error())
                {
                    result.is_error = true;
                    result.content = max_res.get_error();
                }
                else
                {
                    if (max_res.get_value() <= 0)
                    {
                        result.is_error = true;
                        result.content = "'max_matches' must be a positive integer.";
                    }
                    else
                    {
                        const fs::path& path = path_res.get_value();
                        std::int64_t max_matches = max_res.get_value();
                        if (max_matches > HARD_MAX_MATCHES)
                        {
                            max_matches = HARD_MAX_MATCHES;
                        }

                        auto pattern_compile_res = compile_pattern(pattern_res.get_value());
                        if (pattern_compile_res.has_error())
                        {
                            result.is_error = true;
                            result.content = pattern_compile_res.get_error();
                        }
                        else
                        {
                            std::error_code size_ec{};
                            const std::uintmax_t unsigned_file_size = fs::file_size(path, size_ec);

                            if (size_ec || unsigned_file_size > static_cast<std::uintmax_t>(MAX_SEARCH_FILE_BYTES))
                            {
                                result.is_error = true;
                                result.content = "Search failed for '" + path.string() + "': cannot stat file or file exceeds the search size limit";
                            }
                            else
                            {
                                std::ifstream file(path);
                                if (!file)
                                {
                                    result.is_error = true;
                                    result.content = "Open failed for '" + path.string() + "': cannot open for reading";
                                }
                                else
                                {
                                    Json matches = Json::array();
                                    std::int64_t line_number{0};
                                    std::int64_t match_count{0};
                                    bool truncated{false};
                                    std::string line{};

                                    while (std::getline(file, line))
                                    {
                                        ++line_number;
                                        if (!line.empty() && line.back() == '\r')
                                        {
                                            line.pop_back();
                                        }

                                        std::smatch match{};
                                        if (std::regex_search(line, match, pattern_compile_res.get_value()))
                                        {
                                            if (match_count < max_matches)
                                            {
                                                std::string snippet = line;
                                                if (static_cast<std::int64_t>(snippet.size()) > MAX_SNIPPET_CHARS)
                                                {
                                                    snippet.resize(static_cast<std::size_t>(MAX_SNIPPET_CHARS));
                                                }

                                                Json match_obj = Json::object();
                                                match_obj["line_number"] = line_number;
                                                match_obj["position"] = static_cast<std::int64_t>(match.position());
                                                match_obj["snippet"] = std::move(snippet);
                                                matches.push_back(std::move(match_obj));
                                                ++match_count;
                                            }
                                            else
                                            {
                                                truncated = true;
                                                break;
                                            }
                                        }
                                    }

                                    if (file.bad())
                                    {
                                        result.is_error = true;
                                        result.content = "Read failed for '" + path.string() + "'";
                                    }
                                    else
                                    {
                                        Json resp = Json::object();
                                        resp["path"] = path.string();
                                        resp["match_count"] = match_count;
                                        resp["truncated"] = truncated;
                                        resp["matches"] = std::move(matches);

                                        result.is_error = false;
                                        result.content = resp.dump();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_replace_in_file(const Json& args)
    {
        ToolCallResult result{};
        constexpr std::int64_t MAX_EDIT_BYTES = 8'388'608;

        auto path_res = require_path_arg(args, "path");
        auto pattern_res = require_string_arg(args, "pattern");
        auto replacement_res = require_string_arg(args, "replacement");
        auto max_res = optional_int_arg(args, "max_replacements", -1);

        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            if (pattern_res.has_error())
            {
                result.is_error = true;
                result.content = pattern_res.get_error();
            }
            else
            {
                if (replacement_res.has_error())
                {
                    result.is_error = true;
                    result.content = replacement_res.get_error();
                }
                else
                {
                    if (max_res.has_error())
                    {
                        result.is_error = true;
                        result.content = max_res.get_error();
                    }
                    else
                    {
                        const fs::path& path = path_res.get_value();
                        auto pattern_compile_res = compile_pattern(pattern_res.get_value());

                        if (pattern_compile_res.has_error())
                        {
                            result.is_error = true;
                            result.content = pattern_compile_res.get_error();
                        }
                        else
                        {
                            std::error_code size_ec{};
                            const std::uintmax_t unsigned_file_size = fs::file_size(path, size_ec);

                            if (size_ec || unsigned_file_size > static_cast<std::uintmax_t>(MAX_EDIT_BYTES))
                            {
                                result.is_error = true;
                                result.content = "Replace failed for '" + path.string() + "': cannot stat file or file exceeds the in-place edit size limit";
                            }
                            else
                            {
                                std::ifstream file(path, std::ios::binary);
                                if (!file)
                                {
                                    result.is_error = true;
                                    result.content = "Open failed for '" + path.string() + "': cannot open for reading";
                                }
                                else
                                {
                                    file.seekg(0, std::ios::end);
                                    std::streamsize length = file.tellg();
                                    file.seekg(0, std::ios::beg);

                                    if (length < 0)
                                    {
                                        result.is_error = true;
                                        result.content = "Read failed for '" + path.string() + "': unable to determine file length";
                                        return result;
                                    }

                                    std::string content(static_cast<std::size_t>(length), '\0');
                                    file.read(content.data(), length);
                                    if (file.bad())
                                    {
                                        result.is_error = true;
                                        result.content = "Read failed for '" + path.string() + "'";
                                        return result;
                                    }
                                    content.resize(static_cast<std::size_t>(file.gcount()));

                                    std::int64_t limit = max_res.get_value();
                                    if (limit < 0)
                                    {
                                        limit = std::numeric_limits<std::int64_t>::max();
                                    }

                                    std::int64_t old_size = static_cast<std::int64_t>(content.size());
                                    ReplaceResult replaced = replace_pattern_literally(
                                        content,
                                        pattern_compile_res.get_value(),
                                        replacement_res.get_value(),
                                        limit,
                                        static_cast<std::size_t>(MAX_EDIT_BYTES)
                                    );

                                    if (replaced.exceeds_limit)
                                    {
                                        result.is_error = true;
                                        result.content = "Replace output exceeds the in-place edit size limit.";
                                    }
                                    else
                                    {
                                        std::ofstream out_file(path, std::ios::binary | std::ios::trunc);
                                        if (!out_file)
                                        {
                                            result.is_error = true;
                                            result.content = "Write failed for '" + path.string() + "': cannot open for writing";
                                        }
                                        else
                                        {
                                            out_file.write(replaced.text.data(), static_cast<std::streamsize>(replaced.text.size()));
                                            out_file.flush();

                                            if (!out_file)
                                            {
                                                result.is_error = true;
                                                result.content = "Write failed for '" + path.string() + "': write rejected";
                                            }
                                            else
                                            {
                                                Json resp = Json::object();
                                                resp["path"] = path.string();
                                                resp["replacements"] = replaced.count;
                                                resp["old_size"] = old_size;
                                                resp["new_size"] = static_cast<std::int64_t>(replaced.text.size());

                                                result.is_error = false;
                                                result.content = resp.dump();
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_truncate_file(const Json& args)
    {
        ToolCallResult result{};

        auto path_res = require_path_arg(args, "path");
        auto size_res = require_int_arg(args, "size");

        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            if (size_res.has_error())
            {
                result.is_error = true;
                result.content = size_res.get_error();
            }
            else
            {
                if (size_res.get_value() < 0)
                {
                    result.is_error = true;
                    result.content = "'size' must not be negative.";
                }
                else
                {
                    const fs::path& path = path_res.get_value();
                    std::int64_t new_size = size_res.get_value();

                    bool ok{false};
                    std::string detail{};

#ifdef _WIN32
                    int fd = -1;
                    if (_sopen_s(&fd, path.string().c_str(), _O_WRONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd < 0)
                    {
                        detail = "cannot open file for writing";
                    }
                    else
                    {
                        errno_t resize_result = _chsize_s(fd, static_cast<__int64>(new_size));
                        _close(fd);

                        if (resize_result == 0)
                        {
                            ok = true;
                        }
                        else
                        {
                            detail = "_chsize_s rejected the resize";
                        }
                    }
#else
                    if (::truncate(path.string().c_str(), static_cast<off_t>(new_size)) == 0)
                    {
                        ok = true;
                    }
                    else
                    {
                        detail = std::string("errno ") + std::to_string(errno);
                    }
#endif

                    if (ok)
                    {
                        Json resp = Json::object();
                        resp["path"] = path.string();
                        resp["new_size"] = new_size;

                        result.is_error = false;
                        result.content = resp.dump();
                    }
                    else
                    {
                        result.is_error = true;
                        result.content = "Truncate failed for '" + path.string() + "': " + detail;
                    }
                }
            }
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_move_file(const Json& args)
    {
        ToolCallResult result{};

        auto source_res = require_path_arg(args, "source");
        auto destination_res = require_path_arg(args, "destination");
        auto overwrite_res = optional_bool_arg(args, "overwrite", false);

        if (source_res.has_error())
        {
            result.is_error = true;
            result.content = source_res.get_error();
        }
        else
        {
            if (destination_res.has_error())
            {
                result.is_error = true;
                result.content = destination_res.get_error();
            }
            else
            {
                if (overwrite_res.has_error())
                {
                    result.is_error = true;
                    result.content = overwrite_res.get_error();
                }
                else
                {
                    const fs::path& source = source_res.get_value();
                    const fs::path& destination = destination_res.get_value();
                    std::error_code ec{};

                    if (!fs::is_regular_file(source, ec))
                    {
                        result.is_error = true;
                        result.content = "Source is not a regular file: '" + source.string() + "'";
                    }
                    else
                    {
                        const bool destination_exists = fs::exists(destination, ec);
                        if (ec)
                        {
                            result.is_error = true;
                            result.content = "Stat failed for '" + destination.string() + "': " + ec.message();
                        }
                        else
                        {
                            if (destination_exists && !overwrite_res.get_value())
                            {
                                result.is_error = true;
                                result.content = "Destination already exists: '" + destination.string() + "'";
                            }
                            else
                            {
                                std::error_code equivalent_ec{};
                                if (destination_exists && fs::equivalent(source, destination, equivalent_ec) && !equivalent_ec)
                                {
                                    Json resp = Json::object();
                                    resp["source"] = source.string();
                                    resp["destination"] = destination.string();
                                    resp["method"] = "unchanged";
                                    result.is_error = false;
                                    result.content = resp.dump();
                                }
                                else
                                {
                                    std::string method = "rename";
                                    bool moved{false};
                                    std::string detail{};

                                    const auto copy_and_remove = [&](fs::copy_options options)
                                    {
                                        std::error_code copy_ec{};
                                        const bool copied = fs::copy_file(source, destination, options, copy_ec);

                                        if (copy_ec || !copied)
                                        {
                                            detail = copy_ec ? copy_ec.message() : "copy did not create a destination";
                                        }
                                        else
                                        {
                                            std::error_code remove_ec{};
                                            if (fs::remove(source, remove_ec))
                                            {
                                                method = "copy_remove";
                                                moved = true;
                                            }
                                            else
                                            {
                                                detail = "failed to remove source after copy";
                                                if (remove_ec)
                                                {
                                                    detail += ": " + remove_ec.message();
                                                }
                                                if (!destination_exists)
                                                {
                                                    std::error_code cleanup_ec{};
                                                    fs::remove(destination, cleanup_ec);
                                                    if (cleanup_ec)
                                                    {
                                                        detail += "; rollback failed: " + cleanup_ec.message();
                                                    }
                                                }
                                            }
                                        }
                                    };

                                    if (overwrite_res.get_value())
                                    {
                                        std::error_code rename_ec{};
                                        fs::rename(source, destination, rename_ec);
                                        if (!rename_ec)
                                        {
                                            moved = true;
                                        }
                                        else
                                        {
                                            copy_and_remove(fs::copy_options::overwrite_existing);
                                        }
                                    }
                                    else
                                    {
                                        copy_and_remove(fs::copy_options::none);
                                    }

                                    if (moved)
                                    {
                                        Json resp = Json::object();
                                        resp["source"] = source.string();
                                        resp["destination"] = destination.string();
                                        resp["method"] = method;

                                        result.is_error = false;
                                        result.content = resp.dump();
                                    }
                                    else
                                    {
                                        result.is_error = true;
                                        result.content = "Move failed for '" + source.string() + "': " + detail;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_delete_file(const Json& args)
    {
        ToolCallResult result{};

        auto path_res = require_path_arg(args, "path");
        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            const fs::path& path = path_res.get_value();
            std::error_code ec{};
            fs::file_status status = fs::status(path, ec);

            if (ec || !fs::exists(status))
            {
                result.is_error = true;
                result.content = "File does not exist: '" + path.string() + "'";
            }
            else
            {
                if (fs::is_directory(status))
                {
                    result.is_error = true;
                    result.content = "Path is a directory: '" + path.string() + "'";
                }
                else
                {
                    std::error_code remove_ec{};
                    const bool removed = fs::remove(path, remove_ec);

                    if (remove_ec || !removed)
                    {
                        std::string detail = remove_ec ? remove_ec.message() : std::string("file disappeared during delete");
                        result.is_error = true;
                        result.content = "Delete failed for '" + path.string() + "': " + detail;
                    }
                    else
                    {
                        Json resp = Json::object();
                        resp["path"] = path.string();
                        resp["deleted"] = true;

                        result.is_error = false;
                        result.content = resp.dump();
                    }
                }
            }
        }

        return result;
    }
}

int main(int argc, char* argv[])
{
    using namespace pooriayousefi;

    mcp::Server server{};

    Json path_prop = Json::object();
    path_prop["type"] = "string";
    path_prop["description"] = "Absolute or relative file path.";

    Json offset_prop = Json::object();
    offset_prop["type"] = "integer";
    offset_prop["description"] = "Byte offset to start reading from (default 0).";

    mcp::ToolDefinition read_file_def{};
    read_file_def.name = "read_file";
    read_file_def.description = "Reads a text segment of a file with a hard 1 MiB output cap. Reports truncation instead of failing.";
    read_file_def.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"path", path_prop},
            {"offset", offset_prop},
            {"length", {{"type", "integer"}, {"description", "Maximum number of bytes to return (hard cap 1 MiB)."}}}
        }},
        {"required", Json::array({"path"})}
    };
    server.register_tool(read_file_def, mcp::tool_read_file);

    mcp::ToolDefinition read_file_base64_def{};
    read_file_base64_def.name = "read_file_base64";
    read_file_base64_def.description = "Reads raw binary bytes and returns them base64-encoded on the wire, with a hard raw cap of 3 MiB.";
    read_file_base64_def.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"path", path_prop},
            {"offset", offset_prop},
            {"length", {{"type", "integer"}, {"description", "Maximum number of raw bytes to return (hard cap 3 MiB)."}}}
        }},
        {"required", Json::array({"path"})}
    };
    server.register_tool(read_file_base64_def, mcp::tool_read_file_base64);

    mcp::ToolDefinition get_file_info_def{};
    get_file_info_def.name = "get_file_info";
    get_file_info_def.description = "Returns existence, type, and size information for a path without reading its content.";
    get_file_info_def.input_schema = Json{
        {"type", "object"},
        {"properties", {{"path", path_prop}}},
        {"required", Json::array({"path"})}
    };
    server.register_tool(get_file_info_def, mcp::tool_get_file_info);

    mcp::ToolDefinition hash_file_def{};
    hash_file_def.name = "hash_file";
    hash_file_def.description = "Streams the whole file through FNV-1a 64 and returns the hash as 16 hex digits plus the byte count.";
    hash_file_def.input_schema = Json{
        {"type", "object"},
        {"properties", {{"path", path_prop}}},
        {"required", Json::array({"path"})}
    };
    server.register_tool(hash_file_def, mcp::tool_hash_file);

    mcp::ToolDefinition search_in_file_def{};
    search_in_file_def.name = "search_in_file";
    search_in_file_def.description = "Scans a file line by line with an ECMAScript regex and returns capped, numbered matches with snippets.";
    search_in_file_def.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"path", path_prop},
            {"pattern", {{"type", "string"}, {"description", "ECMAScript regex; patterns that can match an empty string are rejected."}}},
            {"max_matches", {{"type", "integer"}, {"description", "Maximum matches to report (default 100, hard cap 1000)."}}}
        }},
        {"required", Json::array({"path", "pattern"})}
    };
    server.register_tool(search_in_file_def, mcp::tool_search_in_file);

    mcp::ToolDefinition write_file_def{};
    write_file_def.name = "write_file";
    write_file_def.description = "Writes text content to a file, creating or truncating it, or appending when requested.";
    write_file_def.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"path", path_prop},
            {"content", {{"type", "string"}, {"description", "Text content to write (hard limit 8 MiB)."}}},
            {"append", {{"type", "boolean"}, {"description", "Append to the file instead of truncating (default false)."}}}
        }},
        {"required", Json::array({"path", "content"})}
    };
    server.register_tool(write_file_def, mcp::tool_write_file);

    mcp::ToolDefinition replace_in_file_def{};
    replace_in_file_def.name = "replace_in_file";
    replace_in_file_def.description = "Rewrites a file applying an ECMAScript regex with literal replacements; '$' in the replacement has no special meaning.";
    replace_in_file_def.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"path", path_prop},
            {"pattern", {{"type", "string"}, {"description", "ECMAScript regex; patterns that can match an empty string are rejected."}}},
            {"replacement", {{"type", "string"}, {"description", "Inserted literally for each match; no $ expansion is performed."}}},
            {"max_replacements", {{"type", "integer"}, {"description", "Maximum replacements to apply; negative means unlimited (default -1)."}}}
        }},
        {"required", Json::array({"path", "pattern", "replacement"})}
    };
    server.register_tool(replace_in_file_def, mcp::tool_replace_in_file);

    mcp::ToolDefinition truncate_file_def{};
    truncate_file_def.name = "truncate_file";
    truncate_file_def.description = "Resizes an existing file to an exact byte size using cross-platform truncate syscalls.";
    truncate_file_def.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"path", path_prop},
            {"size", {{"type", "integer"}, {"description", "New size in bytes; 0 empties the file."}}}
        }},
        {"required", Json::array({"path", "size"})}
    };
    server.register_tool(truncate_file_def, mcp::tool_truncate_file);

    mcp::ToolDefinition move_file_def{};
    move_file_def.name = "move_file";
    move_file_def.description = "Moves a regular file with rename, falling back to copy plus remove across device boundaries.";
    move_file_def.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"source", {{"type", "string"}, {"description", "Path of the file to move."}}},
            {"destination", {{"type", "string"}, {"description", "Target path of the move."}}},
            {"overwrite", {{"type", "boolean"}, {"description", "Overwrite the destination if it exists (default false)."}}}
        }},
        {"required", Json::array({"source", "destination"})}
    };
    server.register_tool(move_file_def, mcp::tool_move_file);

    mcp::ToolDefinition delete_file_def{};
    delete_file_def.name = "delete_file";
    delete_file_def.description = "Deletes a single regular file; directories are refused and must go through the directory server.";
    delete_file_def.input_schema = Json{
        {"type", "object"},
        {"properties", {{"path", path_prop}}},
        {"required", Json::array({"path"})}
    };
    server.register_tool(delete_file_def, mcp::tool_delete_file);

    return server.run_cli(argc, argv, 8093);
}