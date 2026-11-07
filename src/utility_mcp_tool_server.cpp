// ============================================================================
//  src/utility_mcp_tool_server.cpp — MCP String, JSON, and UUID Server
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Provides string operations, JSON extraction/validation/formatting, and
//  UUID generation. Runs over stdio by default or Streamable HTTP on port
//  8098 by default.
// ============================================================================

#include "pooriayousefi/utility/common.hpp"
#include "pooriayousefi/mcp/server.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pooriayousefi::mcp
{
    using namespace pooriayousefi;

    constexpr const char* WORD_DELIMITERS = " \t\n\r";

    // Shared Json schema properties (mirrors git server's cwd_prop).
    const Json text_prop = {{"type", "string"}};
    const Json sentence_prop = {{"type", "string"}};
    const Json delimiters_prop = {{"type", "string"}};
    const Json from_prop = {{"type", "string"}};
    const Json to_prop = {{"type", "string"}};
    const Json start_prop = {{"type", "integer"}};
    const Json length_prop = {{"type", "integer"}};
    const Json parts_prop = {{"type", "array"}, {"items", {{"type", "string"}}}};
    const Json separator_prop = {{"type", "string"}};
    const Json indent_prop = {{"type", "integer"}};

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

    std::vector<std::string> tokenize(const std::string& sentence, const std::string& delimiters)
    {
        std::vector<std::string> tokens{};
        tokens.reserve(16);

        auto last_pos = sentence.find_first_not_of(delimiters, 0);
        auto pos = sentence.find_first_of(delimiters, last_pos);

        while (pos != std::string::npos || last_pos != std::string::npos)
        {
            tokens.emplace_back(sentence.substr(last_pos, pos - last_pos));
            last_pos = sentence.find_first_not_of(delimiters, pos);
            pos = sentence.find_first_of(delimiters, last_pos);
        }
        return tokens;
    }

    std::string generate_uuid_v4()
    {
        thread_local std::mt19937 gen{std::random_device{}()};
        std::uniform_int_distribution<> dis(0, 15);
        std::uniform_int_distribution<> dis2(8, 11);

        std::stringstream ss;
        ss << std::hex;
        for (int i = 0; i < 8; i++)
        {
            ss << dis(gen);
        }
        ss << "-";
        for (int i = 0; i < 4; i++)
        {
            ss << dis(gen);
        }
        ss << "-4";
        for (int i = 0; i < 3; i++)
        {
            ss << dis(gen);
        }
        ss << "-";
        ss << dis2(gen);
        for (int i = 0; i < 3; i++)
        {
            ss << dis(gen);
        }
        ss << "-";
        for (int i = 0; i < 12; i++)
        {
            ss << dis(gen);
        }
        return ss.str();
    }

    std::size_t find_balanced_end(const std::string& text, std::size_t start)
    {
        std::size_t result = std::string::npos;
        char open_char = text[start];
        char close_char = (open_char == '{') ? '}' : ']';

        int balance = 0;
        bool in_string = false;
        bool escaped = false;

        for (std::size_t i = start; i < text.length() && result == std::string::npos; ++i)
        {
            char c = text[i];
            if (in_string)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (c == '\\')
                {
                    escaped = true;
                }
                else if (c == '"')
                {
                    in_string = false;
                }
                continue;
            }
            if (c == '"')
            {
                in_string = true;
                continue;
            }
            if (c == open_char)
            {
                balance++;
            }
            else if (c == close_char)
            {
                balance--;
            }
            if (balance == 0)
            {
                result = i;
            }
        }
        return result;
    }

    std::string trim_string(const std::string& s)
    {
        std::string result;
        const auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos)
        {
            result = "";
        }
        else
        {
            const auto end = s.find_last_not_of(" \t\n\r");
            result = s.substr(start, end - start + 1);
        }
        return result;
    }

    // ---- tools ----

    Expected<Json, Error> tool_tokenize_string(const Json& args)
    {
        Expected<Json, Error> result{};
        auto sentence = require_string_arg(args, "sentence");
        auto delimiters = require_string_arg(args, "delimiters");
        if (!sentence.has_value())
        {
            result.set_error(sentence.get_error());
        }
        else if (!delimiters.has_value())
        {
            result.set_error(delimiters.get_error());
        }
        else
        {
            auto tokens = tokenize(sentence.get_value(), delimiters.get_value());
            Json arr = Json::array();
            for (const auto& t : tokens)
            {
                arr.push_back(t);
            }
            result.set_value(Json{{"tokens", std::move(arr)}, {"count", static_cast<std::int64_t>(tokens.size())}});
        }
        return result;
    }

    Expected<Json, Error> tool_count_words(const Json& args)
    {
        Expected<Json, Error> result{};
        auto text = require_string_arg(args, "text");
        if (!text.has_value())
        {
            result.set_error(text.get_error());
        }
        else
        {
            auto tokens = tokenize(text.get_value(), WORD_DELIMITERS);
            result.set_value(Json{{"word_count", static_cast<std::int64_t>(tokens.size())}});
        }
        return result;
    }

    Expected<Json, Error> tool_extract_json_from_text(const Json& args)
    {
        Expected<Json, Error> result{};
        auto text_or = require_string_arg(args, "text");
        if (!text_or.has_value())
        {
            result.set_error(text_or.get_error());
        }
        else
        {
            const std::string& text = text_or.get_value();
            bool done = false;
            std::size_t search_from = 0;
            while (!done)
            {
                std::size_t start = text.find_first_of("{[", search_from);
                if (start == std::string::npos)
                {
                    result.set_error(Error{error_code::INVALID_PARAMS, "No valid Json object or array found in text."});
                    done = true;
                }
                else
                {
                    std::size_t end = find_balanced_end(text, start);
                    if (end == std::string::npos)
                    {
                        search_from = start + 1;
                    }
                    else
                    {
                        std::string json_str = text.substr(start, end - start + 1);
                        Json parsed = Json::parse(json_str, nullptr, false);
                        if (parsed.is_discarded())
                        {
                            search_from = start + 1;
                        }
                        else
                        {
                            result.set_value(Json{{"json", parsed}});
                            done = true;
                        }
                    }
                }
            }
        }
        return result;
    }

    Expected<Json, Error> tool_generate_uuid(const Json& /*args*/)
    {
        Expected<Json, Error> result{};
        result.set_value(Json{{"uuid", generate_uuid_v4()}});
        return result;
    }

    Expected<Json, Error> tool_to_upper(const Json& args)
    {
        Expected<Json, Error> result{};
        auto text = require_string_arg(args, "text");
        if (!text.has_value())
        {
            result.set_error(text.get_error());
        }
        else
        {
            std::string out = text.get_value();
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::toupper(c));
            });
            result.set_value(Json{{"text", out}});
        }
        return result;
    }

    Expected<Json, Error> tool_to_lower(const Json& args)
    {
        Expected<Json, Error> result{};
        auto text = require_string_arg(args, "text");
        if (!text.has_value())
        {
            result.set_error(text.get_error());
        }
        else
        {
            std::string out = text.get_value();
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            result.set_value(Json{{"text", out}});
        }
        return result;
    }

    Expected<Json, Error> tool_trim(const Json& args)
    {
        Expected<Json, Error> result{};
        auto text = require_string_arg(args, "text");
        if (!text.has_value())
        {
            result.set_error(text.get_error());
        }
        else
        {
            result.set_value(Json{{"text", trim_string(text.get_value())}});
        }
        return result;
    }

    Expected<Json, Error> tool_replace_all(const Json& args)
    {
        Expected<Json, Error> result{};
        auto text = require_string_arg(args, "text");
        auto from = require_string_arg(args, "from");
        auto to = require_string_arg(args, "to");
        if (!text.has_value())
        {
            result.set_error(text.get_error());
        }
        else if (!from.has_value())
        {
            result.set_error(from.get_error());
        }
        else if (!to.has_value())
        {
            result.set_error(to.get_error());
        }
        else if (from.get_value().empty())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "'from' must not be empty."});
        }
        else
        {
            std::string out = text.get_value();
            const std::string& f = from.get_value();
            const std::string& t = to.get_value();
            std::size_t pos = 0;
            std::int64_t replacements = 0;
            while ((pos = out.find(f, pos)) != std::string::npos)
            {
                out.replace(pos, f.size(), t);
                pos += t.size();
                replacements++;
            }
            result.set_value(Json{{"text", out}, {"replacements", replacements}});
        }
        return result;
    }

    Expected<Json, Error> tool_substring(const Json& args)
    {
        Expected<Json, Error> result{};
        auto text = require_string_arg(args, "text");
        if (!text.has_value())
        {
            result.set_error(text.get_error());
        }
        else if (!args.contains("start") || !args["start"].is_number_integer())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or non-integer 'start'."});
        }
        else
        {
            const std::string& s = text.get_value();
            long long start = args["start"].get<long long>();
            long long length = -1;
            if (args.contains("length") && args["length"].is_number_integer())
            {
                length = args["length"].get<long long>();
            }
            if (start < 0 || static_cast<std::size_t>(start) > s.size())
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "'start' out of range."});
            }
            else
            {
                std::string out;
                if (length < 0)
                {
                    out = s.substr(static_cast<std::size_t>(start));
                }
                else
                {
                    out = s.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(length));
                }
                result.set_value(Json{{"text", out}});
            }
        }
        return result;
    }

    Expected<Json, Error> tool_string_length(const Json& args)
    {
        Expected<Json, Error> result{};
        auto text = require_string_arg(args, "text");
        if (!text.has_value())
        {
            result.set_error(text.get_error());
        }
        else
        {
            result.set_value(Json{{"length", static_cast<std::int64_t>(text.get_value().size())}});
        }
        return result;
    }

    Expected<Json, Error> tool_join_strings(const Json& args)
    {
        Expected<Json, Error> result{};
        if (!args.contains("parts") || !args["parts"].is_array())
        {
            result.set_error(Error{error_code::INVALID_PARAMS, "Missing or non-array 'parts'."});
        }
        else
        {
            std::string separator = "";
            if (args.contains("separator") && args["separator"].is_string())
            {
                separator = args["separator"].get<std::string>();
            }
            std::ostringstream oss;
            bool first = true;
            for (const auto& part : args["parts"])
            {
                if (!part.is_string())
                {
                    result.set_error(Error{error_code::INVALID_PARAMS, "'parts' must contain only strings."});
                    first = false;
                    break;
                }
                if (!first)
                {
                    oss << separator;
                }
                oss << part.get<std::string>();
                first = false;
            }
            if (!result.has_error())
            {
                result.set_value(Json{{"text", oss.str()}});
            }
        }
        return result;
    }

    Expected<Json, Error> tool_slugify(const Json& args)
    {
        Expected<Json, Error> result{};
        auto text = require_string_arg(args, "text");
        if (!text.has_value())
        {
            result.set_error(text.get_error());
        }
        else
        {
            std::string out;
            out.reserve(text.get_value().size());
            bool last_dash = false;
            for (unsigned char c : text.get_value())
            {
                if (std::isalnum(c))
                {
                    out.push_back(static_cast<char>(std::tolower(c)));
                    last_dash = false;
                }
                else if (!last_dash && !out.empty())
                {
                    out.push_back('-');
                    last_dash = true;
                }
            }
            if (!out.empty() && out.back() == '-')
            {
                out.pop_back();
            }
            result.set_value(Json{{"slug", out}});
        }
        return result;
    }

    Expected<Json, Error> tool_is_valid_json(const Json& args)
    {
        Expected<Json, Error> result{};
        auto text = require_string_arg(args, "text");
        if (!text.has_value())
        {
            result.set_error(text.get_error());
        }
        else
        {
            Json parsed = Json::parse(text.get_value(), nullptr, false);
            result.set_value(Json{{"valid", !parsed.is_discarded()}});
        }
        return result;
    }

    Expected<Json, Error> tool_pretty_print_json(const Json& args)
    {
        Expected<Json, Error> result{};
        auto text = require_string_arg(args, "text");
        if (!text.has_value())
        {
            result.set_error(text.get_error());
        }
        else
        {
            Json parsed = Json::parse(text.get_value(), nullptr, false);
            if (parsed.is_discarded())
            {
                result.set_error(Error{error_code::INVALID_PARAMS, "Input is not valid Json."});
            }
            else
            {
                int indent = 2;
                if (args.contains("indent") && args["indent"].is_number_integer())
                {
                    indent = args["indent"].get<int>();
                    if (indent < 0)
                    {
                        indent = 0;
                    }
                    if (indent > 8)
                    {
                        indent = 8;
                    }
                }
                result.set_value(Json{{"text", parsed.dump(indent)}});
            }
        }
        return result;
    }
}

