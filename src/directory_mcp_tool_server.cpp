// ============================================================================
//  src/directory_mcp_tool_server.cpp — MCP Directory Operations Server
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Provides directory creation, listing, recursive iteration, deletion, and
//  last-modified-file lookup. Runs over stdio by default or Streamable HTTP
//  on port 8094 by default; operations access the host filesystem.
// ============================================================================

#include <iostream>
#include <string>
#include <string_view>
#include <filesystem>
#include <system_error>
#include "nlohmann/json.hpp"
#include "pooriayousefi/utility/common.hpp"
#include "pooriayousefi/utility/expected.hpp"
#include "pooriayousefi/mcp/protocol.hpp"
#include "pooriayousefi/mcp/server.hpp"

namespace pooriayousefi::mcp
{
    [[nodiscard]] Expected<std::string, std::string> require_string_arg(const Json& args, std::string_view key)
    {
        Expected<std::string, std::string> result_val{};

        if (!args.contains(key.data()) || !args[key.data()].is_string())
        {
            result_val.set_error(std::string("Missing or non-string '") + key.data() + "' in arguments");
        }
        else
        {
            result_val.set_value(args[key.data()].get<std::string>());
        }

        return result_val;
    }

    [[nodiscard]] Json path_argument_schema(std::string_view description)
    {
        Json schema = Json::object();
        schema["type"] = "object";

        Json properties = Json::object();
        Json path_property = Json::object();
        path_property["type"] = "string";
        path_property["description"] = std::string(description);
        properties["path"] = std::move(path_property);

        schema["properties"] = std::move(properties);
        schema["required"] = Json::array({"path"});

        return schema;
    }

