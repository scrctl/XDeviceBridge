// SPDX-License-Identifier: LGPL-3.0-only
#include "Backend.h"
#include "ApiParameters.h"
#include "ipc/Endpoint.h"
#include "Discovery.h"
#include "ddi/Download.h"
#include "hid/Hid.h"
#include "hid/TouchState.h"
#include "media/AudioPump.h"
#include "media/FramePump.h"
#include "media/OutputRate.h"
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
#include "wifi/HostMetadata.h"
#include "wifi/Mdns.h"
#include "wifi/PairRecord.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
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
std::optional<std::string> attachment_key(std::string &error) {
    auto bytes = wifi::random_bytes(32, error);
    if (!bytes) return std::nullopt;
    static constexpr char hex[] = "0123456789abcdef";
    std::string key;
    for (auto byte : *bytes) { key += hex[byte >> 4]; key += hex[byte & 15]; }
    return key;
}
bool unit(double n) { return std::isfinite(n) && n >= 0 && n <= 1; }
} // namespace
std::optional<remote::Device> open_device(const std::string &udid, const std::string &wifi_address,
                                          std::string &error, uint16_t port, const Cancel &cancel,
                                          const std::string &directory, transport::DeviceError *failure) {
    using transport::DeviceError;
    DeviceError wifi_failure = DeviceError::other, usb_failure = DeviceError::other;
    if (failure) *failure = DeviceError::other;
    transport::IoBudget budget({std::chrono::seconds(30), cancel});
    std::string usb_error;
    if (wifi_address.empty()) {
        auto usb = remote::Device::establish_bounded(udid, budget, error, false, true, &usb_failure);
        if (usb) return usb;
        if (cancel()) return std::nullopt;
        usb_error = std::move(error);
        error.clear();
    }
    const auto try_wifi = [&]() -> std::optional<remote::Device> {
        auto paired = records(directory, cancel);
        auto record = std::find_if(paired.begin(), paired.end(), [&](const auto &r) {
            return r.udid == udid && r.complete() && r.has_peer_identity();
        });
        if (record == paired.end()) {
            error = "No usable local Wi-Fi pairing record for the requested UDID" +
                    transport::device_error_hint(DeviceError::not_paired, true);
            wifi_failure = DeviceError::not_paired;
            return std::nullopt;
        }
        if (!wifi_address.empty() && wifi_address != "auto")
            return remote::Device::establish_wifi_bounded(wifi_address, *record, budget, error, false, port, &wifi_failure);
        remote::DiscoveryOptions options;
        options.include_usb = false;
        options.pairing_directory = directory;
        options.should_cancel = cancel;
        auto discovery = remote::discover_devices(options);
        for (const auto &device : discovery.devices) {
            if (device.udid != udid) continue;
            for (const auto &c : device.candidates) {
                if (c.transport != remote::DiscoveryTransport::remote_pairing ||
                    c.pairing != remote::DiscoveryPairing::ready || c.address.empty()) continue;
                auto connected = remote::Device::establish_wifi_bounded(c.address, *record, budget, error,
                                                                         false, c.port, &wifi_failure);
                if (connected) return connected;
                if (cancel()) return std::nullopt;
            }
        }
        if (error.empty()) error = "No paired Wi-Fi device discovered";
        return std::nullopt;
    };
    auto device = try_wifi();
    if (device) return device;
    if (!usb_error.empty()) {
        error = "USB: " + usb_error + "; Wi-Fi: " + error;
        // Mixed failures are not evidence that both transports are unauthorized.
        if (wifi_failure == DeviceError::not_paired && usb_failure != DeviceError::other)
            wifi_failure = usb_failure;
        else if (usb_failure != DeviceError::other && usb_failure != wifi_failure)
            wifi_failure = DeviceError::other;
    }
    if (failure && !cancel()) *failure = wifi_failure;
    return std::nullopt;
}

