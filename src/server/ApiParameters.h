#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace xdb::server::detail {
inline bool read_only_method(std::string_view method) {
    return method == "server.info" || method == "devices" || method == "connection.close" ||
           method == "connection.open" || method == "connection.attach" || method == "device.info" ||
           method == "display.info" || method == "screenshot" || method == "app.list" ||
           method == "clipboard.get" || method == "video.frame" || method == "media.status";
}
inline std::string screenshot_format(const nlohmann::json &params) {
    auto format = params.value("format", std::string("png"));
    if (format != "png" && format != "bgra")
        throw std::invalid_argument("Screenshot format must be png or bgra");
    return format;
}
inline std::optional<int> recording_orientation(std::string_view orientation) {
    if (orientation == "portrait" || orientation == "rot0") return 0;
    if (orientation == "landscapeLeft" || orientation == "rot90") return 90;
    if (orientation == "portraitUpsideDown" || orientation == "rot180") return 180;
    if (orientation == "landscapeRight" || orientation == "rot270") return 270;
    return std::nullopt;
}
} // namespace xdb::server::detail
