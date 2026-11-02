// This file is part of SuperSlicer.
// SuperSlicer is released under the terms of the AGPLv3 or higher.

#include "AutomationSecurity.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace Slic3r::GUI::AutomationSecurity {

std::string lower_ascii(const std::string& input)
{
    std::string value = input;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool constant_time_equal(const std::string& lhs, const std::string& rhs)
{
    const std::size_t count = std::max(lhs.size(), rhs.size());
    std::size_t difference = lhs.size() ^ rhs.size();
    for (std::size_t index = 0; index < count; ++index) {
        const unsigned char left =
            index < lhs.size() ? static_cast<unsigned char>(lhs[index]) : 0;
        const unsigned char right =
            index < rhs.size() ? static_cast<unsigned char>(rhs[index]) : 0;
        difference |= static_cast<std::size_t>(left ^ right);
    }
    return difference == 0;
}

bool local_authority(const std::string& input)
{
    const std::string authority = lower_ascii(input);
    if (authority == "localhost" || authority == "127.0.0.1" ||
        authority == "[::1]")
        return true;

    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos)
        return false;

    const std::string host = authority.substr(0, colon);
    const std::string port_text = authority.substr(colon + 1);
    if (port_text.empty() ||
        !std::all_of(port_text.begin(), port_text.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
        }))
        return false;

    unsigned port = 0;
    for (const char ch : port_text) {
        const unsigned digit = static_cast<unsigned>(ch - '0');
        if (port > (65535U - digit) / 10U)
            return false;
        port = port * 10U + digit;
    }
    return port != 0 &&
           (host == "localhost" || host == "127.0.0.1" || host == "[::1]");
}

bool local_origin(const std::string& origin)
{
    if (origin.empty())
        return true;

    const std::string lowered = lower_ascii(origin);
    constexpr std::string_view HTTP_PREFIX = "http://";
    constexpr std::string_view HTTPS_PREFIX = "https://";
    std::string authority;
    if (lowered.starts_with(HTTP_PREFIX))
        authority = lowered.substr(HTTP_PREFIX.size());
    else if (lowered.starts_with(HTTPS_PREFIX))
        authority = lowered.substr(HTTPS_PREFIX.size());
    else
        return false;

    const std::size_t slash = authority.find('/');
    if (slash != std::string::npos && authority.substr(slash) != "/")
        return false;
    if (slash != std::string::npos)
        authority.resize(slash);
    return local_authority(authority);
}

bool bearer_authorized(
    const std::string& authorization, const std::string& token)
{
    constexpr std::string_view PREFIX = "Bearer ";
    return !token.empty() && authorization.starts_with(PREFIX) &&
           constant_time_equal(authorization.substr(PREFIX.size()), token);
}

} // namespace Slic3r::GUI::AutomationSecurity