struct DeviceConnection {
    remote::Device device;
    std::string address;
    uint16_t port;
    std::atomic<bool> closed{false};
    DeviceConnection(remote::Device value, std::string host = {}, uint16_t p = 49152)
        : device(std::move(value)), address(std::move(host)), port(p) {}
    bool alive() { return !closed && device.stack() && device.stack()->pumping(); }
    void interrupt() {
        if (!closed.exchange(true)) device.interrupt_startup();
    }
};
bool Registry::acquire(const std::string &udid, bool use_connection) {
    std::lock_guard lock(mutex);
    if (devices.contains("@pairing") ||
        (udid == "@pairing" && (!devices.empty() || !connections.empty())) ||
        (!use_connection && connections.contains(udid))) return false;
    return devices.insert(udid).second;
}
struct Backend::Session {
    // Reverse destruction keeps all services/worker references inside Device's lifetime.
    Registry &registry;
    std::string udid, attachment, read_attachment;
    unsigned control_clients;
    bool shared_mode = false;
    // Non-owning entries are protected by control_mutex, including destruction.
    std::map<std::string, SharedControl *> shared_controls;
    Json video_profile;
    media::OutputRateRequests video_rates;
    std::string audio_owner, touch_owner;
    std::shared_mutex media_mutex;
    std::mutex control_mutex, pcm_mutex;
    bool pcm_started = false;
    std::shared_ptr<DeviceConnection> connection;
    remote::Device &device;
    bool retired = false;
    std::shared_ptr<media::SessionGroup> media_group = std::make_shared<media::SessionGroup>();
    std::unique_ptr<remote::DisplayWatcher> display;
    std::unique_ptr<hid::Service> hid;
    std::unique_ptr<hid::Buttons> buttons;
    std::unique_ptr<media::Recorder> recorder;
    std::unique_ptr<media::FramePump> video;
    std::unique_ptr<media::AudioPump> audio;
    std::set<uint16_t> keys;
    std::set<std::pair<uint16_t, uint16_t>> held_buttons;
    hid::TouchState touches;
    Session(Registry &r, std::shared_ptr<DeviceConnection> value, bool read_only)
        : registry(r), udid(value->device.udid()), control_clients(read_only ? 0 : 1),
          connection(std::move(value)), device(connection->device) {}
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
    void release_inputs() {
        std::string ignored;
        if (hid && !connection->closed) {
            touches.release([&](auto contacts, auto &error) { return hid->touches(contacts, error); }, ignored);
            hid->send_report(hid::kSurfaceKeyboard, hid::keyboard_report({}), ignored);
            // send_only queues reports; the last session is about to destroy its tunnel.
            // Wait for transport acknowledgement, not an optional HID business reply.
            hid->drain(1000, ignored);
        }
        if (buttons && !connection->closed)
            for (auto [page, code] : held_buttons) buttons->release(page, code, ignored);
        touches = {};
        keys.clear();
        held_buttons.clear();
        buttons.reset();
        hid.reset();
    }
    void detach_control_locked() {
        if (retired || control_clients == 0 || --control_clients != 0) return;
        std::string ignored;
        finish(ignored);
        release_inputs();
        shared_mode = false;
        video_profile = Json();
        video_rates.clear();
        audio_owner.clear();
        touch_owner.clear();
        // Readers may retain the Device for screenshots, but not its old media
        // ownership or a capability that could revive a departed controller.
        std::lock_guard lock(registry.mutex);
        registry.sessions.erase(attachment);
        attachment.clear();
    }
    void detach_control() {
        std::unique_lock media_lock(media_mutex);
        std::lock_guard control_lock(control_mutex);
        detach_control_locked();
    }
    void detach_shared(SharedControl &);
    void shutdown() {
        std::unique_lock media_lock(media_mutex);
        std::lock_guard control_lock(control_mutex);
        if (retired) return;
        retired = true;
        control_clients = 0;
        std::string ignored;
        finish(ignored);
        release_inputs();
        display.reset();
        std::lock_guard lock(registry.mutex);
        registry.sessions.erase(attachment);
        registry.sessions.erase(read_attachment);
        for (const auto &[key, weak] : shared_controls) registry.shared_attachments.erase(key);
        registry.device_sessions.erase(udid);
        registry.devices.erase(udid);
    }
    ~Session() { shutdown(); }
};
// Each independent mirror owns one group. Its polling/job sockets share this
// object, so closing an auxiliary socket cannot release the mirror's inputs.
struct Backend::SharedControl {
    std::shared_ptr<Session> session;
    std::string attachment;
    std::set<uint16_t> keys;
    std::set<std::pair<uint16_t, uint16_t>> buttons;
    bool registered = false;
    explicit SharedControl(std::shared_ptr<Session> s) : session(std::move(s)) {}
    ~SharedControl() { if (registered) session->detach_shared(*this); }
};
void Backend::Session::detach_shared(SharedControl &owner) {
    std::unique_lock media_lock(media_mutex);
    std::lock_guard control_lock(control_mutex);
    if (retired) return;
    shared_controls.erase(owner.attachment);
    video_rates.erase(owner.attachment);
    if (video) video->set_max_fps(video_rates.effective());
    std::set<uint16_t> remaining_keys;
    std::set<std::pair<uint16_t, uint16_t>> remaining_buttons;
    for (const auto &[key, other] : shared_controls) {
        remaining_keys.insert(other->keys.begin(), other->keys.end());
        remaining_buttons.insert(other->buttons.begin(), other->buttons.end());
    }
    std::string ignored;
    if (hid && !connection->closed) {
        if (touches.active() && touch_owner == owner.attachment) {
            touches.release([&](auto contacts, auto &error) { return hid->touches(contacts, error); }, ignored);
            touches = {};
            touch_owner.clear();
        }
        // Also refresh after a failed final release, when the owner's desired
        // key set is already empty but the device may still hold its old keys.
        hid->send_report(hid::kSurfaceKeyboard,
                         hid::keyboard_report({remaining_keys.begin(), remaining_keys.end()}), ignored);
    }
    if (buttons && !connection->closed)
        for (auto [page, code] : owner.buttons)
            if (!remaining_buttons.contains({page, code})) buttons->release(page, code, ignored);
    keys = std::move(remaining_keys);
    held_buttons = std::move(remaining_buttons);
    if (audio_owner == owner.attachment) {
        audio.reset();
        audio_owner.clear();
        pcm_started = false;
    }
    {
        std::lock_guard lock(registry.mutex);
        registry.shared_attachments.erase(owner.attachment);
    }
    detach_control_locked();
}
bool Backend::has_session() const { return session_ && !session_->connection->closed; }
void Backend::disconnect_connection(Registry &registry, const std::shared_ptr<DeviceConnection> &connection) {
    // Wake blocked service I/O before waiting for the session's media/control locks.
    connection->interrupt();
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard lock(registry.mutex);
        const auto found = registry.connections.find(connection->device.udid());
        if (found != registry.connections.end() && found->second == connection)
            registry.connections.erase(found);
        for (const auto &[key, weak] : registry.device_sessions)
            if (auto session = std::static_pointer_cast<Session>(weak.lock());
                session && session->connection == connection) sessions.push_back(std::move(session));
    }
    for (const auto &session : sessions) session->shutdown();
}
void Backend::maintain_connections(Registry &registry, bool stop_all) {
    std::vector<std::shared_ptr<DeviceConnection>> dead;
    {
        std::lock_guard lock(registry.mutex);
        for (const auto &[id, connection] : registry.connections)
            if (stop_all || !connection->alive()) dead.push_back(connection);
    }
    for (const auto &connection : dead) disconnect_connection(registry, connection);
}
Backend::Backend(Registry &r, std::string dir) : registry_(r), directory_(std::move(dir)) {}
Backend::~Backend() { close(); }
void Backend::close() {
    auto session = std::move(session_);
    const bool was_read_only = std::exchange(read_only_, false);
    if (shared_control_) shared_control_.reset();
    else if (session && !was_read_only) session->detach_control();
    session.reset();
    if (!lease_.empty())
        registry_.release(lease_);
    lease_.clear();
}
Reply Backend::handle(const std::string &method, const Json &p, const Event &event, const Cancel &cancel) {
    auto fail = [](const std::string &error) { return Reply::failure("operation_failed", error); };
    std::string error;
    if (cancel())
        return Reply::failure("cancelled", "Request cancelled");
    if (read_only_ && !detail::read_only_method(method))
        return Reply::failure("read_only_session", "This session only permits screenshots and read-only queries");
    if (method == "devices" || method == "pair" || method == "ddi.download" || method == "ddi.prepare" ||
        method == "ddi.status" || method == "ddi.remove" || method == "mdns.check" || method == "mdns.services")
        return handle_management(method, p, event, cancel);
    if (method == "connect" || method == "disconnect")
        return handle_persistent_connection(method, p, cancel);
    if (method == "connection.close" || method == "connection.attach" || method == "connection.open")
        return handle_connection(method, p, cancel);
    if (method.starts_with("input.") && method != "input.text" && method != "input.tap" &&
        method != "input.touch" && method != "input.keyboard" && method != "input.button")
        return Reply::failure("unknown_method", "Unknown method: " + method);
    if (!has_session())
        return Reply::failure("not_connected", "Open a device session first (or reopen after disconnect)");
    auto &s = *session_;
    if (shared_control_ && (method == "recording.configure" || method == "media.finish" || method == "video.stop"))
        return Reply::failure("exclusive_session_required",
                              "Recording and explicit media shutdown require an exclusive session; close your shared association instead");
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
    if (s.connection->closed)
        return Reply::failure("not_connected", "Device disconnected; open a new session");
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
                    {"recording_error", recording_error},
                    {"video_max_fps", s.video ? s.video->max_fps() : 0},
                    {"video_rate_limited", s.video ? s.video->stats().rate_limited : uint64_t(0)},
                    {"video_max_bitrate", s.video ? s.video->max_bitrate() : 0}};
        const auto video_stats = s.video ? s.video->stats() : media::FramePump::Stats{};
        r.result["video_decoder"] = video_stats.decoder;
        r.result["video_hardware_decode"] = video_stats.hardware_decode ? Json(*video_stats.hardware_decode) : Json(nullptr);
        const auto negotiated = s.video ? s.video->negotiated_max_bitrate() : std::nullopt;
        r.result["video_negotiated_max_bitrate"] = negotiated ? Json(*negotiated) : Json(nullptr);
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
        if (shared_control_ && s.audio && s.audio_owner != shared_control_->attachment)
            return Reply::failure("device_busy", "Audio already has a consumer in another mirror");
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
            if (s.audio && shared_control_) s.audio_owner = shared_control_->attachment;
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
        if (shared_control_ && s.audio && s.audio_owner != shared_control_->attachment)
            return Reply::failure("device_busy", "Audio belongs to another mirror");
        s.audio.reset();
        s.audio_owner.clear();
        return {};
    }
    if (method == "audio.read") {
        if (shared_control_ && s.audio && s.audio_owner != shared_control_->attachment)
            return Reply::failure("device_busy", "Audio belongs to another mirror");
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
        const unsigned max_fps = detail::video_max_fps(p);
        const uint64_t max_bitrate = detail::video_max_bitrate(p);
        if (max_fps && !p.value("decode", true))
            return Reply::failure("invalid_params", "max_fps requires decoded video");
#ifndef XDB_HAVE_LIBAV
        if (p.value("decode", true) && !p.value("hardware_decode", true))
            return Reply::failure("unsupported", "Software video decoding is not included in this build");
#endif
        Json profile;
        if (shared_control_) {
            if (!p.value("decode", true) || !p.value("record_path", std::string()).empty())
                return Reply::failure("exclusive_session_required", "Recording requires an exclusive session");
            profile = {{"hardware_decode", p.value("hardware_decode", true)}, {"decode", true},
                       {"restart_on_silence", p.value("restart_on_silence", true)}};
            if (s.video && profile != s.video_profile)
                return Reply::failure("media_conflict", "Shared video parameters differ from the running stream");
        }
        if (!kHaveDecoder && p.value("decode", true))
            return Reply::failure("unsupported", "No HEVC decoder in this build");
        if (!s.video) {
            if (!s.display)
                s.display = remote::DisplayWatcher::start(s.device, 1, error);
            media::FramePump::Options options;
            options.session_group = s.media_group;
            options.decode_video = p.value("decode", true);
            options.use_hardware = p.value("hardware_decode", options.decode_video);
            options.max_fps = max_fps;
            options.offer.max_bitrate = max_bitrate;
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
            if (s.video && shared_control_) s.video_profile = std::move(profile);
        }
        if (!s.video)
            return fail(error);
        if (shared_control_) {
            s.video_rates.set(shared_control_->attachment, max_fps);
            s.video->set_max_fps(s.video_rates.effective());
        } else {
            s.video->set_max_fps(max_fps);
        }
        if (!p.value("decode", true) && !s.video->wait_ready(5000)) {
            error = s.video->capture_error();
            s.video.reset();
            return fail(error.empty() ? "No valid recording IDR within five seconds" : error);
        }
        Reply r;
        r.result["max_bitrate"] = s.video->max_bitrate();
        const auto negotiated = s.video->negotiated_max_bitrate();
        r.result["negotiated_max_bitrate"] = negotiated ? Json(*negotiated) : Json(nullptr);
        if (p.contains("max_bitrate") && max_bitrate != s.video->max_bitrate()) {
            const auto warning = "Ignoring max_bitrate=" + std::to_string(max_bitrate) +
                "; reusing running stream max_bitrate=" + std::to_string(s.video->max_bitrate()) +
                " (0 uses device defaults)";
            r.result["warnings"] = Json::array({warning});
            std::fprintf(stderr, "Warning: %s\n", warning.c_str());
        }
        return r;
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
    if (method == "mdns.check" || method == "mdns.services") {
        wifi::mdns::BrowseOptions options;
        options.timeout = std::chrono::milliseconds(bounded(p, "timeout_ms", 3000, 1, 60000));
        options.should_cancel = cancel;
        const auto found = wifi::mdns::browse(options);
        Reply r;
        r.result = {{"backend", found.backend}, {"available", found.available},
                    {"cancelled", found.cancelled}, {"warnings", found.warnings},
                    {"service_type", "_remotepairing._tcp.local."}};
        if (method == "mdns.services") {
            auto services = Json::array();
            for (const auto &ad : found.advertisements) {
                auto endpoints = Json::array();
                for (const auto &ep : ad.endpoints)
                    endpoints.push_back({{"address", ep.address}, {"port", ep.port},
                                         {"interface", ep.interface_name}, {"interface_index", ep.interface_index}});
                services.push_back({{"instance", ad.instance}, {"name", ad.name},
                                    {"target", ad.target}, {"endpoints", std::move(endpoints)}});
            }
            r.result["services"] = std::move(services);
        }
        return r;
    }
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
        std::vector<std::shared_ptr<DeviceConnection>> connections;
        {
            std::lock_guard lock(registry_.mutex);
            for (const auto &[id, connection] : registry_.connections)
                if (connection->alive()) connections.push_back(connection);
            for (const auto &[key, weak] : registry_.device_sessions) {
                if (auto session = std::static_pointer_cast<Session>(weak.lock()))
                    sessions.push_back(std::move(session));
            }
        }
        for (const auto &session : sessions)
            if (!session->connection->closed) connected.insert(session->udid);
        for (const auto &connection : connections) connected.insert(connection->device.udid());
        r.result = discovery_json(value, connected);
        if (mode != "usb")
            for (const auto &connection : connections)
                merge_manual_connection(r.result, connection->device.udid(), connection->device.property("DeviceName"),
                                        connection->address, connection->port);
        r.result["capabilities"] = {{"video_decode", kHaveDecoder}};
        return r;
    }
    if (method == "pair") {
        const auto display_brand = p.value("display_brand", std::string("xdb"));
        if (!wifi::validate_display_brand(display_brand, error))
            return Reply::failure("invalid_params", error);
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
            o.display_brand = display_brand;
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
            o.display_brand = display_brand;
            o.udid = udid;
            o.pairing_directory = directory_;
            o.should_cancel = cancel;
            o.progress = progress;
            o.timeout_ms = bounded(p, "timeout_ms", 120000, 1, 300000);
            o.allow_repair = p.value("repair", false);
            value = remote::pair_usb_remote(o);
        }
        if (!value.ok)
            return Reply::failure(transport::device_error_code(value.error_code), value.error);
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
            return Reply::failure("device_busy", "Close device sessions and disconnect manual connections before managing DDI");
        struct Release {
            Registry &r;
            std::string id;
            ~Release() { r.release(id); }
        } release{registry_, o.serial};
        o.data_directory = directory_;
        o.wifi_port = uint16_t(bounded(p, "port", 49152, 1, 65535));
        transport::DeviceError failure;
        if (method != "ddi.prepare") {
            auto result = manage_ddi(o, method == "ddi.remove", error, cancel, &failure);
            if (!result)
                return Reply::failure(transport::device_error_code(failure), error);
            Reply r;
            r.result = std::move(*result);
            return r;
        }
        o.ddi_directory = p.value("cache_directory", std::string());
        o.replace_ddi = p.value("replace", false);
        o.ddi_download_timeout = bounded(p, "download_timeout_seconds", 300, 1, 3600);
        o.ddi_install_timeout = bounded(p, "install_timeout_seconds", 300, 1, 3600);
        if (!prepare_ddi(o, error, event, cancel, &failure))
            return Reply::failure(transport::device_error_code(failure), error);
        Reply r;
        r.result = {{"prepared", true}};
        return r;
    }
    return Reply::failure("unknown_method", "Unknown method: " + method);
}

