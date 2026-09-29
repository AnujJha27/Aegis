#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

namespace aegis::daemon::api {

inline bool allowedWebSocketOrigin(std::string_view origin) {
    if (origin.empty()) return true;
    constexpr std::string_view scheme = "http://";
    if (!origin.starts_with(scheme)) return false;
    const auto authority = origin.substr(scheme.size());
    if (authority.empty() || authority.find_first_of("/?#@\\") != std::string_view::npos) return false;

    std::string_view host;
    std::string_view port;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return false;
        host = authority.substr(0, close + 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return false;
            port = authority.substr(close + 2);
        }
    } else {
        const auto colon = authority.find(':');
        if (colon == std::string_view::npos) host = authority;
        else {
            if (authority.find(':', colon + 1) != std::string_view::npos) return false;
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }
    }

    std::string normalizedHost(host);
    std::transform(normalizedHost.begin(), normalizedHost.end(), normalizedHost.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (normalizedHost != "localhost" && normalizedHost != "127.0.0.1" && normalizedHost != "[::1]") return false;
    if (port.empty()) return authority.find(':') == std::string_view::npos || (authority.front() == '[' && authority.back() == ']');

    unsigned int number = 0;
    const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
    return error == std::errc{} && end == port.data() + port.size() && number > 0 && number <= 65535;
}

inline bool allowedLoopbackHost(std::string_view host, std::size_t headerCount = 1) {
    if (headerCount != 1) return false;
    std::string origin = "http://";
    origin.append(host);
    return allowedWebSocketOrigin(origin);
}

}