int main(int argc, char* argv[])
{
    using namespace pooriayousefi::mcp;

    Server server{"mcp-tool-utility-server", "1.0.0"};

    server.set_instructions(
        "String, Json, and UUID utility tool server over MCP 2026-07-28. "
        "All tools are pure, stateless computations: no filesystem, network, or shell access. "
        "String tools operate on UTF-8 bytes: to_upper/to_lower/slugify classify ASCII bytes via "
        "<cctype>, string_length reports std::string size in bytes (not Unicode code points), and "
        "count_words splits on spaces, tabs, and newlines. tokenize_string takes an explicit delimiter "
        "character set. replace_all requires a non-empty 'from'. substring takes a 0-based 'start' and "
        "an optional 'length' (negative or omitted length means 'to the end'). join_strings concatenates "
        "a 'parts' array of strings with an optional 'separator'. Json tools parse with a real Json "
        "parser: extract_json_from_text returns the first valid object or array embedded in messy text, "
        "is_valid_json reports validity, and pretty_print_json re-dumps the document with an 'indent' "
        "clamped to [0, 8]. generate_uuid returns a random UUID v4. Missing or wrongly typed arguments "
        "return INVALID_PARAMS errors."
    );

    Tool tokenize_string{};
    tokenize_string.name = "tokenize_string";
    tokenize_string.title = "Tokenize string";
    tokenize_string.description = "Splits a string into tokens using delimiter characters.";
    tokenize_string.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"sentence", sentence_prop},
            {"delimiters", delimiters_prop}
        }},
        {"required", Json::array({"sentence", "delimiters"})}
    };
    tokenize_string.handler = tool_tokenize_string;
    server.add_tool(std::move(tokenize_string));

    Tool count_words{};
    count_words.name = "count_words";
    count_words.title = "Count words";
    count_words.description = "Counts words in text (whitespace-delimited).";
    count_words.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"text", text_prop}
        }},
        {"required", Json::array({"text"})}
    };
    count_words.handler = tool_count_words;
    server.add_tool(std::move(count_words));

    Tool extract_json_from_text{};
    extract_json_from_text.name = "extract_json_from_text";
    extract_json_from_text.title = "Extract Json from text";
    extract_json_from_text.description = "Extracts the first valid Json object or array from messy text.";
    extract_json_from_text.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"text", text_prop}
        }},
        {"required", Json::array({"text"})}
    };
    extract_json_from_text.handler = tool_extract_json_from_text;
    server.add_tool(std::move(extract_json_from_text));

    Tool generate_uuid{};
    generate_uuid.name = "generate_uuid";
    generate_uuid.title = "Generate UUID";
    generate_uuid.description = "Generates a random UUID v4 string.";
    generate_uuid.input_schema = Json{
        {"type", "object"},
        {"properties", Json::object()},
        {"required", Json::array()}
    };
    generate_uuid.handler = tool_generate_uuid;
    server.add_tool(std::move(generate_uuid));

    Tool to_upper{};
    to_upper.name = "to_upper";
    to_upper.title = "Uppercase text";
    to_upper.description = "Converts text to upper case.";
    to_upper.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"text", text_prop}
        }},
        {"required", Json::array({"text"})}
    };
    to_upper.handler = tool_to_upper;
    server.add_tool(std::move(to_upper));

    Tool to_lower{};
    to_lower.name = "to_lower";
    to_lower.title = "Lowercase text";
    to_lower.description = "Converts text to lower case.";
    to_lower.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"text", text_prop}
        }},
        {"required", Json::array({"text"})}
    };
    to_lower.handler = tool_to_lower;
    server.add_tool(std::move(to_lower));

    Tool trim{};
    trim.name = "trim";
    trim.title = "Trim whitespace";
    trim.description = "Trims leading/trailing whitespace.";
    trim.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"text", text_prop}
        }},
        {"required", Json::array({"text"})}
    };
    trim.handler = tool_trim;
    server.add_tool(std::move(trim));

    Tool replace_all{};
    replace_all.name = "replace_all";
    replace_all.title = "Replace all occurrences";
    replace_all.description = "Replaces all occurrences of from with to.";
    replace_all.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"text", text_prop},
            {"from", from_prop},
            {"to", to_prop}
        }},
        {"required", Json::array({"text", "from", "to"})}
    };
    replace_all.handler = tool_replace_all;
    server.add_tool(std::move(replace_all));

    Tool substring{};
    substring.name = "substring";
    substring.title = "Substring";
    substring.description = "Substring by start index and optional length.";
    substring.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"text", text_prop},
            {"start", start_prop},
            {"length", length_prop}
        }},
        {"required", Json::array({"text", "start"})}
    };
    substring.handler = tool_substring;
    server.add_tool(std::move(substring));

    Tool string_length{};
    string_length.name = "string_length";
    string_length.title = "String length";
    string_length.description = "Byte/char length of a string (std::string size).";
    string_length.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"text", text_prop}
        }},
        {"required", Json::array({"text"})}
    };
    string_length.handler = tool_string_length;
    server.add_tool(std::move(string_length));

    Tool join_strings{};
    join_strings.name = "join_strings";
    join_strings.title = "Join strings";
    join_strings.description = "Joins string parts with an optional separator.";
    join_strings.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"parts", parts_prop},
            {"separator", separator_prop}
        }},
        {"required", Json::array({"parts"})}
    };
    join_strings.handler = tool_join_strings;
    server.add_tool(std::move(join_strings));

    Tool slugify{};
    slugify.name = "slugify";
    slugify.title = "Slugify text";
    slugify.description = "URL-ish slug from text (lowercase, hyphenated).";
    slugify.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"text", text_prop}
        }},
        {"required", Json::array({"text"})}
    };
    slugify.handler = tool_slugify;
    server.add_tool(std::move(slugify));

    Tool is_valid_json{};
    is_valid_json.name = "is_valid_json";
    is_valid_json.title = "Validate Json";
    is_valid_json.description = "Checks whether text is valid Json.";
    is_valid_json.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"text", text_prop}
        }},
        {"required", Json::array({"text"})}
    };
    is_valid_json.handler = tool_is_valid_json;
    server.add_tool(std::move(is_valid_json));

    Tool pretty_print_json{};
    pretty_print_json.name = "pretty_print_json";
    pretty_print_json.title = "Pretty-print Json";
    pretty_print_json.description = "Pretty-prints a Json string.";
    pretty_print_json.input_schema = Json{
        {"type", "object"},
        {"properties", Json{
            {"text", text_prop},
            {"indent", indent_prop}
        }},
        {"required", Json::array({"text"})}
    };
    pretty_print_json.handler = tool_pretty_print_json;
    server.add_tool(std::move(pretty_print_json));

    const int exit_code = server.run_cli(argc, argv, 8098);
    return exit_code;
}