Reply Backend::handle_persistent_connection(const std::string &method, const Json &p, const Cancel &cancel) {
    // Explicit endpoints never invoke discovery. A sole usable local record may
    // choose the identity, but pair-verify must still authenticate the endpoint.
    const auto udid = p.value("udid", std::string());
    const auto address = p.value("address", std::string());
    std::optional<ipc::Endpoint> endpoint;
    if (!address.empty()) endpoint = ipc::parse_endpoint(address);
    if (p.contains("port")) {
        if (!endpoint) return Reply::failure("invalid_params", "port requires an address");
        endpoint->port = uint16_t(bounded(p, "port", 49152, 1, 65535));
    }
    maintain_connections(registry_);
    if (method == "disconnect") {
        std::vector<std::shared_ptr<DeviceConnection>> selected;
        {
            std::lock_guard lock(registry_.mutex);
            for (const auto &[id, connection] : registry_.connections)
                if ((udid.empty() || id == udid) &&
                    (!endpoint || (endpoint->address == connection->address && endpoint->port == connection->port)))
                    selected.push_back(connection);
        }
        Reply reply;
        reply.result["disconnected"] = Json::array();
        for (const auto &connection : selected) {
            disconnect_connection(registry_, connection);
            reply.result["disconnected"].push_back({{"udid", connection->device.udid()},
                                                     {"address", connection->address}, {"port", connection->port}});
        }
        return reply;
    }
    if (!endpoint) return Reply::failure("invalid_params", "connect requires an address");
    const auto timeout = bounded(p, "timeout_ms", 30000, 1, 300000);
    auto paired = records(directory_, cancel);
    const wifi::PairRecord *record = nullptr;
    for (const auto &candidate : paired) {
        if (!candidate.complete() || !candidate.has_peer_identity() || (!udid.empty() && candidate.udid != udid))
            continue;
        if (record && candidate.udid != record->udid)
            return Reply::failure("ambiguous_device", "Multiple paired devices; select one with -s UDID");
        record = &candidate;
    }
    if (!record) return Reply::failure("not_paired", "No usable local Wi-Fi pairing record" +
        transport::device_error_hint(transport::DeviceError::not_paired, true));
    const auto result = [](const std::shared_ptr<DeviceConnection> &connection, bool reused) {
        Reply reply;
        reply.result = {{"udid", connection->device.udid()}, {"address", connection->address},
                        {"port", connection->port}, {"reused", reused}};
        return reply;
    };
    {
        std::lock_guard lock(registry_.mutex);
        if (auto found = registry_.connections.find(record->udid); found != registry_.connections.end()) {
            if (found->second->alive()) return result(found->second, true);
            return Reply::failure("not_connected", "Previous connection is closing; connect again after cleanup");
        }
    }
    if (!registry_.acquire(record->udid)) return Reply::failure("device_busy", "Device is already in use");
    struct Release {
        Registry &registry;
        const std::string &udid;
        ~Release() { registry.release(udid); }
    } release{registry_, record->udid};
    std::string error;
    const transport::IoBudget budget({std::chrono::milliseconds(timeout), cancel});
    transport::DeviceError failure;
    auto device = remote::Device::establish_wifi_bounded(endpoint->address, *record, budget, error, false, endpoint->port, &failure);
    if (!device) return Reply::failure(transport::device_error_code(failure), error);
    if (!device->enable_idle_keepalive(error)) return Reply::failure("operation_failed", error);
    if (!budget.check(error)) return Reply::failure("cancelled", error);
    auto connection = std::make_shared<DeviceConnection>(std::move(*device), endpoint->address, endpoint->port);
    if (!connection->alive()) return Reply::failure("not_connected", "Device disconnected during authentication");
    {
        std::lock_guard lock(registry_.mutex);
        if (cancel()) return Reply::failure("cancelled", "Connection request cancelled");
        registry_.connections.emplace(record->udid, connection);
    }
    return result(connection, false);
}

