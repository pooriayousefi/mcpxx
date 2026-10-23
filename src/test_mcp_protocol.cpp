#include "pooriayousefi/mcp/protocol.hpp"

#include <cassert>
#include <iostream>

int main()
{
    using namespace pooriayousefi::mcp;

    const ClientInfo client{"mcpp-test-client", "0.1.0"};
    const JSON request = make_request(
        1, 
        "server/discover", 
        JSON::object(), 
        client
    );

    assert(is_valid_request(request));
    assert(has_valid_mcp_meta(request));
    assert(request["method"] == "server/discover");
    assert(request["params"]["_meta"][std::string(META_PROTOCOL_VERSION)] == PROTOCOL_VERSION);

    const JSON result = make_result(
        1, 
        {
            {"supportedVersions", {std::string(PROTOCOL_VERSION)}},
            {"capabilities", JSON::object()}
        }
    );
    assert(result["jsonrpc"] == "2.0");
    assert(result["id"] == 1);
    assert(result.contains("result"));

    const JSON error = make_error(
        2, 
        error_code::METHOD_NOT_FOUND,
        "Unknown method"
    );
    assert(error["id"] == 2);
    assert(error["error"]["code"] == error_code::METHOD_NOT_FOUND);

    const JSON notification = make_notification("notifications/example");
    assert(is_notification(notification));
    assert(!notification.contains("id"));

    JSON malformed = request;
    malformed["params"]["_meta"].erase(std::string(META_CLIENT_CAPABILITIES));
    assert(!has_valid_mcp_meta(malformed));

    std::cout << "protocol tests passed\n";
}
