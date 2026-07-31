// This file is part of SuperSlicer.
// SuperSlicer is released under the terms of the AGPLv3 or higher.

#include "catch2/catch.hpp"

#include "slic3r/GUI/Automation/AutomationSecurity.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Security = Slic3r::GUI::AutomationSecurity;

TEST_CASE("Automation bearer authentication is exact", "[Automation][Security]")
{
    constexpr const char* TOKEN = "development-secret";

    REQUIRE(Security::bearer_authorized("Bearer development-secret", TOKEN));
    REQUIRE_FALSE(Security::bearer_authorized("bearer development-secret", TOKEN));
    REQUIRE_FALSE(Security::bearer_authorized("Bearer development-secret ", TOKEN));
    REQUIRE_FALSE(Security::bearer_authorized("Bearer wrong", TOKEN));
    REQUIRE_FALSE(Security::bearer_authorized("development-secret", TOKEN));
    REQUIRE_FALSE(Security::bearer_authorized("Bearer ", ""));
}

TEST_CASE("Automation token comparison checks content and full length", "[Automation][Security]")
{
    REQUIRE(Security::constant_time_equal("", ""));
    REQUIRE(Security::constant_time_equal("same", "same"));
    REQUIRE_FALSE(Security::constant_time_equal("same", "different"));
    REQUIRE_FALSE(Security::constant_time_equal("prefix", "prefix-suffix"));

    const std::string short_value(1, '\0');
    const std::string long_value(257, '\0');
    REQUIRE_FALSE(Security::constant_time_equal(short_value, long_value));
}

TEST_CASE("Automation Host validation accepts only loopback authorities", "[Automation][Security]")
{
    constexpr const char* LOCALHOST = "localhost";
    const std::vector<std::string> accepted = {
        LOCALHOST, "LOCALHOST:43127", "127.0.0.1", "127.0.0.1:1",
        "[::1]", "[::1]:65535"
    };
    for (const std::string& value : accepted) {
        CAPTURE(value);
        REQUIRE(Security::local_authority(value));
    }

    const std::vector<std::string> rejected = {
        "", "localhost:", "localhost:0", "localhost:65536",
        "localhost:not-a-port", "localhost.example", "127.0.0.2",
        "::1", "[::1", "user@localhost:43127", "localhost/path"
    };
    for (const std::string& value : rejected) {
        CAPTURE(value);
        REQUIRE_FALSE(Security::local_authority(value));
    }
}

TEST_CASE("Automation Origin validation accepts only local HTTP origins", "[Automation][Security]")
{
    const std::vector<std::string> accepted = {
        "", "http://localhost", "HTTP://LOCALHOST:43127/",
        "https://127.0.0.1", "http://[::1]:43127"
    };
    for (const std::string& value : accepted) {
        CAPTURE(value);
        REQUIRE(Security::local_origin(value));
    }

    const std::vector<std::string> rejected = {
        "null", "file://localhost", "http://example.com",
        "http://localhost/path", "http://localhost?query=1",
        "http://user@localhost:43127", "http://localhost:65536"
    };
    for (const std::string& value : rejected) {
        CAPTURE(value);
        REQUIRE_FALSE(Security::local_origin(value));
    }
}
