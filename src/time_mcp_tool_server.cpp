// ============================================================================
//  src/time_mcp_tool_server.cpp — MCP Clock and Timestamp Server
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Provides UTC/local clock and Unix timestamp tools. Runs over stdio by
//  default or Streamable HTTP on port 8091 by default.
// ============================================================================

#include "pooriayousefi/utility/common.hpp"
#include "pooriayousefi/mcp/server.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace pooriayousefi::mcp
{
    // Cross-platform UTC conversion (Windows: gmtime_s, POSIX: gmtime_r).
    std::tm to_utc(std::time_t seconds_since_epoch, bool& ok)
    {
        std::tm broken_down{};
        ok = false;

#ifdef _WIN32
        ok = (::gmtime_s(&broken_down, &seconds_since_epoch) == 0);
#else
        ok = (::gmtime_r(&seconds_since_epoch, &broken_down) != nullptr);
#endif
        return broken_down;
    }

    // Cross-platform local-time conversion (Windows: localtime_s, POSIX: localtime_r).
    std::tm to_local(std::time_t seconds_since_epoch, bool& ok)
    {
        std::tm broken_down{};
        ok = false;

#ifdef _WIN32
        ok = (::localtime_s(&broken_down, &seconds_since_epoch) == 0);
#else
        ok = (::localtime_r(&seconds_since_epoch, &broken_down) != nullptr);
#endif
        return broken_down;
    }

    // Renders "%Y-%m-%dT%H:%M:%S" + ".mmm" + suffix (e.g. "Z" or "+02:00").
    std::string format_iso(const std::tm& broken_down, long long milliseconds, const char* suffix)
    {
        std::string result{};
        char seconds_buffer[32]{};
        const std::size_t written = std::strftime(seconds_buffer, sizeof(seconds_buffer), "%Y-%m-%dT%H:%M:%S", &broken_down);

        if (written > 0)
        {
            char millis_buffer[8]{};
            std::snprintf(millis_buffer, sizeof(millis_buffer), ".%03lld", milliseconds);
            result = std::string(seconds_buffer) + millis_buffer + suffix;
        }

        return result;
    }

    // Seconds the local zone is ahead of UTC (POSIX: tm_gmtoff; Windows:
    // _get_timezone negated, plus one hour during DST).
    long long local_utc_offset_seconds(const std::tm& local_time)
    {
        long long offset = 0;

#ifdef _WIN32
        long timezone_seconds = 0;

        if (_get_timezone(&timezone_seconds) == 0)
        {
            offset = -static_cast<long long>(timezone_seconds);

            if (local_time.tm_isdst > 0)
            {
                offset += 3600;
            }
        }
#else
        offset = static_cast<long long>(local_time.tm_gmtoff);
#endif
        return offset;
    }

    // Renders a UTC offset as "+HH:MM" or "-HH:MM" (or "Z" for zero).
    std::string format_utc_offset(long long offset_seconds)
    {
        std::string result{"Z"};

        if (offset_seconds != 0)
        {
            const char sign = (offset_seconds < 0) ? '-' : '+';
            long long absolute = (offset_seconds < 0) ? -offset_seconds : offset_seconds;
            const long long hours = absolute / 3600;
            const long long minutes = (absolute % 3600) / 60;
            char buffer[8]{};
            std::snprintf(buffer, sizeof(buffer), "%c%02lld:%02lld", sign, hours, minutes);
            result = std::string(buffer);
        }

        return result;
    }

    Json empty_object_schema()
    {
        Json schema = Json::object();
        schema["type"] = "object";
        schema["properties"] = Json::object();
        schema["required"] = Json::array();
        schema["additionalProperties"] = false;
        return schema;
    }

    const ToolHandler handle_get_current_time = [](const Json& arguments) -> Expected<Json, Error>
    {
        (void)arguments;
        Expected<Json, Error> outcome{};

        const auto now_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::time_t now_seconds = static_cast<std::time_t>(now_milliseconds / 1000);
        const long long millisecond_part = now_milliseconds % 1000;

        bool converted = false;
        const std::tm utc_time = to_utc(now_seconds, converted);

        if (!converted)
        {
            outcome.set_error(Error{error_code::INTERNAL_ERROR, "failed to convert current time to UTC"});
        }
        else
        {
            Json result = Json::object();
            result["iso8601"] = format_iso(utc_time, millisecond_part, "Z");
            result["timezone"] = "UTC";
            result["year"] = utc_time.tm_year + 1900;
            result["month"] = utc_time.tm_mon + 1;
            result["day"] = utc_time.tm_mday;
            result["hour"] = utc_time.tm_hour;
            result["minute"] = utc_time.tm_min;
            result["second"] = utc_time.tm_sec;
            result["millisecond"] = millisecond_part;
            result["weekday"] = utc_time.tm_wday;
            result["day_of_year"] = utc_time.tm_yday;
            outcome.set_value(std::move(result));
        }

        return outcome;
    };

    const ToolHandler handle_get_local_time = [](const Json& arguments) -> Expected<Json, Error>
    {
        (void)arguments;
        Expected<Json, Error> outcome{};

        const auto now_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::time_t now_seconds = static_cast<std::time_t>(now_milliseconds / 1000);
        const long long millisecond_part = now_milliseconds % 1000;

        bool converted = false;
        const std::tm local_time = to_local(now_seconds, converted);

        if (!converted)
        {
            outcome.set_error(Error{error_code::INTERNAL_ERROR, "failed to convert current time to the local zone"});
        }
        else
        {
            const long long offset_seconds = local_utc_offset_seconds(local_time);

            Json result = Json::object();
            result["iso8601"] = format_iso(local_time, millisecond_part, format_utc_offset(offset_seconds).c_str());
            result["timezone"] = "local";
            result["utc_offset_seconds"] = offset_seconds;
            result["utc_offset"] = format_utc_offset(offset_seconds);
            result["year"] = local_time.tm_year + 1900;
            result["month"] = local_time.tm_mon + 1;
            result["day"] = local_time.tm_mday;
            result["hour"] = local_time.tm_hour;
            result["minute"] = local_time.tm_min;
            result["second"] = local_time.tm_sec;
            result["millisecond"] = millisecond_part;
            result["daylight_saving"] = (local_time.tm_isdst > 0);
            outcome.set_value(std::move(result));
        }

        return outcome;
    };

    const ToolHandler handle_get_unix_timestamp = [](const Json& arguments) -> Expected<Json, Error>
    {
        (void)arguments;
        Expected<Json, Error> outcome{};

        const auto now_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::time_t now_seconds = static_cast<std::time_t>(now_milliseconds / 1000);

        bool converted = false;
        const std::tm utc_time = to_utc(now_seconds, converted);

        if (!converted)
        {
            outcome.set_error(Error{error_code::INTERNAL_ERROR, "failed to convert current time to UTC"});
        }
        else
        {
            Json result = Json::object();
            result["unix_seconds"] = now_milliseconds / 1000;
            result["unix_milliseconds"] = now_milliseconds;
            result["iso8601"] = format_iso(utc_time, now_milliseconds % 1000, "Z");
            outcome.set_value(std::move(result));
        }

        return outcome;
    };
}

