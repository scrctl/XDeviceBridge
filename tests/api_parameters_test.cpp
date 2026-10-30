#include "server/ApiParameters.h"
#include "media/OutputRate.h"
#include "transport/DeviceError.h"
#include <iostream>
#include <stdexcept>
using nlohmann::json;
int main() {
    using namespace xdb::server::detail;
    using namespace std::chrono_literals;
    if (video_max_fps(json::object()) != 0 || video_max_fps({{"max_fps", 0}}) != 0 ||
        video_max_fps({{"max_fps", 10}}) != 10 || video_max_fps({{"max_fps", 240u}}) != 240)
        throw std::runtime_error("video output rate defaults/range changed");
    for (const auto &value : {json(-1), json(241), json(10.0), json(true), json(nullptr),
                             json("10"), json(uint64_t(-1))}) {
        bool rejected = false;
        try { (void)video_max_fps({{"max_fps", value}}); } catch (const std::exception &) { rejected = true; }
        if (!rejected) throw std::runtime_error("invalid video output rate accepted");
    }
    if (video_max_bitrate(json::object()) != 0 || video_max_bitrate({{"max_bitrate", 0}}) != 0 ||
        video_max_bitrate({{"max_bitrate", 1500000}}) != 1500000 ||
        video_max_bitrate({{"max_bitrate", UINT64_MAX}}) != UINT64_MAX)
        throw std::runtime_error("video bitrate defaults/range changed");
    for (const auto &value : {json(-1), json(1500000.0), json(true), json(nullptr),
                             json("1500000")}) {
        bool rejected = false;
        try { (void)video_max_bitrate({{"max_bitrate", value}}); } catch (const std::exception &) { rejected = true; }
        if (!rejected) throw std::runtime_error("invalid video bitrate accepted");
    }
    xdb::media::OutputRate gate;
    const auto now = std::chrono::steady_clock::time_point{};
    if (!gate.due(10, now)) throw std::runtime_error("first frame rate limited");
    gate.published(now);
    if (gate.due(10, now + 99ms) || !gate.due(10, now + 100ms) || !gate.due(0, now))
        throw std::runtime_error("output interval or unlimited mode failed");
    gate.published(now + 10s);
    if (gate.due(10, now + 10001ms) || !gate.due(60, now + 10017ms))
        throw std::runtime_error("pause catch-up burst or live rate change");
    xdb::media::OutputRateRequests rates;
    rates.set("maa", 10); rates.set("mirror", 60);
    if (rates.effective() != 60) throw std::runtime_error("low-rate owner capped another mirror");
    rates.set("mirror", 0);
    if (rates.effective() != 0) throw std::runtime_error("unlimited owner was rate limited");
    rates.erase("mirror");
    if (rates.effective() != 10) throw std::runtime_error("departed owner's rate retained");
    rates.set("maa", 20);
    if (rates.effective() != 20) throw std::runtime_error("same group created duplicate rate ownership");
    rates.clear();
    if (rates.effective() != 0) throw std::runtime_error("closed session retained rate requests");
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
    for (auto method : {"mdns.check", "mdns.services", "screenshot", "device.info", "display.info", "app.list", "clipboard.get", "video.frame", "media.status", "connection.close"})
        if (!read_only_method(method)) throw std::runtime_error("read-only query rejected");
    for (auto method : {"input.tap", "video.start", "video.stop", "audio.read", "recording.configure", "media.finish", "pair", "ddi.remove", "disconnect", "server.stop", "future.method"})
        if (read_only_method(method)) throw std::runtime_error("mutation allowed in a read-only session");
    using namespace xdb::transport;
    for (auto [response, expected] : {std::pair{"PasswordProtected", DeviceError::locked},
            {"DeviceLocked", DeviceError::locked}, {"InvalidHostID", DeviceError::unauthorized},
            {"UserDeniedPairing", DeviceError::unauthorized}, {"UserDenied", DeviceError::unauthorized},
            {"InvalidPairRecord", DeviceError::unauthorized}, {"PairingDialogResponsePending", DeviceError::unauthorized},
            {"InvalidService", DeviceError::other}, {"SSL handshake failed", DeviceError::other},
            {"timed out", DeviceError::other}, {"not PasswordProtected", DeviceError::other}}) {
        DeviceError actual = DeviceError::not_paired;
        const auto message = lockdown_rejection("StartSession", response, &actual);
        if (actual != expected || message.find(response) == std::string::npos)
            throw std::runtime_error("device response classification or cause preservation failed");
        if (actual == DeviceError::locked && (message.find("unlock") == std::string::npos ||
            message.find("pair") != std::string::npos || message.find("DDI") != std::string::npos))
            throw std::runtime_error("lock error incorrectly recommends pairing or DDI");
        if (actual == DeviceError::unauthorized && message.find("xdb -d pair") == std::string::npos)
            throw std::runtime_error("USB trust recovery guidance missing");
    }
    std::cout << "PASS: screenshot enum/type/default and recognized/unknown initial orientations\n";
}
