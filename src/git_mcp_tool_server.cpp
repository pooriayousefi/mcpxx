// ============================================================================
//  src/git_mcp_tool_server.cpp — MCP Git Operations Server
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Provides repository inspection, mutation, and remote-operation tools by
//  invoking git with explicit process arguments. Runs over stdio by default
//  or Streamable HTTP on port 8099 by default; it operates on local repos and
//  can contact remotes for network-related tools.
// ============================================================================

#include "pooriayousefi/utility/common.hpp"
#include "pooriayousefi/mcp/server.hpp"
#include "pooriayousefi/process/process.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

using namespace pooriayousefi::process;
using pooriayousefi::Json;

namespace pooriayousefi::mcp
{
    bool is_blank(const std::string& text)
    {
        bool result = true;

        for (char character : text)
        {
            if (!std::isspace(static_cast<unsigned char>(character)))
            {
                result = false;
                break;
            }
        }

        return result;
    }

    bool contains_substring(const std::string& haystack, const std::string& needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    Expected<std::string, Error> require_string_arg(const Json& args, const char* key)
    {
        Expected<std::string, Error> result{};

        if (!args.contains(key) || !args[key].is_string())
        {
            result.set_error(Error{
                error_code::INVALID_PARAMS,
                std::string("Missing or non-string '") + key + "' in arguments"
            });
        }
        else
        {
            result.set_value(args[key].get<std::string>());
        }

        return result;
    }

    Expected<std::vector<std::string>, Error> require_string_array_arg(
        const Json& args,
        const char* key
    )
    {
        Expected<std::vector<std::string>, Error> result{};

        if (!args.contains(key) || !args[key].is_array())
        {
            result.set_error(Error{
                error_code::INVALID_PARAMS,
                std::string("Missing or non-array '") + key + "' in arguments"
            });
        }
        else
        {
            const auto& arr = args[key];
            bool all_strings = std::all_of(
                arr.begin(),
                arr.end(),
                [](const Json& value)
                {
                    return value.is_string();
                }
            );

            if (!all_strings)
            {
                result.set_error(Error{
                    error_code::INVALID_PARAMS,
                    std::string("Array '") + key + "' contains non-string values"
                });
            }
            else
            {
                std::vector<std::string> out;
                out.reserve(arr.size());

                for (const auto& value : arr)
                {
                    out.push_back(value.get<std::string>());
                }

                result.set_value(std::move(out));
            }
        }

        return result;
    }

    // Optional "cwd": prefix argv with -C <path>
    std::vector<std::string> with_optional_cwd(
        const Json& args,
        std::vector<std::string> git_args
    )
    {
        std::vector<std::string> result = std::move(git_args);

        if (args.contains("cwd") && args["cwd"].is_string())
        {
            const std::string cwd = args["cwd"].get<std::string>();

            if (!cwd.empty())
            {
                result.insert(result.begin(), {"-C", cwd});
            }
        }

        return result;
    }

    Expected<std::string, Error> run_git_command(const std::vector<std::string>& args)
    {
        Expected<std::string, Error> result{};

        try
        {
            Process process;
            process.start("git", args, true);
            process.close_stdin();
            std::string output = process.read_all_stdout();
            process.wait();
            result.set_value(std::move(output));
        }
        catch (const std::exception& failure)
        {
            result.set_error(Error{
                error_code::INTERNAL_ERROR,
                std::string("git process failed: ") + failure.what()
            });
        }

        return result;
    }

    Expected<Json, Error> ok_output(std::string output, Json extra = Json::object())
    {
        Expected<Json, Error> result{};
        extra["output"] = std::move(output);
        result.set_value(std::move(extra));
        return result;
    }

    // ------------------------------------------------------------------ tools

    Expected<Json, Error> tool_git_init(const Json& args)
    {
        Expected<Json, Error> result{};

        std::vector<std::string> git_args = {"init"};

        if (args.contains("bare") && args["bare"].is_boolean() && args["bare"].get<bool>())
        {
            git_args.push_back("--bare");
        }

        if (args.contains("path") && args["path"].is_string())
        {
            const std::string path = args["path"].get<std::string>();

            if (!path.empty())
            {
                git_args.push_back(path);
            }
        }

        // Prefer explicit path; also allow cwd for consistency
        auto output_or_error = run_git_command(with_optional_cwd(args, std::move(git_args)));

        if (!output_or_error.has_value())
        {
            result.set_error(output_or_error.get_error());
        }
        else
        {
            result = ok_output(output_or_error.get_value());
        }

        return result;
    }

    Expected<Json, Error> tool_git_status(const Json& args)
    {
        Expected<Json, Error> result{};

        auto output_or_error = run_git_command(
            with_optional_cwd(args, {"status", "--porcelain"})
        );

        if (!output_or_error.has_value())
        {
            result.set_error(output_or_error.get_error());
        }
        else
        {
            result = ok_output(output_or_error.get_value());
        }

        return result;
    }

    Expected<Json, Error> tool_git_diff(const Json& args)
    {
        Expected<Json, Error> result{};

        bool staged = false;

        if (args.contains("staged") && args["staged"].is_boolean())
        {
            staged = args["staged"].get<bool>();
        }

        std::vector<std::string> git_args;

        if (staged)
        {
            git_args = {"diff", "--staged"};
        }
        else
        {
            git_args = {"diff"};
        }

        auto output_or_error = run_git_command(with_optional_cwd(args, std::move(git_args)));

        if (!output_or_error.has_value())
        {
            result.set_error(output_or_error.get_error());
        }
        else
        {
            result = ok_output(output_or_error.get_value(), Json{{"staged", staged}});
        }

        return result;
    }

    Expected<Json, Error> tool_git_log(const Json& args)
    {
        Expected<Json, Error> result{};

        int limit = 10;

        if (args.contains("limit") && args["limit"].is_number())
        {
            int raw = args["limit"].get<int>();

            if (raw > 0)
            {
                limit = raw;
            }
        }

        auto output_or_error = run_git_command(
            with_optional_cwd(args, {"log", "--oneline", "-n", std::to_string(limit)})
        );

        if (!output_or_error.has_value())
        {
            result.set_error(output_or_error.get_error());
        }
        else
        {
            result = ok_output(output_or_error.get_value(), Json{{"limit", limit}});
        }

        return result;
    }

    Expected<Json, Error> tool_git_show(const Json& args)
    {
        Expected<Json, Error> result{};

        std::vector<std::string> git_args = {"show"};

        if (args.contains("object") && args["object"].is_string())
        {
            const std::string object = args["object"].get<std::string>();

            if (!object.empty())
            {
                git_args.push_back(object);
            }
        }

        auto output_or_error = run_git_command(with_optional_cwd(args, std::move(git_args)));

        if (!output_or_error.has_value())
        {
            result.set_error(output_or_error.get_error());
        }
        else
        {
            result = ok_output(output_or_error.get_value());
        }

        return result;
    }

    Expected<Json, Error> tool_git_add(const Json& args)
    {
        Expected<Json, Error> result{};

        auto files_or_error = require_string_array_arg(args, "files");

        if (!files_or_error.has_value())
        {
            result.set_error(files_or_error.get_error());
        }
        else if (files_or_error.get_value().empty())
        {
            result.set_error(Error{
                error_code::INVALID_PARAMS,
                "Missing or empty 'files' array in arguments."
            });
        }
        else
        {
            std::vector<std::string> git_args{"add"};

            for (const auto& file : files_or_error.get_value())
            {
                git_args.push_back(file);
            }

            auto output_or_error = run_git_command(with_optional_cwd(args, std::move(git_args)));

            if (!output_or_error.has_value())
            {
                result.set_error(output_or_error.get_error());
            }
            else if (is_blank(output_or_error.get_value()))
            {
                result.set_value(Json{{"message", "Files staged successfully."}});
            }
            else
            {
                result = ok_output(output_or_error.get_value());
            }
        }

        return result;
    }

    Expected<Json, Error> tool_git_commit(const Json& args)
    {
        Expected<Json, Error> result{};

        auto message_or_error = require_string_arg(args, "message");

        if (!message_or_error.has_value())
        {
            result.set_error(message_or_error.get_error());
        }
        else
        {
            auto output_or_error = run_git_command(
                with_optional_cwd(args, {"commit", "-m", message_or_error.get_value()})
            );

            if (!output_or_error.has_value())
            {
                result.set_error(output_or_error.get_error());
            }
            else
            {
                const std::string& output = output_or_error.get_value();

                if (contains_substring(output, "nothing to commit") ||
                    contains_substring(output, "no changes added"))
                {
                    result.set_error(Error{error_code::INVALID_PARAMS, output});
                }
                else
                {
                    result = ok_output(output);
                }
            }
        }

        return result;
    }

    Expected<Json, Error> tool_git_branch(const Json& args)
    {
        Expected<Json, Error> result{};

        auto output_or_error = run_git_command(
            with_optional_cwd(args, {"branch", "-a"})
        );

        if (!output_or_error.has_value())
        {
            result.set_error(output_or_error.get_error());
        }
        else
        {
            result = ok_output(output_or_error.get_value());
        }

        return result;
    }

    Expected<Json, Error> tool_git_current_branch(const Json& args)
    {
        Expected<Json, Error> result{};

        auto output_or_error = run_git_command(
            with_optional_cwd(args, {"rev-parse", "--abbrev-ref", "HEAD"})
        );

        if (!output_or_error.has_value())
        {
            result.set_error(output_or_error.get_error());
        }
        else
        {
            std::string branch = output_or_error.get_value();

            while (!branch.empty() &&
                   (branch.back() == '\n' || branch.back() == '\r'))
            {
                branch.pop_back();
            }

            result.set_value(Json{{"branch", branch}});
        }

        return result;
    }

    Expected<Json, Error> tool_git_checkout(const Json& args)
    {
        Expected<Json, Error> result{};

        auto branch_or_error = require_string_arg(args, "branch");

        if (!branch_or_error.has_value())
        {
            result.set_error(branch_or_error.get_error());
        }
        else
        {
            auto output_or_error = run_git_command(
                with_optional_cwd(args, {"checkout", branch_or_error.get_value()})
            );

            if (!output_or_error.has_value())
            {
                result.set_error(output_or_error.get_error());
            }
            else
            {
                const std::string& output = output_or_error.get_value();

                if (contains_substring(output, "error") ||
                    contains_substring(output, "did not match"))
                {
                    result.set_error(Error{error_code::INVALID_PARAMS, output});
                }
                else
                {
                    result = ok_output(
                        output,
                        Json{{"branch", branch_or_error.get_value()}}
                    );
                }
            }
        }

        return result;
    }

    Expected<Json, Error> tool_git_create_branch(const Json& args)
    {
        Expected<Json, Error> result{};

        auto name_or_error = require_string_arg(args, "name");

        if (!name_or_error.has_value())
        {
            result.set_error(name_or_error.get_error());
        }
        else
        {
            bool checkout = true;

            if (args.contains("checkout") && args["checkout"].is_boolean())
            {
                checkout = args["checkout"].get<bool>();
            }

            std::vector<std::string> git_args;

            if (checkout)
            {
                git_args = {"checkout", "-b", name_or_error.get_value()};
            }
            else
            {
                git_args = {"branch", name_or_error.get_value()};
            }

            auto output_or_error = run_git_command(with_optional_cwd(args, std::move(git_args)));

            if (!output_or_error.has_value())
            {
                result.set_error(output_or_error.get_error());
            }
            else
            {
                result = ok_output(
                    output_or_error.get_value(),
                    Json{
                        {"name", name_or_error.get_value()},
                        {"checkout", checkout}
                    }
                );
            }
        }

        return result;
    }

    Expected<Json, Error> tool_git_reset(const Json& args)
    {
        Expected<Json, Error> result{};

        // mode: soft | mixed | hard  (default mixed)
        std::string mode = "mixed";

        if (args.contains("mode") && args["mode"].is_string())
        {
            mode = args["mode"].get<std::string>();
        }

        if (mode != "soft" && mode != "mixed" && mode != "hard")
        {
            result.set_error(Error{
                error_code::INVALID_PARAMS,
                "mode must be one of: soft, mixed, hard"
            });
        }
        else
        {
            std::vector<std::string> git_args = {"reset", std::string("--") + mode};

            if (args.contains("target") && args["target"].is_string())
            {
                const std::string target = args["target"].get<std::string>();

                if (!target.empty())
                {
                    git_args.push_back(target);
                }
            }

            auto output_or_error = run_git_command(with_optional_cwd(args, std::move(git_args)));

            if (!output_or_error.has_value())
            {
                result.set_error(output_or_error.get_error());
            }
            else
            {
                result = ok_output(output_or_error.get_value(), Json{{"mode", mode}});
            }
        }

        return result;
    }

    Expected<Json, Error> tool_git_stash(const Json& args)
    {
        Expected<Json, Error> result{};

        // action: push | list | pop | apply | drop  (default list)
        std::string action = "list";

        if (args.contains("action") && args["action"].is_string())
        {
            action = args["action"].get<std::string>();
        }

        std::vector<std::string> git_args = {"stash"};

        if (action == "list")
        {
            git_args.push_back("list");
        }
        else if (action == "push")
        {
            git_args.push_back("push");

            if (args.contains("message") && args["message"].is_string())
            {
                git_args.push_back("-m");
                git_args.push_back(args["message"].get<std::string>());
            }
        }
        else if (action == "pop" || action == "apply" || action == "drop")
        {
            git_args.push_back(action);

            if (args.contains("ref") && args["ref"].is_string())
            {
                git_args.push_back(args["ref"].get<std::string>());
            }
        }
        else
        {
            result.set_error(Error{
                error_code::INVALID_PARAMS,
                "action must be one of: push, list, pop, apply, drop"
            });
        }

        if (!result.has_error())
        {
            auto output_or_error = run_git_command(with_optional_cwd(args, std::move(git_args)));

            if (!output_or_error.has_value())
            {
                result.set_error(output_or_error.get_error());
            }
            else
            {
                result = ok_output(output_or_error.get_value(), Json{{"action", action}});
            }
        }

        return result;
    }

    Expected<Json, Error> tool_git_remote(const Json& args)
    {
        Expected<Json, Error> result{};

        auto output_or_error = run_git_command(
            with_optional_cwd(args, {"remote", "-v"})
        );

        if (!output_or_error.has_value())
        {
            result.set_error(output_or_error.get_error());
        }
        else
        {
            result = ok_output(output_or_error.get_value());
        }

        return result;
    }

    Expected<Json, Error> tool_git_fetch(const Json& args)
    {
        Expected<Json, Error> result{};

        std::vector<std::string> git_args = {"fetch"};

        if (args.contains("remote") && args["remote"].is_string())
        {
            const std::string remote = args["remote"].get<std::string>();

            if (!remote.empty())
            {
                git_args.push_back(remote);
            }
        }

        if (args.contains("all") && args["all"].is_boolean() && args["all"].get<bool>())
        {
            git_args.push_back("--all");
        }

        auto output_or_error = run_git_command(with_optional_cwd(args, std::move(git_args)));

        if (!output_or_error.has_value())
        {
            result.set_error(output_or_error.get_error());
        }
        else
        {
            result = ok_output(output_or_error.get_value());
        }

        return result;
    }

    Expected<Json, Error> tool_git_pull(const Json& args)
    {
        Expected<Json, Error> result{};

        std::vector<std::string> git_args = {"pull"};

        if (args.contains("remote") && args["remote"].is_string())
        {
            git_args.push_back(args["remote"].get<std::string>());
        }

        if (args.contains("branch") && args["branch"].is_string())
        {
            git_args.push_back(args["branch"].get<std::string>());
        }

        auto output_or_error = run_git_command(with_optional_cwd(args, std::move(git_args)));

        if (!output_or_error.has_value())
        {
            result.set_error(output_or_error.get_error());
        }
        else
        {
            result = ok_output(output_or_error.get_value());
        }

        return result;
    }

    Expected<Json, Error> tool_git_push(const Json& args)
    {
        Expected<Json, Error> result{};

        std::vector<std::string> git_args = {"push"};

        if (args.contains("remote") && args["remote"].is_string())
        {
            git_args.push_back(args["remote"].get<std::string>());
        }

        if (args.contains("branch") && args["branch"].is_string())
        {
            git_args.push_back(args["branch"].get<std::string>());
        }

        if (args.contains("set_upstream") && args["set_upstream"].is_boolean() &&
            args["set_upstream"].get<bool>())
        {
            git_args.insert(git_args.begin() + 1, "-u");
        }

        auto output_or_error = run_git_command(with_optional_cwd(args, std::move(git_args)));

        if (!output_or_error.has_value())
        {
            result.set_error(output_or_error.get_error());
        }
        else
        {
            result = ok_output(output_or_error.get_value());
        }

        return result;
    }

    Expected<Json, Error> tool_git_clone(const Json& args)
    {
        Expected<Json, Error> result{};

        auto url_or_error = require_string_arg(args, "url");

        if (!url_or_error.has_value())
        {
            result.set_error(url_or_error.get_error());
        }
        else
        {
            std::vector<std::string> git_args = {"clone", url_or_error.get_value()};

            if (args.contains("directory") && args["directory"].is_string())
            {
                const std::string directory = args["directory"].get<std::string>();

                if (!directory.empty())
                {
                    git_args.push_back(directory);
                }
            }

            if (args.contains("depth") && args["depth"].is_number())
            {
                int depth = args["depth"].get<int>();

                if (depth > 0)
                {
                    git_args.push_back("--depth");
                    git_args.push_back(std::to_string(depth));
                }
            }

            auto output_or_error = run_git_command(std::move(git_args));

            if (!output_or_error.has_value())
            {
                result.set_error(output_or_error.get_error());
            }
            else
            {
                result = ok_output(
                    output_or_error.get_value(),
                    Json{{"url", url_or_error.get_value()}}
                );
            }
        }

        return result;
    }
}

int main(int argc, char* argv[])
{
    using namespace pooriayousefi::mcp;

    Server server{"mcp-tool-git-server", "1.0.0"};
    server.set_instructions("Git tools: inspect (status, diff, log, show, branches, remotes) and mutate (init, add, commit, checkout, reset, stash) plus network operations (fetch, pull, push, clone). Every tool except git_clone accepts an optional 'cwd' repository path passed as git -C. The git executable must be on PATH.");

    const Json cwd_prop = {
        {"type", "string"},
        {"description", "Optional repository path (passed as git -C <cwd>)."}
    };

    Tool git_init{};
    git_init.name = "git_init";
    git_init.title = "Git init";
    git_init.description = "Initialize a new git repository.";
    git_init.input_schema = Json::parse(R"({
        "type": "object",
        "properties": {
            "path": { "type": "string", "description": "Directory to initialize (optional)." },
            "bare": { "type": "boolean", "description": "Create a bare repository." },
            "cwd": { "type": "string", "description": "Optional path for git -C." }
        },
        "required": []
    })");
    git_init.handler = tool_git_init;
    server.add_tool(std::move(git_init));

