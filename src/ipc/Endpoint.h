// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace xdb::ipc {
struct Endpoint {
    std::string address;
    uint16_t port = 49152;
    std::string label() const {
        return (address.find(':') == std::string::npos ? address : "[" + address + "]") +
               ":" + std::to_string(port);
    }
};
// Bare IPv6 uses the default port; an explicit IPv6 port requires [address]:port.
inline Endpoint parse_endpoint(std::string_view text) {
    Endpoint result;
    std::string_view host = text, port;
    bool explicit_port = false;
    if (text.starts_with('[')) {
        const auto end = text.find(']');
        if (end == std::string_view::npos) throw std::invalid_argument("Missing ] in device address");
        host = text.substr(1, end - 1);
        if (host.find(':') == std::string_view::npos)
            throw std::invalid_argument("Brackets require an IPv6 address");
        const auto tail = text.substr(end + 1);
        if (!tail.empty()) {
            if (!tail.starts_with(':')) throw std::invalid_argument("Expected [IPv6]:PORT");
            port = tail.substr(1);
            explicit_port = true;
        }
    } else if (const auto colon = text.find(':'); colon != std::string_view::npos && colon == text.rfind(':')) {
        host = text.substr(0, colon);
        port = text.substr(colon + 1);
        explicit_port = true;
    }
    if (host.empty() || host.size() > 253 || host == "auto")
        throw std::invalid_argument("A device IP address or hostname is required");
    for (const unsigned char c : host)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == ':' || c == '%' || c == '_'))
            throw std::invalid_argument("Invalid device address");
    if (explicit_port) {
        unsigned n = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), n);
        if (port.empty() || error != std::errc{} || end != port.data() + port.size() || !n || n > 65535)
            throw std::invalid_argument("Device port must be in 1..65535");
        result.port = static_cast<uint16_t>(n);
    }
    result.address = host;
    return result;
}
} // namespace xdb::ipc
