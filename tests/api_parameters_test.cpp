#include "server/ApiParameters.h"
#include <iostream>
#include <stdexcept>
using nlohmann::json;
int main() {
    using namespace xdb::server::detail;
    if (screenshot_format(json::object()) != "png" || screenshot_format({{"format", "bgra"}}) != "bgra")
        throw std::runtime_error("supported/default screenshot format changed");
    for (const auto &value : {json("jpeg"), json(""), json("PNG"), json(nullptr), json(1), json(true)}) {
        bool rejected = false;
        try { (void)screenshot_format({{"format", value}}); } catch (const std::exception &) { rejected = true; }
        if (!rejected) throw std::runtime_error("invalid screenshot format silently accepted");
    }
    for (auto [name, expected] : {std::pair{"rot0",0}, {"portrait",0}, {"rot90",90},
          {"landscapeLeft",90}, {"rot180",180}, {"portraitUpsideDown",180},
          {"rot270",270}, {"landscapeRight",270}}) {
        const auto angle = recording_orientation(name);
        if (!angle || *angle != expected) throw std::runtime_error("device angle mapping changed");
    }
    if (recording_orientation("") || recording_orientation("unknown"))
        throw std::runtime_error("unknown orientation silently treated as portrait");
    for (auto method : {"screenshot", "device.info", "display.info", "app.list", "clipboard.get", "video.frame", "media.status", "connection.close"})
        if (!read_only_method(method)) throw std::runtime_error("read-only query rejected");
    for (auto method : {"input.tap", "video.start", "video.stop", "audio.read", "recording.configure", "media.finish", "pair", "ddi.remove", "disconnect", "server.stop", "future.method"})
        if (read_only_method(method)) throw std::runtime_error("mutation allowed in a read-only session");
    std::cout << "PASS: screenshot enum/type/default and recognized/unknown initial orientations\n";
}
