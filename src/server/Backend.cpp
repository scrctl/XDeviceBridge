// SPDX-License-Identifier: LGPL-3.0-only
#include "Backend.h"
#include "ApiParameters.h"
#include "Discovery.h"
#include "ddi/Download.h"
#include "hid/Hid.h"
#include "media/AudioPump.h"
#include "media/FramePump.h"
#include "media/Recorder.h"
#include "media/ScreenshotSource.h"
#include "net/Stack.h"
#include "remote/App.h"
#include "remote/Discovery.h"
#include "remote/DisplayInfo.h"
#include "remote/OrientationControl.h"
#include "remote/Pairing.h"
#include "remote/Pasteboard.h"
#include "remote/WifiPairing.h"
#include "transport/IoBudget.h"
#include "util/Hex.h"
#include "wifi/Crypto.h"
#include "wifi/PairRecord.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <shared_mutex>
#include <thread>
namespace xdb::server {
namespace {
int bounded(const Json &p, const char *key, int fallback, int min, int max) {
    if (!p.contains(key))
        return fallback;
    if (!p[key].is_number_integer())
        throw std::invalid_argument(std::string(key) + " must be an integer");
    auto v = p[key].get<int64_t>();
    if (v < min || v > max)
        throw std::invalid_argument(std::string(key) + " is out of range");
    return int(v);
}
std::vector<wifi::PairRecord> records(const std::string &dir, const Cancel &cancel) {
    std::vector<std::string> warnings;
    bool stopped = false;
    return remote::detail::load_discovery_records(dir, cancel, warnings, stopped);
}
bool unit(double n) { return std::isfinite(n) && n >= 0 && n <= 1; }
} // namespace
std::optional<remote::Device> open_device(const std::string &udid, const std::string &wifi_address,
                                          std::string &error, uint16_t port, const Cancel &cancel,
                                          const std::string &directory) {
    transport::IoBudget budget({std::chrono::seconds(30), cancel});
    if (wifi_address.empty()) {
        auto usb = remote::Device::establish_bounded(udid, budget, error, false, true);
        if (usb)
            return usb;
        if (cancel())
            return std::nullopt;
        error.clear();
    }
    auto paired = records(directory, cancel);
    auto record = std::find_if(paired.begin(), paired.end(), [&](const auto &r) {
        return r.udid == udid && r.complete() && r.has_peer_identity();
    });
    if (record == paired.end()) {
        error = "No usable pairing for the requested UDID";
        return std::nullopt;
    }
    if (!wifi_address.empty() && wifi_address != "auto")
        return remote::Device::establish_wifi_bounded(wifi_address, *record, budget, error, false, port);
    remote::DiscoveryOptions options;
    options.include_usb = false;
    options.pairing_directory = directory;
    options.should_cancel = cancel;
    auto discovery = remote::discover_devices(options);
    for (const auto &device : discovery.devices) {
        if (device.udid != udid)
            continue;
        for (const auto &c : device.candidates) {
            if (c.transport != remote::DiscoveryTransport::remote_pairing ||
                c.pairing != remote::DiscoveryPairing::ready || c.address.empty())
                continue;
            auto connected =
                remote::Device::establish_wifi_bounded(c.address, *record, budget, error, false, c.port);
            if (connected)
                return connected;
            if (cancel())
                return std::nullopt;
        }
    }
    if (error.empty())
        error = "No paired Wi-Fi device discovered";
    return std::nullopt;
}
struct Backend::Session {
    // Reverse destruction keeps all services/worker references inside Device's lifetime.
    Registry &registry;
    std::string udid, attachment;
    std::shared_mutex media_mutex;
    std::mutex control_mutex, pcm_mutex;
    bool pcm_started = false;
    remote::Device device;
    std::shared_ptr<media::SessionGroup> media_group = std::make_shared<media::SessionGroup>();
    std::unique_ptr<remote::DisplayWatcher> display;
    std::unique_ptr<hid::Service> hid;
    std::unique_ptr<hid::Buttons> buttons;
    std::unique_ptr<media::Recorder> recorder;
    std::unique_ptr<media::FramePump> video;
    std::unique_ptr<media::AudioPump> audio;
    std::set<uint16_t> keys;
    std::set<std::pair<uint16_t, uint16_t>> held_buttons;
    bool touching = false;
    double x = 0, y = 0;
    Session(Registry &r, remote::Device value) : registry(r), udid(value.udid()), device(std::move(value)) {}
    bool finish(std::string &error) {
        if (audio)
            audio->stop_receiving();
        bool ok = !video || video->finish_recording(error);
        std::string record_error;
        if (recorder && !recorder->finish(record_error)) {
            error = record_error;
            ok = false;
        }
        if (audio)
            audio->stop();
        audio.reset();
        video.reset();
        recorder.reset();
        return ok;
    }
    ~Session() {
        std::string ignored;
        finish(ignored);
        {
            std::lock_guard lock(registry.mutex);
            registry.sessions.erase(attachment);
            registry.devices.erase(udid);
        }
        if (hid) {
            if (touching)
                hid->touch(hid::kSurfaceMainTouchscreen, x, y, false, ignored);
            hid->send_report(hid::kSurfaceKeyboard, hid::keyboard_report({}), ignored);
        }
        if (buttons)
            for (auto [page, code] : held_buttons)
                buttons->release(page, code, ignored);
    }
};
Backend::Backend(Registry &r, std::string dir) : registry_(r), directory_(std::move(dir)) {}
Backend::~Backend() { close(); }
void Backend::close() {
    session_.reset();
    if (!lease_.empty())
        registry_.release(lease_);
    lease_.clear();
}
Reply Backend::handle(const std::string &method, const Json &p, const Event &event, const Cancel &cancel) {
    auto fail = [](const std::string &error) { return Reply::failure("operation_failed", error); };
    std::string error;
    if (cancel())
        return Reply::failure("cancelled", "Request cancelled");
    if (method == "devices" || method == "pair" || method == "ddi.download" || method == "ddi.prepare" ||
        method == "ddi.status" || method == "ddi.remove")
        return handle_management(method, p, event, cancel);
    if (method == "connection.close" || method == "connection.attach" || method == "connection.open")
        return handle_connection(method, p, cancel);
    if (method.starts_with("input.") && method != "input.text" && method != "input.tap" &&
        method != "input.touch" && method != "input.keyboard" && method != "input.button")
        return Reply::failure("unknown_method", "Unknown method: " + method);
    if (!session_)
        return Reply::failure("not_connected", "Open a device session first");
    auto &s = *session_;
    if (method == "network.debug") {
        if (auto *stack = s.device.stack())
            stack->set_net_debug(p.value("enabled", false));
        return {};
    }
    if (method == "device.info") {
        Reply r;
        r.result = {{"udid", s.device.udid()},
                    {"os_version", s.device.property("OSVersion")},
                    {"product_type", s.device.property("ProductType")}};
        return r;
    }
    // Read-only media requests use shared ownership; configuration/stop cannot free a pump during a read.
    const bool media_read = method == "video.frame" || method == "audio.read" || method == "media.status";
    std::shared_lock<std::shared_mutex> read_lock(s.media_mutex, std::defer_lock);
    std::unique_lock<std::shared_mutex> write_lock(s.media_mutex, std::defer_lock);
    if (media_read)
        read_lock.lock();
    else if (method.starts_with("video.") || method.starts_with("audio.") || method.starts_with("media."))
        write_lock.lock();
    std::unique_lock<std::mutex> control_lock(s.control_mutex, std::defer_lock);
    if (!media_read)
        control_lock.lock();
    const auto geometry = [&] {
        if (!s.display)
            return Json::object();
        auto g = s.display->latest();
        return Json{
            {"width", g.width}, {"height", g.height}, {"orientation", g.orientation}, {"serial", g.seq}};
    };
    const auto prepare_display = [&] {
        if (!s.display)
            s.display = remote::DisplayWatcher::start(s.device, 1, error);
        if (!s.display)
            return false;
        // Subscription setup precedes its first asynchronous state. Do not treat
        // unknown -> known geometry as a device rotation during the first capture.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!cancel()) {
            const auto state = s.display->latest();
            if (state.width > 0 && state.height > 0 && !state.orientation.empty())
                return true;
            if (!s.display->alive() || std::chrono::steady_clock::now() >= deadline)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        error = cancel() ? "Display initialization cancelled" : "Timed out waiting for initial display geometry";
        return false;
    };
    if (method == "display.info") {
        if (!prepare_display())
            return fail(error);
        Reply r;
        r.result = geometry();
        return r;
    }
    if (method == "clipboard.set" || method == "clipboard.get") {
        Reply r;
        if (method == "clipboard.set") {
            auto text = p.at("text").get<std::string>();
            if (text.size() > remote::kMaxClipboardTextBytes)
                return Reply::failure("invalid_params", "Clipboard exceeds 256 KiB");
            if (!remote::Pasteboard::set_text(s.device, text, error))
                return fail(error);
        } else {
            std::string text;
            if (!remote::Pasteboard::get_text(s.device, text, error))
                return fail(error);
            r.result = {{"text", text}};
        }
        return r;
    }
    if (method == "display.rotate") {
        auto control = remote::OrientationControl::connect(s.device, error);
        remote::OrientationState before, after;
        if (!control || control->query(before, error) != remote::CallResult::Ok)
            return fail(error);
        auto baseline = remote::OrientationControl::effective_cardinal(before);
        if (!baseline)
            return fail("Device orientation has no cardinal baseline");
        auto target = *baseline == remote::DeviceOrientation::Portrait ||
                              *baseline == remote::DeviceOrientation::PortraitUpsideDown
                          ? remote::DeviceOrientation::LandscapeLeft
                          : remote::DeviceOrientation::Portrait;
        if (control->change(target, after, error) != remote::CallResult::Ok)
            return fail(error);
        if (remote::OrientationControl::effective_cardinal(after) != target)
            return fail("Device did not report the requested orientation");
        Reply r;
        r.result = {{"target", remote::OrientationControl::name(target)}};
        return r;
    }
    if (method == "screenshot") {
        const auto format = detail::screenshot_format(p);
        if (!prepare_display())
            return fail(error);
        auto before = geometry();
        auto input = xpc::make_dict();
        xpc::dict_set(input, "displayUniqueID", xpc::make_null());
        xpc::dict_set(input, "requestedFormat", xpc::make_string("png"));
        xpc::Value answer;
        if (!s.device.feature("com.apple.coredevice.screencaptureservice",
                              "com.apple.coredevice.feature.capturescreenshot",
                              "com.apple.coredevice.action.capturescreenshot", input, answer, error))
            return fail(error);
        auto *image = answer.find("image");
        if (!image || image->data.empty())
            return fail("Screenshot response has no image data");
        Reply r;
        auto after = geometry();
        if (before != after)
            return Reply::failure("geometry_changed", "Display changed during screenshot; retry capture");
        r.result = {{"format", "png"}, {"display", after}};
        if (format == "bgra") {
            Frame frame;
            if (!media::decode_png_bgra(image->data, frame, error))
                return fail(error);
            r.result["format"] = "bgra";
            r.result["width"] = frame.width;
            r.result["height"] = frame.height;
            r.result["row_pitch"] = frame.row_pitch;
            r.binary = std::move(frame.pixels);
        } else
            r.binary = image->data;
        return r;
    }
    if (method == "media.finish") {
        return s.finish(error) ? Reply{} : fail(error);
    }
    if (method == "media.status") {
        Reply r;
        std::string terminal = s.device.stack() ? s.device.stack()->pump_error() : std::string();
        const std::string recording_error = s.recorder ? s.recorder->error() : std::string();
        if (!s.video && s.audio && !s.audio->terminal_error().empty())
            terminal = s.audio->terminal_error();
        r.result = {{"error", terminal}, {"video_unusable", s.video && s.video->video_unusable()},
                    {"recording_error", recording_error}};
        return r;
    }
    if (method == "recording.configure") {
        if (s.video || s.audio || s.recorder)
            return Reply::failure("invalid_state", "Configure recording before media starts");
        media::Recorder::Options o;
        o.path = p.at("path").get<std::string>();
        if (!std::filesystem::u8path(o.path).is_absolute())
            return Reply::failure("invalid_params", "Recording path must be absolute on the server host");
        auto format = p.value("format", std::string("mkv"));
        if (format != "mkv" && format != "mp4")
            return Reply::failure("invalid_params", "Container must be mkv or mp4");
        o.format =
            format == "mp4" ? media::RecordingMuxer::Format::Mp4 : media::RecordingMuxer::Format::Matroska;
        o.include_video = p.value("video", true);
        o.include_audio = p.value("audio", false);
        o.video_orientation = bounded(p, "orientation", 0, 0, 270);
        if (p.value("orientation_from_device", false)) {
            if (!prepare_display())
                return fail(error);
            const auto angle = detail::recording_orientation(geometry().value("orientation", std::string()));
            if (!angle)
                return fail("Unsupported initial display orientation");
            o.video_orientation = *angle;
        }
        s.recorder = media::Recorder::start(o, error);
        return s.recorder ? Reply{} : fail(error);
    }
    if (method == "audio.start") {
        if (!s.audio) {
            media::AudioPump::Options o;
            o.audio_dup = p.value("duplicate", false);
            o.decode_pcm = p.value("decode", true);
            o.target_backlog_ms = bounded(p, "buffer_ms", 50, 0, 1000);
            o.recorder = s.recorder.get();
            o.stop_device_on_exit = true;
            o.session_group = s.media_group;
            s.audio = media::AudioPump::start(s.device, o, error);
            s.pcm_started = false;
        }
        if (!s.audio)
            return fail(error);
        Reply r;
        r.result = {{"sample_rate", s.audio->sample_rate()},
                    {"channels", s.audio->channels()},
                    {"preroll", s.audio->preroll_frames()}};
        return r;
    }
    if (method == "audio.stop") {
        s.audio.reset();
        return {};
    }
    if (method == "audio.read") {
        if (!s.audio || !s.audio->decoding_enabled())
            return Reply::failure("invalid_state", "PCM audio is not running");
        size_t frames = bounded(p, "frames", 960, 1, 48000);
        std::lock_guard pcm_lock(s.pcm_mutex);
        Reply r;
        if (!s.pcm_started && s.audio->buffered_frames() < s.audio->preroll_frames()) {
            r.result = {{"buffered", s.audio->buffered_frames()}, {"frames", 0}};
            return r;
        }
        s.pcm_started = true;
        r.result = {{"buffered", s.audio->buffered_frames()}};
        std::vector<int16_t> pcm(frames * s.audio->channels());
        frames = s.audio->read(pcm.data(), frames);
        r.binary.resize(frames * s.audio->channels() * 2);
        // Protocol PCM is explicitly little endian, independent of the server CPU.
        for (size_t i = 0; i < frames * s.audio->channels(); ++i) {
            r.binary[2 * i] = uint16_t(pcm[i]) & 255;
            r.binary[2 * i + 1] = uint16_t(pcm[i]) >> 8;
        }
        r.result["frames"] = frames;
        return r;
    }
    if (method == "video.start") {
        if (!kHaveDecoder && p.value("decode", true))
            return Reply::failure("unsupported", "No HEVC decoder in this build");
        if (!s.video) {
            if (!s.display)
                s.display = remote::DisplayWatcher::start(s.device, 1, error);
            media::FramePump::Options options;
            options.session_group = s.media_group;
            options.use_hardware = p.value("hardware_decode", false);
            options.decode_video = p.value("decode", true);
            options.recorder = s.recorder.get();
            options.record_path = p.value("record_path", std::string());
            if (!options.record_path.empty() && !std::filesystem::u8path(options.record_path).is_absolute())
                return Reply::failure("invalid_params",
                                      "Raw recording path must be absolute on the server host");
            options.silence_restart_ms = p.value("restart_on_silence", true) ? 3000 : 0;
            options.frame_geometry = [&s]() -> std::optional<FrameGeometry> {
                if (!s.display)
                    return std::nullopt;
                auto g = s.display->latest();
                return FrameGeometry{g.width, g.height, g.orientation, g.seq};
            };
            s.video = media::FramePump::start(s.device, options, error);
        }
        if (!s.video)
            return fail(error);
        if (!p.value("decode", true) && !s.video->wait_ready(5000)) {
            error = s.video->capture_error();
            s.video.reset();
            return fail(error.empty() ? "No valid recording IDR within five seconds" : error);
        }
        return {};
    }
    if (method == "video.stop") {
        s.video.reset();
        return {};
    }
    if (method == "video.frame") {
        if (!s.video)
            return Reply::failure("invalid_state", "Video is not running");
        Frame frame;
        auto timeout = bounded(p, "timeout_ms", 50, 0, 1000);
        const auto since = p.value("since", uint64_t(0));
        auto serial = s.video->newer(frame, since, timeout);
        if (!serial && p.value("allow_cached", false) && !s.video->reviving() && !s.video->video_unusable() &&
            s.video->latest(frame, 0))
            serial = s.video->serial();
        if (!serial)
            return Reply::failure("no_frame", "No newer frame within the requested timeout");
        Reply r;
        r.result = {{"format", "bgra"},
                    {"serial", serial},
                    {"width", frame.width},
                    {"height", frame.height},
                    {"row_pitch", frame.row_pitch}};
        if (frame.geometry) {
            auto geometry = *frame.geometry;
            r.result["display"] = {{"width", geometry.width},
                                   {"height", geometry.height},
                                   {"orientation", geometry.orientation},
                                   {"serial", geometry.serial}};
        }
        r.binary = std::move(frame.pixels);
        return r;
    }
    if (method == "app.list") {
        std::vector<remote::App::Entry> apps;
        if (!remote::App::list(s.device, apps, error))
            return fail(error);
        Reply r;
        r.result = Json::array();
        for (const auto &app : apps)
            r.result.push_back({{"bundle_id", app.bundle_id}, {"name", app.name}});
        return r;
    }
    if (method == "app.launch" || method == "app.stop") {
        const auto bundle = p.at("bundle_id").get<std::string>();
        if (bundle.empty())
            return Reply::failure("invalid_params", "bundle_id is required");
        bool ok = method == "app.launch"
                      ? remote::App::launch(s.device, bundle, error, p.value("terminate_existing", false))
                      : remote::App::stop(s.device, bundle, error);
        return ok ? Reply{} : fail(error);
    }
    if (method.starts_with("input."))
        return handle_input(method, p, cancel, s);
    return Reply::failure("unknown_method", "Unknown method: " + method);
}

Reply Backend::handle_management(const std::string &method, const Json &p, const Event &event, const Cancel &cancel) {
    auto fail = [](const std::string &error) { return Reply::failure("operation_failed", error); };
    std::string error;
    if (method == "devices") {
        remote::DiscoveryOptions o;
        o.pairing_directory = directory_;
        o.should_cancel = cancel;
        o.timeout = std::chrono::milliseconds(bounded(p, "timeout_ms", 3000, 0, 60000));
        auto mode = p.value("transport", std::string("auto"));
        if (mode != "usb" && mode != "wifi" && mode != "auto")
            return Reply::failure("invalid_params", "Unknown transport");
        o.include_usb = mode != "wifi";
        o.include_wifi = mode != "usb";
        auto value = remote::discover_devices(o);
        Reply r;
        std::set<std::string> connected;
        std::vector<std::shared_ptr<Session>> sessions;
        {
            std::lock_guard lock(registry_.mutex);
            for (const auto &[key, weak] : registry_.sessions) {
                if (auto session = std::static_pointer_cast<Session>(weak.lock()))
                    sessions.push_back(std::move(session));
            }
        }
        for (const auto &session : sessions)
            connected.insert(session->udid);
        r.result = discovery_json(value, connected);
        r.result["capabilities"] = {{"video_decode", kHaveDecoder}};
        return r;
    }
    if (method == "pair") {
        auto udid = p.value("udid", std::string());
        auto lease = udid.empty() ? std::string("@pairing") : udid;
        if (!registry_.acquire(lease))
            return Reply::failure("device_busy", "Device is already in use");
        struct Release {
            Registry &r;
            std::string id;
            ~Release() { r.release(id); }
        } release{registry_, lease};
        auto progress = [&](std::string_view text) { event("progress", {{"message", text}}); };
        remote::PairingResult value;
        auto mode = p.value("transport", std::string("wifi"));
        if (mode != "wifi" && mode != "usb")
            return Reply::failure("invalid_params", "Choose usb or wifi pairing");
        if (mode == "wifi") {
            remote::WifiPairingOptions o;
            o.udid = udid;
            o.pairing_directory = directory_;
            o.should_cancel = cancel;
            o.progress = progress;
            o.timeout_ms = bounded(p, "timeout_ms", 120000, 1, 300000);
            o.display_pin = [&](std::string_view pin, std::string &) {
                event("pin", {{"pin", pin}});
                return !cancel();
            };
            value = remote::pair_wifi_remote(o);
        } else {
            remote::UsbPairingOptions o;
            o.udid = udid;
            o.pairing_directory = directory_;
            o.should_cancel = cancel;
            o.progress = progress;
            o.timeout_ms = bounded(p, "timeout_ms", 120000, 1, 300000);
            o.allow_repair = p.value("repair", false);
            value = remote::pair_usb_remote(o);
        }
        if (!value.ok)
            return fail(value.error);
        Reply r;
        r.result = {{"udid", value.udid}, {"reused", value.reused}};
        return r;
    }
    if (method == "ddi.download") {
        ddi::DownloadOptions o;
        o.cancelled = cancel;
        o.timeout = std::chrono::seconds(bounded(p, "timeout_seconds", 300, 1, 3600));
        auto cache = p.value("cache_directory", std::string());
        if (!cache.empty())
            o.cache_root = std::filesystem::u8path(cache);
        o.progress = [&](const std::string &file, uint64_t bytes, uint64_t total) {
            event("progress", {{"file", file}, {"bytes", bytes}, {"total", total}});
        };
        ddi::DownloadTarget target;
        target.system_version = p.at("os_version").get<std::string>();
        target.product_type = p.value("product_type", std::string());
        auto image = ddi::download_for_target(o, target, error);
        if (!image)
            return fail(error);
        Reply r;
        r.result = {{"build", image->build}, {"cache_hit", image->cache_hit}};
        return r;
    }
    if (method == "ddi.prepare" || method == "ddi.status" || method == "ddi.remove") {
        PrepareOptions o;
        o.serial = p.at("udid").get<std::string>();
        o.wifi = p.value("wifi", std::string());
        if (o.serial.empty())
            return Reply::failure("invalid_params", "UDID is required");
        if (!registry_.acquire(o.serial))
            return Reply::failure("device_busy", "Close the device session before managing DDI");
        struct Release {
            Registry &r;
            std::string id;
            ~Release() { r.release(id); }
        } release{registry_, o.serial};
        o.data_directory = directory_;
        o.wifi_port = uint16_t(bounded(p, "port", 49152, 1, 65535));
        if (method != "ddi.prepare") {
            auto result = manage_ddi(o, method == "ddi.remove", error, cancel);
            if (!result)
                return fail(error);
            Reply r;
            r.result = std::move(*result);
            return r;
        }
        o.ddi_directory = p.value("cache_directory", std::string());
        o.replace_ddi = p.value("replace", false);
        o.ddi_download_timeout = bounded(p, "download_timeout_seconds", 300, 1, 3600);
        o.ddi_install_timeout = bounded(p, "install_timeout_seconds", 300, 1, 3600);
        if (!prepare_ddi(o, error, event, cancel))
            return fail(error);
        Reply r;
        r.result = {{"prepared", true}};
        return r;
    }
    return Reply::failure("unknown_method", "Unknown method: " + method);
}

Reply Backend::handle_connection(const std::string &method, const Json &p, const Cancel &cancel) {
    auto fail = [](const std::string &error) { return Reply::failure("operation_failed", error); };
    std::string error;
    if (method == "connection.close") {
        close();
        return {};
    }
    if (method == "connection.attach") {
        if (session_)
            return Reply::failure("invalid_state", "This client already owns a session");
        auto key = p.at("attachment").get<std::string>();
        {
            std::lock_guard lock(registry_.mutex);
            auto found = registry_.sessions.find(key);
            if (found != registry_.sessions.end())
                session_ = std::static_pointer_cast<Session>(found->second.lock());
        }
        if (!session_)
            return Reply::failure("invalid_session", "Session attachment is no longer available");
        return {};
    }
    if (method == "connection.open") {
        if (session_)
            return Reply::failure("invalid_state", "This client already owns a device session");
        auto udid = p.at("udid").get<std::string>();
        auto mode = p.value("transport", std::string("auto"));
        if (mode != "auto" && mode != "usb" && mode != "wifi")
            return Reply::failure("invalid_params", "Unknown transport");
        if (udid.empty()) {
            remote::DiscoveryOptions o;
            o.pairing_directory = directory_;
            o.should_cancel = cancel;
            o.include_wifi = mode != "usb";
            o.include_usb = mode != "wifi";
            auto found = remote::discover_devices(o);
            for (const auto &d : found.devices) {
                if (d.udid.empty())
                    continue;
                if (!udid.empty())
                    return Reply::failure("ambiguous_device", "Multiple devices; select a UDID");
                udid = d.udid;
            }
            if (udid.empty())
                return Reply::failure("device_not_found", "No identified device available");
        }
        if (!registry_.acquire(udid))
            return Reply::failure("device_busy", "Device is already in use");
        lease_ = udid;
        try {
            auto address = p.value("wifi", std::string());
            if (mode == "wifi" && address.empty())
                address = "auto";
            auto device =
                mode == "usb"
                    ? remote::Device::establish_bounded(
                          udid, transport::IoBudget({std::chrono::seconds(30), cancel}), error, false, true)
                    : open_device(udid, address, error, uint16_t(bounded(p, "port", 49152, 1, 65535)), cancel,
                                  directory_);
            if (!device) {
                close();
                return fail(error);
            }
            session_ = std::make_shared<Session>(registry_, std::move(*device));
            lease_.clear(); // Session releases the device only after its last attachment closes.
            auto key = wifi::random_bytes(32, error);
            if (!key) {
                close();
                return fail(error);
            }
            static constexpr char hex[] = "0123456789abcdef";
            for (auto byte : *key) {
                session_->attachment += hex[byte >> 4];
                session_->attachment += hex[byte & 15];
            }
            std::lock_guard lock(registry_.mutex);
            registry_.sessions[session_->attachment] = session_;
        } catch (...) {
            close();
            throw;
        }
        Reply r;
        r.result = {{"udid", session_->device.udid()}, {"attachment", session_->attachment}};
        return r;
    }
    return Reply::failure("unknown_method", "Unknown method: " + method);
}

Reply Backend::handle_input(const std::string &method, const Json &p, const Cancel &cancel, Session &s) {
    auto fail = [](const std::string &error) { return Reply::failure("operation_failed", error); };
    std::string error;
    if (method == "input.text") {
        auto text = p.at("text").get<std::string>();
        if (text.size() > 4096)
            return Reply::failure("invalid_params", "Text exceeds 4096 bytes");
        for (unsigned char c : text)
            if ((c < 32 && c != '\t' && c != '\n') || c > 126)
                return Reply::failure("unsupported", "Only US keyboard ASCII text is supported");
        const int hold = bounded(p, "hold_ms", 0, 0, 1000);
        const int timeout = bounded(p, "timeout_ms", 30000, 1, 300000);
        if (s.video)
            s.video->wake();
        const transport::IoBudget budget({std::chrono::milliseconds(timeout), cancel});
        std::stop_source stop;
        transport::BudgetWatchdog watch(budget, [&] { stop.request_stop(); });
        // A per-request endpoint lets cancellation interrupt this send without
        // breaking another attached client's media/control service.
        auto typing = hid::Service::open(s.device, error, false, stop.get_token());
        const bool delivered = typing && typing->type_text(text, hold, error, stop.get_token());
        const bool in_budget = watch.finish(error);
        if (!in_budget || !delivered)
            return fail(error);
        s.hid = std::move(typing);
        return {};
    }
    if (!s.hid)
        s.hid = hid::Service::open(s.device, error);
    if (!s.hid)
        return fail(error);
    if (s.video)
        s.video->wake();
    if (method == "input.touch" || method == "input.tap") {
        auto x = p.at("x").get<double>(), y = p.at("y").get<double>();
        if (!unit(x) || !unit(y))
            return Reply::failure("invalid_params", "Touch coordinates must be in [0,1]");
        s.x = x;
        s.y = y;
        if (method == "input.tap") {
            s.touching = true;
            const bool ok = s.hid->tap(x, y, bounded(p, "hold_ms", 30, 0, 2000), error);
            if (ok)
                s.touching = false;
            return ok ? Reply{} : fail(error);
        }
        bool down = p.at("down").get<bool>();
        if (down)
            s.touching = true;
        const bool ok = s.hid->touch(hid::kSurfaceMainTouchscreen, x, y, down, error);
        if (ok)
            s.touching = down;
        return ok ? Reply{} : fail(error);
    }
    if (method == "input.keyboard") {
        auto usages = p.at("usages").get<std::vector<uint16_t>>();
        for (auto key : usages)
            if (key >= 240)
                return Reply::failure("invalid_params", "Keyboard usage must be below 240");
        s.keys = {usages.begin(), usages.end()};
        return s.hid->send_report(hid::kSurfaceKeyboard, hid::keyboard_report(usages), error)
                   ? Reply{}
                   : fail(error);
    }
    if (method == "input.button") {
        auto page = uint16_t(bounded(p, "page", 12, 0, 65535)),
             code = uint16_t(bounded(p, "usage", 0, 0, 65535));
        bool down = p.at("down").get<bool>();
        if (!s.buttons)
            s.buttons = hid::Buttons::open(s.device, error);
        if (!s.buttons)
            return fail(error);
        if (down)
            s.held_buttons.insert({page, code});
        const bool ok = down ? s.buttons->down(page, code, error) : s.buttons->release(page, code, error);
        if (ok && !down)
            s.held_buttons.erase({page, code});
        return ok ? Reply{} : fail(error);
    }
    return Reply::failure("unknown_method", "Unknown method: " + method);
}
} // namespace xdb::server