Reply Backend::bind_shared(const std::shared_ptr<Session> &session, const std::string &mode,
                           const std::shared_ptr<SharedControl> &attached) {
    std::unique_lock media_lock(session->media_mutex);
    std::lock_guard control_lock(session->control_mutex);
    if (session->retired || !session->connection->alive())
        return Reply::failure("not_connected", "Device session is no longer available");
    if (!attached) {
        const bool wifi = session->device.connection_type() == "WiFi";
        if ((mode == "usb" && wifi) || (mode == "wifi" && !wifi))
            return Reply::failure("invalid_state", "Active session uses a different transport");
        if (session->control_clients && !session->shared_mode)
            return Reply::failure("device_busy", "Device has an exclusive control owner");
        std::string error;
        auto key = attachment_key(error);
        if (!key) return Reply::failure("operation_failed", error);
        auto owner = std::make_shared<SharedControl>(session);
        // Register only after generating the capability. The session lock must
        // be released before any registered owner's destructor can run.
        owner->attachment = std::move(*key);
        session->shared_controls[owner->attachment] = owner.get();
        try {
            std::lock_guard lock(registry_.mutex);
            registry_.shared_attachments[owner->attachment] = owner;
        } catch (...) {
            session->shared_controls.erase(owner->attachment);
            throw;
        }
        session->shared_mode = true;
        ++session->control_clients;
        owner->registered = true;
        shared_control_ = std::move(owner);
    } else {
        shared_control_ = attached;
    }
    session_ = session;
    read_only_ = false;
    Reply reply;
    if (!attached) reply.result = {{"udid", session->udid},
        {"attachment", shared_control_->attachment}, {"access", "shared"}};
    return reply;
}