    Tool git_status{};
    git_status.name = "git_status";
    git_status.title = "Git status";
    git_status.description = "Gets the current working tree status (porcelain format).";
    git_status.input_schema = Json{
        {"type", "object"},
        {"properties", {{"cwd", cwd_prop}}},
        {"required", Json::array()}
    };
    git_status.handler = tool_git_status;
    server.add_tool(std::move(git_status));

    Tool git_diff{};
    git_diff.name = "git_diff";
    git_diff.title = "Git diff";
    git_diff.description = "Shows unstaged or staged changes.";
    git_diff.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"staged", {{"type", "boolean"}, {"description", "If true, show staged changes."}}},
            {"cwd", cwd_prop}
        }},
        {"required", Json::array()}
    };
    git_diff.handler = tool_git_diff;
    server.add_tool(std::move(git_diff));

    Tool git_log{};
    git_log.name = "git_log";
    git_log.title = "Git log";
    git_log.description = "Views commit history (oneline).";
    git_log.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"limit", {{"type", "integer"}, {"description", "Max commits (default 10)."}}},
            {"cwd", cwd_prop}
        }},
        {"required", Json::array()}
    };
    git_log.handler = tool_git_log;
    server.add_tool(std::move(git_log));

    Tool git_show{};
    git_show.name = "git_show";
    git_show.title = "Git show";
    git_show.description = "Shows a commit or object (git show).";
    git_show.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"object", {{"type", "string"}, {"description", "Commit/object to show (optional)."}}},
            {"cwd", cwd_prop}
        }},
        {"required", Json::array()}
    };
    git_show.handler = tool_git_show;
    server.add_tool(std::move(git_show));

    Tool git_add{};
    git_add.name = "git_add";
    git_add.title = "Git add";
    git_add.description = "Stages files for commit.";
    git_add.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"files", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "File paths to stage."}
            }},
            {"cwd", cwd_prop}
        }},
        {"required", Json::array({"files"})}
    };
    git_add.handler = tool_git_add;
    server.add_tool(std::move(git_add));

    Tool git_commit{};
    git_commit.name = "git_commit";
    git_commit.title = "Git commit";
    git_commit.description = "Commits staged changes with a message.";
    git_commit.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"message", {{"type", "string"}, {"description", "Commit message."}}},
            {"cwd", cwd_prop}
        }},
        {"required", Json::array({"message"})}
    };
    git_commit.handler = tool_git_commit;
    server.add_tool(std::move(git_commit));

    Tool git_branch{};
    git_branch.name = "git_branch";
    git_branch.title = "Git branch";
    git_branch.description = "Lists all local and remote branches.";
    git_branch.input_schema = Json{
        {"type", "object"},
        {"properties", {{"cwd", cwd_prop}}},
        {"required", Json::array()}
    };
    git_branch.handler = tool_git_branch;
    server.add_tool(std::move(git_branch));

    Tool git_current_branch{};
    git_current_branch.name = "git_current_branch";
    git_current_branch.title = "Git current branch";
    git_current_branch.description = "Returns the current branch name.";
    git_current_branch.input_schema = Json{
        {"type", "object"},
        {"properties", {{"cwd", cwd_prop}}},
        {"required", Json::array()}
    };
    git_current_branch.handler = tool_git_current_branch;
    server.add_tool(std::move(git_current_branch));

    Tool git_checkout{};
    git_checkout.name = "git_checkout";
    git_checkout.title = "Git checkout";
    git_checkout.description = "Switches to a specified branch.";
    git_checkout.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"branch", {{"type", "string"}, {"description", "Branch name."}}},
            {"cwd", cwd_prop}
        }},
        {"required", Json::array({"branch"})}
    };
    git_checkout.handler = tool_git_checkout;
    server.add_tool(std::move(git_checkout));

    Tool git_create_branch{};
    git_create_branch.name = "git_create_branch";
    git_create_branch.title = "Git create branch";
    git_create_branch.description = "Creates a branch; optionally checks it out (-b).";
    git_create_branch.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"name", {{"type", "string"}, {"description", "New branch name."}}},
            {"checkout", {{"type", "boolean"}, {"description", "Checkout after create (default true)."}}},
            {"cwd", cwd_prop}
        }},
        {"required", Json::array({"name"})}
    };
    git_create_branch.handler = tool_git_create_branch;
    server.add_tool(std::move(git_create_branch));

    Tool git_reset{};
    git_reset.name = "git_reset";
    git_reset.title = "Git reset";
    git_reset.description = "Resets HEAD (soft | mixed | hard). Use hard with caution.";
    git_reset.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"mode", {{"type", "string"}, {"description", "soft, mixed, or hard (default mixed)."}}},
            {"target", {{"type", "string"}, {"description", "Optional commit/ref (default HEAD)."}}},
            {"cwd", cwd_prop}
        }},
        {"required", Json::array()}
    };
    git_reset.handler = tool_git_reset;
    server.add_tool(std::move(git_reset));

    Tool git_stash{};
    git_stash.name = "git_stash";
    git_stash.title = "Git stash";
    git_stash.description = "Stash operations: push, list, pop, apply, drop.";
    git_stash.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"action", {{"type", "string"}, {"description", "push|list|pop|apply|drop (default list)."}}},
            {"message", {{"type", "string"}, {"description", "Message for stash push."}}},
            {"ref", {{"type", "string"}, {"description", "Stash ref for pop/apply/drop."}}},
            {"cwd", cwd_prop}
        }},
        {"required", Json::array()}
    };
    git_stash.handler = tool_git_stash;
    server.add_tool(std::move(git_stash));

    Tool git_remote{};
    git_remote.name = "git_remote";
    git_remote.title = "Git remote";
    git_remote.description = "Lists remotes (git remote -v).";
    git_remote.input_schema = Json{
        {"type", "object"},
        {"properties", {{"cwd", cwd_prop}}},
        {"required", Json::array()}
    };
    git_remote.handler = tool_git_remote;
    server.add_tool(std::move(git_remote));

    Tool git_fetch{};
    git_fetch.name = "git_fetch";
    git_fetch.title = "Git fetch";
    git_fetch.description = "Fetches from remotes.";
    git_fetch.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"remote", {{"type", "string"}}},
            {"all", {{"type", "boolean"}}},
            {"cwd", cwd_prop}
        }},
        {"required", Json::array()}
    };
    git_fetch.handler = tool_git_fetch;
    server.add_tool(std::move(git_fetch));

    Tool git_pull{};
    git_pull.name = "git_pull";
    git_pull.title = "Git pull";
    git_pull.description = "Pulls from a remote branch.";
    git_pull.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"remote", {{"type", "string"}}},
            {"branch", {{"type", "string"}}},
            {"cwd", cwd_prop}
        }},
        {"required", Json::array()}
    };
    git_pull.handler = tool_git_pull;
    server.add_tool(std::move(git_pull));

    Tool git_push{};
    git_push.name = "git_push";
    git_push.title = "Git push";
    git_push.description = "Pushes to a remote branch.";
    git_push.input_schema = Json{
        {"type", "object"},
        {"properties", {
            {"remote", {{"type", "string"}}},
            {"branch", {{"type", "string"}}},
            {"set_upstream", {{"type", "boolean"}}},
            {"cwd", cwd_prop}
        }},
        {"required", Json::array()}
    };
    git_push.handler = tool_git_push;
    server.add_tool(std::move(git_push));

    Tool git_clone{};
    git_clone.name = "git_clone";
    git_clone.title = "Git clone";
    git_clone.description = "Clones a repository URL into an optional directory.";
    git_clone.input_schema = Json::parse(R"({
        "type": "object",
        "properties": {
            "url": { "type": "string", "description": "Repository URL." },
            "directory": { "type": "string", "description": "Target directory (optional)." },
            "depth": { "type": "integer", "description": "Shallow clone depth (optional)." }
        },
        "required": ["url"]
    })");
    git_clone.handler = tool_git_clone;
    server.add_tool(std::move(git_clone));

    const int exit_code = server.run_cli(argc, argv, 8099);
    return exit_code;
}