    [[nodiscard]] ToolCallResult tool_create_directory(const Json& args)
    {
        ToolCallResult result{};

        auto path_res = require_string_arg(args, "path");
        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            const std::string& path = path_res.get_value();
            std::error_code ec{};
            namespace fs = std::filesystem;
            fs::create_directories(path, ec);

            if (ec)
            {
                result.is_error = true;
                result.content = "Failed to create directory: " + ec.message();
            }
            else
            {
                Json resp = Json::object();
                resp["message"] = "Directory created successfully (or already existed)";
                resp["path"] = path;
                result.is_error = false;
                result.content = resp.dump();
            }
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_list_directory(const Json& args)
    {
        ToolCallResult result{};

        auto path_res = require_string_arg(args, "path");
        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            const std::string& path = path_res.get_value();
            std::error_code ec{};
            namespace fs = std::filesystem;

            if (!fs::is_directory(path, ec) || ec)
            {
                result.is_error = true;
                result.content = "Directory not found: " + path;
            }
            else
            {
                Json content = Json::array();
                fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);

                if (ec)
                {
                    result.is_error = true;
                    result.content = "Failed to open directory: " + ec.message();
                }
                else
                {
                    for (const auto& entry : it)
                    {
                        std::error_code entry_ec{};
                        bool is_dir = entry.is_directory(entry_ec);
                        std::uintmax_t size{0};

                        if (!is_dir)
                        {
                            size = entry.file_size(entry_ec);
                            if (entry_ec)
                            {
                                size = 0;
                            }
                        }

                        Json entry_obj = Json::object();
                        entry_obj["name"] = entry.path().filename().string();
                        entry_obj["is_directory"] = is_dir;
                        entry_obj["size"] = size;
                        content.push_back(std::move(entry_obj));
                    }

                    Json resp = Json::object();
                    resp["path"] = path;
                    resp["entries"] = std::move(content);
                    result.is_error = false;
                    result.content = resp.dump();
                }
            }
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_iterate_directories_recursively(const Json& args)
    {
        ToolCallResult result{};
        constexpr std::size_t MAX_RECURSIVE_ENTRIES = 50'000;

        auto path_res = require_string_arg(args, "path");
        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            const std::string& path = path_res.get_value();
            std::error_code ec{};
            namespace fs = std::filesystem;

            if (!fs::is_directory(path, ec) || ec)
            {
                result.is_error = true;
                result.content = "Directory not found: " + path;
            }
            else
            {
                Json content = Json::array();
                bool truncated{false};

                fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
                fs::recursive_directory_iterator end{};

                if (ec)
                {
                    result.is_error = true;
                    result.content = "Failed to open directory: " + ec.message();
                }
                else
                {
                    for (; it != end; it.increment(ec))
                    {
                        if (ec)
                        {
                            break;
                        }

                        if (content.size() >= MAX_RECURSIVE_ENTRIES)
                        {
                            truncated = true;
                            break;
                        }

                        content.push_back(it->path().string());
                    }

                    if (ec)
                    {
                        result.is_error = true;
                        result.content = "Failed while iterating directory: " + ec.message();
                    }
                    else
                    {
                        Json resp = Json::object();
                        resp["path"] = path;
                        resp["entries"] = std::move(content);
                        resp["truncated"] = truncated;
                        resp["max_entries"] = MAX_RECURSIVE_ENTRIES;
                        result.is_error = false;
                        result.content = resp.dump();
                    }
                }
            }
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_delete_directory(const Json& args)
    {
        ToolCallResult result{};

        auto path_res = require_string_arg(args, "path");
        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            const std::string& path = path_res.get_value();
            std::error_code ec{};
            namespace fs = std::filesystem;

            if (!fs::exists(path, ec))
            {
                result.is_error = true;
                result.content = "Directory not found: " + path;
            }
            else
            {
                auto removed_count = fs::remove_all(path, ec);

                if (ec)
                {
                    result.is_error = true;
                    result.content = "Failed to delete directory: " + ec.message();
                }
                else
                {
                    Json resp = Json::object();
                    resp["message"] = "Directory deleted successfully";
                    resp["path"] = path;
                    resp["removed_count"] = static_cast<std::uint64_t>(removed_count);
                    result.is_error = false;
                    result.content = resp.dump();
                }
            }
        }

        return result;
    }

    [[nodiscard]] ToolCallResult tool_get_last_modified_file(const Json& args)
    {
        ToolCallResult result{};

        auto path_res = require_string_arg(args, "path");
        if (path_res.has_error())
        {
            result.is_error = true;
            result.content = path_res.get_error();
        }
        else
        {
            const std::string& path = path_res.get_value();
            std::error_code ec{};
            namespace fs = std::filesystem;

            if (!fs::is_directory(path, ec) || ec)
            {
                result.is_error = true;
                result.content = "Directory not found: " + path;
            }
            else
            {
                fs::path latest_file{};
                fs::file_time_type latest_time{};
                bool found{false};

                fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
                fs::recursive_directory_iterator end{};

                if (ec)
                {
                    result.is_error = true;
                    result.content = "Failed to open directory: " + ec.message();
                }
                else
                {
                    for (; it != end; it.increment(ec))
                    {
                        if (ec)
                        {
                            break;
                        }

                        std::error_code file_ec{};
                        if (it->is_regular_file(file_ec) && !file_ec)
                        {
                            std::error_code time_ec{};
                            auto ftime = it->last_write_time(time_ec);

                            if (!time_ec && (!found || ftime > latest_time))
                            {
                                latest_time = ftime;
                                latest_file = it->path();
                                found = true;
                            }
                        }
                    }

                    if (ec)
                    {
                        result.is_error = true;
                        result.content = "Failed while iterating directory: " + ec.message();
                    }
                    else if (!found)
                    {
                        result.is_error = true;
                        result.content = "No files found in directory: " + path;
                    }
                    else
                    {
                        Json resp = Json::object();
                        resp["path"] = path;
                        resp["last_modified_file"] = latest_file.string();
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

    mcp::ToolDefinition create_dir_def{};
    create_dir_def.name = "create_directory";
    create_dir_def.description = "Creates a directory at the given absolute path. If it already exists, does nothing.";
    create_dir_def.input_schema = mcp::path_argument_schema("The absolute path of the directory to create.");
    server.register_tool(create_dir_def, mcp::tool_create_directory);

    mcp::ToolDefinition list_dir_def{};
    list_dir_def.name = "list_directory";
    list_dir_def.description = "Lists the immediate files and folders inside a given directory (non-recursive).";
    list_dir_def.input_schema = mcp::path_argument_schema("The absolute path of the directory to list.");
    server.register_tool(list_dir_def, mcp::tool_list_directory);

    mcp::ToolDefinition iterate_dir_def{};
    iterate_dir_def.name = "iterate_directories_recursively";
    iterate_dir_def.description = "Iterates a given directory recursively and returns a flat list of all files and folders inside it.";
    iterate_dir_def.input_schema = mcp::path_argument_schema("The absolute path of the directory which has to be iterated.");
    server.register_tool(iterate_dir_def, mcp::tool_iterate_directories_recursively);

    mcp::ToolDefinition delete_dir_def{};
    delete_dir_def.name = "delete_directory";
    delete_dir_def.description = "Deletes a directory and all of its contents recursively. Use with caution.";
    delete_dir_def.input_schema = mcp::path_argument_schema("The absolute path of the directory to delete.");
    server.register_tool(delete_dir_def, mcp::tool_delete_directory);

    mcp::ToolDefinition last_mod_def{};
    last_mod_def.name = "get_last_modified_file";
    last_mod_def.description = "Finds and returns the absolute path of the most recently modified file in a given directory.";
    last_mod_def.input_schema = mcp::path_argument_schema("The absolute path of the directory to search.");
    server.register_tool(last_mod_def, mcp::tool_get_last_modified_file);

    return server.run_cli(argc, argv, 8094);
}