Reply Backend::bind_existing(const std::shared_ptr<Session> &session, bool read_only,
                             const std::string &mode, const std::string &key) {
    std::unique_lock media_lock(session->media_mutex);
    std::lock_guard control_lock(session->control_mutex);
    if (session->retired || !session->connection->alive())
        return Reply::failure("not_connected", "Device session is no longer available");
    if (!key.empty()) {
        read_only = key == session->read_attachment;
        if (!read_only && (key != session->attachment || session->control_clients == 0))
            return Reply::failure("invalid_session", "Session attachment is no longer available");
        if (!read_only) ++session->control_clients;
    } else {
        const bool wifi = session->device.connection_type() == "WiFi";
        if ((mode == "usb" && wifi) || (mode == "wifi" && !wifi))
            return Reply::failure("invalid_state", "Active session uses a different transport");
        if (!read_only) {
            if (session->control_clients)
                return Reply::failure("device_busy", "Device already has a control owner; use access: read for queries");
            std::string error;
            auto fresh = attachment_key(error);
            if (!fresh) return Reply::failure("operation_failed", error);
            session->attachment = std::move(*fresh);
            std::lock_guard lock(registry_.mutex);
            registry_.sessions[session->attachment] = session;
            session->control_clients = 1;
        }
    }
    session_ = session;
    read_only_ = read_only;
    Reply reply;
    if (key.empty()) reply.result = {{"udid", session->udid},
        {"attachment", read_only ? session->read_attachment : session->attachment},
        {"access", read_only ? "read" : "control"}};
    return reply;
}

