#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace xdb::server::detail {
inline unsigned video_max_fps(const nlohmann::json &params) {
    if (!params.contains("max_fps")) return 0;
    const auto &value = params.at("max_fps");
    if (value.is_number_unsigned()) {
        const auto fps = value.get<uint64_t>();
        if (fps <= 240) return static_cast<unsigned>(fps);
    } else if (value.is_number_integer()) {
        const auto fps = value.get<int64_t>();
        if (fps >= 0 && fps <= 240) return static_cast<unsigned>(fps);
    }
    throw std::invalid_argument("max_fps must be an integer in 0..240 (0 is unlimited)");
}
inline bool read_only_method(std::string_view method) {
    return method == "mdns.check" || method == "mdns.services" || method == "server.info" || method == "devices" || method == "connection.close" ||
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