// =====================================================================
//  main
// =====================================================================
int main(int argc, char* argv[])
{
    using namespace pooriayousefi::mcp;

    Server server{"time", "1.0.0"};
    server.set_instructions("Time tools: UTC and local wall-clock time plus unix timestamps. Every tool takes no arguments.");

    Tool current_time{};
    current_time.name = "get_current_time";
    current_time.title = "Get current time (UTC)";
    current_time.description = "Returns the current UTC time as ISO 8601 with millisecond precision plus date/time components.";
    current_time.input_schema = empty_object_schema();
    current_time.handler = handle_get_current_time;
    server.add_tool(std::move(current_time));

    Tool local_time{};
    local_time.name = "get_local_time";
    local_time.title = "Get local time";
    local_time.description = "Returns the current local time as ISO 8601 plus the UTC offset and date/time components.";
    local_time.input_schema = empty_object_schema();
    local_time.handler = handle_get_local_time;
    server.add_tool(std::move(local_time));

    Tool unix_timestamp{};
    unix_timestamp.name = "get_unix_timestamp";
    unix_timestamp.title = "Get unix timestamp";
    unix_timestamp.description = "Returns the seconds and milliseconds elapsed since the Unix epoch.";
    unix_timestamp.input_schema = empty_object_schema();
    unix_timestamp.handler = handle_get_unix_timestamp;
    server.add_tool(std::move(unix_timestamp));

    const int exit_code = server.run_cli(argc, argv, 8091);
    return exit_code;
}