Reply Backend::handle_connection(const std::string &method, const Json &p, const Cancel &cancel) {
    auto fail = [](const std::string &error) { return Reply::failure("operation_failed", error); };
    std::string error;
    if (method == "connection.close") {
        close();
        return {};
    }
    if (method == "connection.attach") {
        if (session_) return Reply::failure("invalid_state", "This client already owns a session");
        auto key = p.at("attachment").get<std::string>();
        std::shared_ptr<Session> session;
        std::shared_ptr<SharedControl> shared;
        {
            std::lock_guard lock(registry_.mutex);
            auto found = registry_.sessions.find(key);
            if (found != registry_.sessions.end()) session = std::static_pointer_cast<Session>(found->second.lock());
            auto group = registry_.shared_attachments.find(key);
            if (group != registry_.shared_attachments.end()) shared = std::static_pointer_cast<SharedControl>(group->second.lock());
        }
        if (shared) return bind_shared(shared->session, "auto", shared);
        if (!session) return Reply::failure("invalid_session", "Session attachment is no longer available");
        return bind_existing(session, false, "auto", key);
    }
    if (method == "connection.open") {
        if (session_)
            return Reply::failure("invalid_state", "This client already owns a device session");
        const auto access = p.value("access", std::string("control"));
        if (access != "read" && access != "control" && access != "shared")
            return Reply::failure("invalid_params", "access must be read, control or shared");
        const bool read_only = access == "read";
        const bool shared = access == "shared";
        auto udid = p.at("udid").get<std::string>();
        auto mode = p.value("transport", std::string("auto"));
        if (mode != "auto" && mode != "usb" && mode != "wifi")
            return Reply::failure("invalid_params", "Unknown transport");
        if (udid.empty() && mode != "usb") {
            std::lock_guard lock(registry_.mutex);
            for (const auto &[id, connection] : registry_.connections) {
                if (!connection->alive()) continue;
                if (!udid.empty()) return Reply::failure("ambiguous_device", "Multiple connected devices; select a UDID");
                udid = id;
            }
        }
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
        const auto existing = [&]() {
            std::lock_guard lock(registry_.mutex);
            const auto found = registry_.device_sessions.find(udid);
            return found == registry_.device_sessions.end() ? std::shared_ptr<Session>()
                : std::static_pointer_cast<Session>(found->second.lock());
        };
        if (auto session = existing())
            return shared ? bind_shared(session, mode) : bind_existing(session, read_only, mode);
        if (!registry_.acquire(udid, true)) {
            if (auto session = existing())
                return shared ? bind_shared(session, mode) : bind_existing(session, read_only, mode);
            return Reply::failure("device_busy", "Device is already in use");
        }
        lease_ = udid;
        try {
            std::shared_ptr<DeviceConnection> connection;
            {
                std::lock_guard lock(registry_.mutex);
                auto found = registry_.connections.find(udid);
                if (found != registry_.connections.end()) {
                    connection = found->second;
                }
            }
            if (connection && mode == "usb") {
                close();
                return Reply::failure("device_busy", "Disconnect the manual Wi-Fi connection before using USB");
            }
            auto address = p.value("wifi", std::string());
            if (mode == "wifi" && address.empty())
                address = "auto";
            if (!connection) {
                transport::DeviceError failure;
                auto device =
                    mode == "usb"
                        ? remote::Device::establish_bounded(
                              udid, transport::IoBudget({std::chrono::seconds(30), cancel}), error, false, true, &failure)
                        : open_device(udid, address, error, uint16_t(bounded(p, "port", 49152, 1, 65535)), cancel,
                                      directory_, &failure);
                if (!device) {
                    close();
                    return Reply::failure(transport::device_error_code(failure), error);
                }
                connection = std::make_shared<DeviceConnection>(std::move(*device));
            }
            if (!connection->alive()) {
                close();
                return Reply::failure("not_connected", "Device connection was lost; connect again");
            }
            session_ = std::make_shared<Session>(registry_, std::move(connection), read_only || shared);
            read_only_ = read_only || shared;
            lease_.clear();
            auto read_key = attachment_key(error);
            auto control_key = read_only || shared ? std::optional<std::string>(std::string()) : attachment_key(error);
            if (!read_key || !control_key) { close(); return fail(error); }
            session_->read_attachment = std::move(*read_key);
            session_->attachment = std::move(*control_key);
            bool disconnected;
            {
                std::lock_guard lock(registry_.mutex);
                disconnected = session_->connection->closed;
                if (!disconnected) {
                    if (!read_only && !shared) registry_.sessions[session_->attachment] = session_;
                    registry_.sessions[session_->read_attachment] = session_;
                    registry_.device_sessions[udid] = session_;
                }
            }
            if (disconnected) {
                close();
                return Reply::failure("not_connected", "Device disconnected while opening session");
            }
        } catch (...) {
            close();
            throw;
        }
        if (shared) {
            auto result = bind_shared(session_, mode);
            if (!result.ok) close();
            return result;
        }
        Reply r;
        r.result = {{"udid", session_->device.udid()},
                    {"attachment", read_only_ ? session_->read_attachment : session_->attachment},
                    {"access", read_only_ ? "read" : "control"}};
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
        if (shared_control_ && !s.keys.empty())
            return Reply::failure("input_busy", "Release held keyboard keys before typing text in a shared session");
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
    if (method == "input.touch" || method == "input.tap") {
        const auto x = p.at("x").get<double>(), y = p.at("y").get<double>();
        if (!unit(x) || !unit(y))
            return Reply::failure("invalid_params", "Touch coordinates must be in [0,1]");
        const bool tap = method == "input.tap";
        const int contact = tap ? 0 : bounded(p, "contact", 0, 0, 4);
        const bool down = tap || p.at("down").get<bool>();
        const int hold = tap ? bounded(p, "hold_ms", 30, 0, 2000) : 0;
        if (shared_control_ && s.touches.active() && s.touch_owner != shared_control_->attachment)
            return Reply::failure("input_busy", "Another mirror is holding the touch gesture");
        if (!s.hid) s.hid = hid::Service::open(s.device, error);
        if (!s.hid) return fail(error);
        if (s.video) s.video->wake();
        if (shared_control_ && down) s.touch_owner = shared_control_->attachment;
        const auto send = [&](auto contacts, auto &failure) { return s.hid->touches(contacts, failure); };
        bool ok = s.touches.update(contact, x, y, down, send, error);
        if (tap && ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(hold));
            ok = s.touches.update(contact, x, y, false, send, error);
        }
        if (!s.touches.active()) s.touch_owner.clear();
        return ok ? Reply{} : fail(error);
    }
    if (!s.hid)
        s.hid = hid::Service::open(s.device, error);
    if (!s.hid)
        return fail(error);
    if (s.video)
        s.video->wake();
    if (method == "input.keyboard") {
        auto usages = p.at("usages").get<std::vector<uint16_t>>();
        for (auto key : usages)
            if (key >= 240)
                return Reply::failure("invalid_params", "Keyboard usage must be below 240");
        if (shared_control_) {
            shared_control_->keys = {usages.begin(), usages.end()};
            std::set<uint16_t> combined;
            for (const auto &[key, owner] : s.shared_controls)
                combined.insert(owner->keys.begin(), owner->keys.end());
            usages.assign(combined.begin(), combined.end());
        }
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
        if (shared_control_) {
            const auto button = std::pair{page, code};
            if (down) {
                shared_control_->buttons.insert(button);
                if (s.held_buttons.contains(button)) return {};
            } else {
                if (!shared_control_->buttons.contains(button)) return {};
                bool other_held = false;
                for (const auto &[key, owner] : s.shared_controls)
                    if (owner != shared_control_.get() && owner->buttons.contains(button)) other_held = true;
                if (other_held) { shared_control_->buttons.erase(button); return {}; }
            }
        }
        if (down)
            s.held_buttons.insert({page, code});
        const bool ok = down ? s.buttons->down(page, code, error) : s.buttons->release(page, code, error);
        if (ok && !down)
            s.held_buttons.erase({page, code});
        if (ok && !down && shared_control_) shared_control_->buttons.erase({page, code});
        return ok ? Reply{} : fail(error);
    }
    return Reply::failure("unknown_method", "Unknown method: " + method);
}
} // namespace xdb::server
