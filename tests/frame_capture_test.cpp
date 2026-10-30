#include "media/FramePump.h"
#include "media/AudioPump.h"
#include "media/Recorder.h"
#include "media/RecordingVideoConfig.h"
#include "media/StreamSession.h"
#include "rt/Rtcp.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <thread>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

#ifdef XDB_HAVE_LIBAVFORMAT
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}
#endif

// 运行真实 FramePump、RTP/AU/参数及 IDR 检查、Recorder。仅在本可执行文件
// 替换已有 StreamSession 网络边界及 Decoder 工厂，不造设备服务器或测试私有状态。
namespace {
using Pump = xdb::media::FramePump;
using Session = xdb::media::StreamSession;
using Recorder = xdb::media::Recorder;
using Bytes = xdb::Nal;
using namespace std::chrono_literals;
int checks = 0, failures = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct Script {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Bytes> packets;
    std::deque<Bytes> audio_packets;
    std::vector<Session::Request> starts;
    std::vector<std::pair<Bytes, uint16_t>> sent;
    std::atomic<unsigned> factories{0}, configured{0}, decoded{0}, materialized{0}, max_nal_calls{0}, stops{0}, probes{0};
    std::atomic<bool> decoder_available{false}, pixels{false};
    std::size_t platform_limit = ~std::size_t{0};
    bool local_present = true, send_ok = true;
    uint32_t local = 0;
    std::optional<Session::StartStatus> fail_next;
    bool block_recovery = false;
    std::atomic<bool> recovery_waiting{false};
    void reset() {
        std::lock_guard lock(mutex);
        packets.clear(); audio_packets.clear(); starts.clear(); sent.clear(); fail_next.reset();
        block_recovery = false; recovery_waiting = false;
        factories = configured = decoded = materialized = max_nal_calls = stops = probes = 0;
        decoder_available = pixels = false;
        platform_limit = ~std::size_t{0};
        local_present = true; local = 0; send_ok = true;
    }
    void enqueue(Bytes packet) {
        { std::lock_guard lock(mutex); packets.push_back(std::move(packet)); }
        ready.notify_all();
    }
    void enqueue_audio(Bytes packet) {
        { std::lock_guard lock(mutex); audio_packets.push_back(std::move(packet)); }
        ready.notify_all();
    }
    std::size_t start_count() { std::lock_guard lock(mutex); return starts.size(); }
} script;
template<class Predicate> bool wait(Predicate predicate, std::chrono::milliseconds limit = 2000ms) {
    const auto until = std::chrono::steady_clock::now() + limit;
    do { if (predicate()) return true; std::this_thread::sleep_for(5ms); }
    while (std::chrono::steady_clock::now() < until);
    return predicate();
}
void put32(Bytes& bytes, std::size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
}
uint32_t get32(const Bytes& bytes, std::size_t offset) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value = (value << 8) | bytes[offset + i];
    return value;
}
Bytes hex(std::string_view text) {
    Bytes bytes;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned value = 0; std::sscanf(text.data() + i, "%2x", &value);
        bytes.push_back(static_cast<uint8_t>(value));
    }
    return bytes;
}
// 固定 FFmpeg/x265 64x64、bframes=0 样例，和 Recorder 既有用例相同。
const auto vps = hex("40010c01ffff01600000030090000003000003001eba0240");
const auto sps = hex("42010101600000030090000003000003001ea020810596e92930bc05a020000003002000000303c1");
const auto pps = hex("4401c073c089");
const auto idr = hex("2801ac21800e7ffeebf349ac");
Bytes rtp(uint16_t sequence, uint32_t ticks, const Bytes& payload, bool marker = false,
          uint32_t source = 0, uint8_t type = 101) {
    Bytes packet(12, 0);
    packet[0] = 0x80; packet[1] = static_cast<uint8_t>(type | (marker ? 0x80 : 0));
    packet[2] = static_cast<uint8_t>(sequence >> 8); packet[3] = static_cast<uint8_t>(sequence);
    put32(packet, 4, ticks); put32(packet, 8, source);
    packet.insert(packet.end(), payload.begin(), payload.end()); return packet;
}
Bytes sr(uint32_t ticks, uint32_t seconds, uint32_t source = 0) {
    auto packet = xdb::rt::build_sr(source, 0, 0);
    // 设备心跳的 RC=1 形态；保留完整 24 字节报告块，使严格 SR 解析也能通过。
    packet.resize(52, 0); packet[0] = 0x81; packet[3] = 12;
    put32(packet, 8, seconds); put32(packet, 12, 0); put32(packet, 16, ticks); return packet;
}
uint16_t parameters(uint16_t sequence, uint32_t ticks = 0, uint32_t source = 0) {
    for (const auto* nal : {&vps, &sps, &pps}) script.enqueue(rtp(sequence++, ticks, *nal, false, source));
    return sequence;
}
struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("xdb-frame-capture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Directory() { std::filesystem::create_directory(path); }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
class Decoder final : public xdb::Decoder {
    std::size_t limit_;
public:
    explicit Decoder(std::size_t limit = ~std::size_t{0}) : limit_(limit) {}
    bool configure(const Bytes&, const Bytes&, const Bytes&) override { ++script.configured; return true; }
    bool decode(const std::vector<Bytes>&, xdb::Frame& frame, bool output_pixels) override {
        ++script.decoded;
        if (!script.pixels) return false;
        if (!output_pixels) return true;
        ++script.materialized;
        frame.width = frame.height = 64; frame.row_pitch = 256; frame.pixels.assign(64u * 256u, 17);
        return true;
    }
    std::size_t max_nal_size() const override { ++script.max_nal_calls; return limit_; }
    const char* backend_name() const override { return "controlled"; }
};
void no_decode(Pump& pump) {
    check(!pump.decodes_video() && script.factories == 0 && script.configured == 0 &&
          script.decoded == 0 && script.max_nal_calls == 0,
          "encoded capture never constructs/configures/queries/uses a display Decoder");
    const auto stats = pump.stats();
    check(stats.decoded == 0 && stats.decode_calls == 0 && stats.ms_decode == 0 &&
          stats.no_output == 0 && stats.dropped_oversized == 0 && stats.forced_decode_failures == 0,
          "intentional decode bypass is not a decoder failure or oversized fallback");
    xdb::Frame frame;
    check(pump.serial() == 0 && !pump.latest(frame, 0) && pump.newer(frame, 0, 0) == 0 && !frame,
          "encoded readiness does not fabricate a frame or serial");
}
void startup_and_default(const Directory& directory, bool available) {
    script.reset(); xdb::remote::Device device; std::string error;
    Pump::Options options;
    check(options.decode_video, "existing FramePump callers decode by default");
    options.decode_video = false;
    check(!Pump::start(device, options, error) && error.find("recording consumer") != std::string::npos &&
          script.start_count() == 0 && script.factories == 0,
          "capture without a consumer is rejected before negotiation or factory");
    options.record_path = (directory.path / "unused.hevc").string();
    for (unsigned option = 0; option < 3; ++option) {
        auto invalid = options;
        if (option == 0) invalid.debug_fail_decode_of_keyframe = 1;
        else if (option == 1) invalid.debug_fail_any_keyframe = true;
        else invalid.debug_suppress_pli_after_fail = true;
        check(!Pump::start(device, invalid, error) && error.find("require video decoding") != std::string::npos &&
              script.start_count() == 0 && script.factories == 0,
              "decode-failure injection cannot silently become ineffective in capture mode");
    }
    auto hardware = options; hardware.use_hardware = true;
    check(!Pump::start(device, hardware, error) && error.find("requires video decoding") != std::string::npos &&
          script.start_count() == 0 && script.factories == 0,
          "explicit hardware decoding is rejected before capture negotiation rather than silently ignored");
    if (!available) {
        check(!Pump::start(device, options, error) && !error.empty() && script.start_count() == 0 &&
              script.factories == 0, "missing checker capability is rejected before negotiation");
    }
    if (!xdb::kHaveDecoder) {
        options = {};
        check(!Pump::start(device, options, error) && script.start_count() == 0,
              "default no-decoder build retains its pre-negotiation failure");
        return;
    }
    script.reset(); script.decoder_available = true; options = {};
    auto pump = Pump::start(device, options, error);
    check(pump != nullptr && pump->decodes_video(), "default decoded loop still starts");
    if (!pump) return;
    auto sequence = parameters(10); script.enqueue(rtp(sequence++, 0, idr, true));
    check(wait([] { return script.decoded == 1; }) && !pump->wait_ready(0) && pump->serial() == 0,
          "default readiness requires actual pixels, not merely a complete IDR");
    script.pixels = true; script.enqueue(rtp(sequence, 2400, idr, true));
    check(pump->wait_ready(2000) && pump->serial() == 1 && pump->stats().decoded == 1,
          "default success publishes a real frame and marks readiness");
    int width = 0, height = 0; pump->size(width, height);
    check(width == 64 && height == 64 && script.factories == 1 && script.configured == 1,
          "default configure/factory/size behavior is preserved");
    Bytes heartbeat(28, 0); heartbeat[0] = 0x81; heartbeat[1] = 0xc8;
    put32(heartbeat, 20, 17); put32(heartbeat, 24, 1700);
    check(xdb::rt::is_rtcp_sr(heartbeat) && !xdb::rt::validate_rtcp(heartbeat),
          "legacy device heartbeat remains distinct from strict RTCP validation");
    script.enqueue(heartbeat);
    check(wait([&] { return pump->stats().dev_sent_packets == 17 && pump->stats().dev_sent_octets == 1700; }) &&
          pump->serial() == 1, "nonrecording validation retains heartbeat counters without publishing a frame");
    check(pump->finish_recording(error) && !pump->wait_ready(0), "stop wakes and invalidates readiness");
}
void decoded_software_fallback() {
    script.reset(); script.decoder_available = true; script.pixels = true;
    script.platform_limit = 1;
    xdb::remote::Device device; Pump::Options options; options.use_hardware = true;
    std::string error; auto pump = Pump::start(device, options, error);
    check(pump != nullptr, "limited platform decoder starts before its first IDR");
    if (!pump) return;
    auto sequence = parameters(10); script.enqueue(rtp(sequence, 0, idr, true));
    check(pump->wait_ready(2000) && script.factories == 2 && script.configured == 1 &&
          pump->stats().decoded == 1 && pump->stats().dropped_oversized == 0,
          "oversized IDR switches to software and configures it before publishing pixels");
    (void)pump->finish_recording(error);
}
void decoded_output_rate() {
    xdb::remote::Device invalid_device; Pump::Options invalid; std::string invalid_error;
    invalid.max_fps = 241;
    check(!Pump::start(invalid_device, invalid, invalid_error) &&
          invalid_error.find("max_fps") != std::string::npos,
          "native caller cannot exceed the documented output limit");
    invalid.max_fps = 10; invalid.decode_video = false;
    check(!Pump::start(invalid_device, invalid, invalid_error) &&
          invalid_error.find("requires decoded video") != std::string::npos,
          "native encoded-only caller cannot silently request an ineffective limit");
    if (!xdb::kHaveDecoder) return;
    script.reset(); script.decoder_available = true; script.pixels = true;
    xdb::remote::Device device; Pump::Options options; options.max_fps = 1;
    options.offer.max_bitrate = 1500000;
    std::string error; auto pump = Pump::start(device, options, error);
    check(pump != nullptr, "limited decoded pump starts");
    if (!pump) return;
    check(pump->max_bitrate() == 1500000 && pump->negotiated_max_bitrate() == 1200000,
          "requested bitrate and device-negotiated bitrate stay distinct");
    { std::lock_guard lock(script.mutex);
      check(script.starts.back().offer.max_bitrate == 1500000, "bitrate reaches stream negotiation"); }
    auto sequence = parameters(10);
    script.enqueue(rtp(sequence++, 0, idr, true));
    check(pump->wait_ready(2000) && pump->serial() == 1 && script.materialized == 1,
          "limited pump publishes its first IDR immediately");
    for (unsigned n = 1; n <= 3; ++n) script.enqueue(rtp(sequence++, n*3000, idr, true));
    check(wait([&] { return pump->stats().rate_limited == 3; }) && script.decoded == 4 &&
          script.materialized == 1 && pump->serial() == 1 && !pump->video_unusable(),
          "limited AUs retain decoder reference processing without allocating pixels or advancing publication");
    pump->set_max_fps(0);
    script.enqueue(rtp(sequence++, 12000, idr, true));
    check(wait([&] { return pump->serial() == 2; }) && script.materialized == 2,
          "raising shared demand resumes output without restarting the stream");
    pump->set_max_fps(1);
    script.enqueue(rtp(sequence++, 15000, idr, true));
    check(wait([&] { return pump->stats().rate_limited == 4; }) && pump->serial() == 2,
          "lowering shared demand updates the running limiter");
    // A sequence gap must still request an IDR and publish recovery immediately.
    sequence += 1;
    script.enqueue(rtp(sequence++, 18000, idr, true));
    check(wait([&] { return pump->serial() == 3; }) && !pump->video_unusable(),
          "rate limiting cannot suppress the first recovered keyframe");
    (void)pump->finish_recording(error);
}
std::unique_ptr<Pump> raw_pump(xdb::remote::Device& device, const Directory& directory,
                              std::string_view name, int stall = 0, std::shared_ptr<xdb::media::SessionGroup> group = {}) {
    Pump::Options options; options.decode_video = false;
    options.record_path = (directory.path / name).string();
    options.stall_restart_ms = stall; options.silence_restart_ms = 0;
    options.session_group = std::move(group);
    std::string error; auto pump = Pump::start(device, options, error);
    check(pump != nullptr && error.empty(), "raw encoded recording starts with the real syntax checker");
    return pump;
}
void healthy_static_capture(const Directory& directory) {
    script.reset(); xdb::remote::Device device;
    auto pump = raw_pump(device, directory, "static.hevc"); if (!pump) return;
    auto sequence = parameters(65534); script.enqueue(rtp(sequence++, 0, idr, true));
    check(pump->wait_ready(2000) && !pump->reviving(), "legal complete IDR establishes encoded readiness");
    int width = 0, height = 0; pump->size(width, height);
    check(width == 64 && height == 64, "size comes from the independently checked encoded parameters");
    script.enqueue(rtp(1, 0, idr, true)); // duplicate
    script.enqueue(rtp(0, 0, idr, true)); // late
    script.enqueue(rtp(sequence, 2400, idr, true, 42)); // foreign before depacketizer
    script.enqueue(rtp(sequence, 2400, idr, true, 0, 99));
    std::jthread heartbeat([](std::stop_token token) {
        while (!token.stop_requested()) { script.enqueue(sr(24000, 1001)); std::this_thread::sleep_for(200ms); }
    });
    const auto begin = std::chrono::steady_clock::now();
    const auto minimum = begin + 5300ms;
    const auto deadline = begin + 8000ms;
    // 必须跨过五秒无关键帧边界，同时等接收线程实际处理足够的 SR/RR。
    // sanitizer 或繁忙 CI 的调度可能延后处理；八秒上限仍会暴露停止收发。
    while (std::chrono::steady_clock::now() < deadline) {
        const auto stats = pump->stats();
        if (std::chrono::steady_clock::now() >= minimum &&
            stats.sr_packets >= 20 && stats.rtcp_sent >= 5) break;
        std::this_thread::sleep_for(20ms);
    }
    heartbeat.request_stop(); heartbeat.join();
    const auto stats = pump->stats();
    std::printf("static capture: SR=%llu RR=%llu gaps=%llu elapsed=%lld ms\n",
                static_cast<unsigned long long>(stats.sr_packets),
                static_cast<unsigned long long>(stats.rtcp_sent),
                static_cast<unsigned long long>(stats.gaps),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - begin).count()));
    check(script.start_count() == 1 && pump->stats().restarts == 0 && pump->wait_ready(0),
          "a valid IDR followed by SR-only silence beyond five seconds does not trigger no-key restart");
    check(stats.sr_packets >= 20 && stats.rtcp_sent >= 5 && stats.gaps == 0,
          "SR heartbeat and RR renewal continue without decoding and late/foreign packets create no gap");
    { std::lock_guard lock(script.mutex);
      bool found = false, matching_feedback = true;
      for (const auto& [bytes, port] : script.sent) if (bytes.size() == 32 && get32(bytes, 16) == 65537) {
          found = true;
          matching_feedback = matching_feedback && port == 24680 && get32(bytes, 8) == 0;
      }
      check(found && matching_feedback,
            "capture RR retains extended sequence across wrap, negotiated peer and explicit source zero"); }
    no_decode(*pump); std::string error;
    check(pump->finish_recording(error) && pump->capture_error().empty(), "raw capture finishes without an invented decoder error");
    std::ifstream file(directory.path / "static.hevc", std::ios::binary);
    const Bytes actual((std::istreambuf_iterator<char>(file)), {});
    Bytes expected;
    for (const auto* nal : {&vps, &sps, &pps, &idr}) {
        expected.insert(expected.end(), {0,0,0,1}); expected.insert(expected.end(), nal->begin(), nal->end());
    }
    check(actual == expected, "raw recording preserves original Annex-B NAL bytes without duplicate samples");
}
void loss_and_validation(const Directory& directory) {
    script.reset(); xdb::remote::Device device;
    auto pump = raw_pump(device, directory, "validation.hevc"); if (!pump) return;
    auto sequence = parameters(10); script.enqueue(rtp(sequence++, 0, idr, true));
    check(pump->wait_ready(2000), "recovery fixture begins with a legal IDR");
    ++sequence; script.enqueue(rtp(sequence++, 2400, Bytes{0x02,0x01,0x80,0x00}, true));
    check(wait([&] { return pump->stats().gaps == 1; }) && !pump->wait_ready(0),
          "actual sequence loss clears encoded health before a following non-IDR can restore it");
    const auto inject_wait = [&](Bytes nal, uint32_t ticks, const char* message) {
        const auto before = pump->stats().aus; script.enqueue(rtp(sequence++, ticks, nal, true));
        check(wait([&] { return pump->stats().aus > before; }) && !pump->wait_ready(0), message);
    };
    inject_wait(Bytes{0x28,0x01,0xc0}, 4800, "truncated IDR slice syntax cannot clear keyframe wait");
    auto cra = idr; cra[0] = 0x2a;
    inject_wait(cra, 7200, "generic CRA/IRAP marker does not count as a clean type 19/20 IDR");
    inject_wait(idr, 6000, "a backward sampling timestamp cannot clear keyframe wait");
    auto idr19 = idr; idr19[0] = 0x26;
    script.enqueue(rtp(sequence++, 9600, idr19, true));
    check(pump->wait_ready(2000) && !pump->reviving() && !pump->video_unusable(),
          "only a newer fully checked IDR restores encoded health");
    auto before = pump->stats().aus;
    script.enqueue(rtp(sequence++, 7200, Bytes{0x02,0x01,0x80,0x00}, true));
    check(wait([&] { return pump->stats().aus > before; }) && pump->wait_ready(0),
          "a forward-sequence ordinary AU with backward picture timestamp does not falsely revoke raw readiness");
    inject_wait(idr, 8400, "a newer-than-last-P but older-than-high-water IDR still cannot restore health");
    script.enqueue(rtp(sequence++, 12000, idr, true));
    check(pump->wait_ready(2000), "a forward IDR restores health after the raw picture-order contrast");
    const auto before_prefix = pump->stats().video_packets;
    script.enqueue(rtp(sequence++, 14400, pps));
    check(wait([&] { return pump->stats().video_packets > before_prefix; }) && pump->wait_ready(0),
          "repeating exact checked parameters does not revoke raw readiness");
    auto changed_pps = pps; changed_pps.back() ^= 1;
    const auto before_changed = pump->stats().video_packets;
    script.enqueue(rtp(sequence++, 14400, changed_pps));
    check(wait([&] { return pump->stats().video_packets > before_changed && !pump->wait_ready(0); }),
          "new parameters without a fresh IDR immediately revoke stale encoded readiness");
    int width = -1, height = -1; pump->size(width, height);
    check(width == 0 && height == 0, "new unverified parameters do not advertise old checked dimensions");
    before = pump->stats().aus;
    script.enqueue(rtp(sequence++, 14400, Bytes{0x02,0x01,0x80,0x00}, true));
    check(wait([&] { return pump->stats().aus > before; }) && !pump->wait_ready(0),
          "a forward ordinary transport fixture after parameter change cannot replace an IDR");
    script.enqueue(rtp(sequence++, 16800, pps));
    script.enqueue(rtp(sequence++, 16800, idr, true));
    check(pump->wait_ready(2000), "a fresh checked IDR publishes readiness after parameters are restored");
    check(pump->stats().pli_sent >= 1 && pump->stats().dropped_awaiting_keyframe >= 4,
          "failed candidates retain PLI and recovery accounting");
    no_decode(*pump); std::string error; check(pump->finish_recording(error), "validation raw producer stops and joins");
}
void stop_during_recovery(const Directory& directory) {
    script.reset();
    { std::lock_guard lock(script.mutex); script.block_recovery = true; }
    xdb::remote::Device device;
    auto pump = raw_pump(device, directory, "cancel-recovery.hevc", 20);
    if (!pump) return;
    auto sequence = parameters(100);
    script.enqueue(rtp(sequence++, 0, idr, true));
    check(pump->wait_ready(2000), "cancellation fixture reaches its first real IDR");
    ++sequence;
    script.enqueue(rtp(sequence, 2400, Bytes{0x02,0x01,0x80,0x00}, true));
    check(wait([] { return script.recovery_waiting.load(); }), "sequence loss enters a pending recovery start");
    const auto before = std::chrono::steady_clock::now();
    std::string error;
    (void)pump->finish_recording(error);
    check(std::chrono::steady_clock::now() - before < 1s,
          "stop cancels the pending start before joining instead of waiting for its reply timeout");
}

void raw_file_failure(const Directory& directory) {
#ifndef _WIN32
    struct rlimit previous{};
    check(getrlimit(RLIMIT_FSIZE, &previous) == 0, "read the writer fixture file-size limit");
    struct Restore {
        struct rlimit limit;
        void (*handler)(int);
        ~Restore() { setrlimit(RLIMIT_FSIZE, &limit); std::signal(SIGXFSZ, handler); }
    } restore{previous, std::signal(SIGXFSZ, SIG_IGN)};
    auto limited = previous; limited.rlim_cur = 0;
    const bool applied = setrlimit(RLIMIT_FSIZE, &limited) == 0;
    check(applied, "apply a real file-size limit to the isolated test process");
    if (!applied) return;
    script.reset(); xdb::remote::Device device; Pump::Options options;
    options.decode_video = false; options.record_path = (directory.path / "write-failure.hevc").string();
    options.stall_restart_ms = 0; options.silence_restart_ms = 0;
    std::string error; auto pump = Pump::start(device, options, error);
    check(pump != nullptr, "a new raw file opens before a real write failure");
    if (!pump) return;
    auto oversized_parameter = vps; oversized_parameter.resize(16384, 0);
    script.enqueue(rtp(10, 0, oversized_parameter));
    check(wait([&] { return !pump->capture_error().empty(); }), "actual raw fwrite failure is observable through capture_error");
    const auto first = pump->capture_error();
    check(!pump->finish_recording(error) && error == first && pump->capture_error() == first,
          "raw flush/close preserves the original writer failure for headless owner cleanup");
    no_decode(*pump);
#else
    (void)directory;
    std::puts("SKIP: raw writer failure fixture requires POSIX RLIMIT_FSIZE");
#endif
}
void alive_without_idr(const Directory& directory) {
    script.reset(); xdb::remote::Device device;
    auto pump = raw_pump(device, directory, "alive.hevc"); if (!pump) return;
    auto sequence = parameters(10);
    script.enqueue(rtp(sequence++, 0, Bytes{0x28,0x01,0xc0}, true));
    check(wait([&] { return pump->stats().aus == 1; }) && pump->reviving() && !pump->wait_ready(0),
          "a damaged initial IDR cannot finish recovery");
    std::this_thread::sleep_for(1250ms); pump->wake();
    check(wait([] { return script.probes >= 1; }) && pump->reviving() && !pump->wait_ready(0),
          "an actual Alive status RPC without usable IDR cannot finish encoded recovery");
    script.enqueue(rtp(sequence, 2400, idr, true));
    check(pump->wait_ready(2000) && !pump->reviving(), "legal IDR completes recovery after an Alive reply");
    no_decode(*pump); std::string error; check(pump->finish_recording(error), "Alive-only fixture stops without an invented terminal error");
}
void epoch_and_terminal(const Directory& directory, bool terminal) {
    script.reset(); xdb::remote::Device device;
    auto pump = raw_pump(device, directory, terminal ? "terminal.hevc" : "epoch.hevc", 80); if (!pump) return;
    auto sequence = parameters(10); script.enqueue(rtp(sequence++, 0, idr, true));
    check(pump->wait_ready(2000), "epoch fixture starts healthy");
    if (terminal) { std::lock_guard lock(script.mutex); script.fail_next = Session::StartStatus::AcceptedInvalidAnswer; }
    ++sequence; script.enqueue(rtp(sequence, 2400, Bytes{0x02,0x01,0x80,0x00}, true));
    check(wait([&] { return script.start_count() >= 2; }), "loss wait rebuilds the actual video session");
    if (terminal) {
        // 错误与恢复标记由 worker 分阶段发布；在原两秒预算内等待完整终态。
        check(wait([&] { return !pump->capture_error().empty() && !pump->wait_ready(0) &&
                               pump->video_unusable() && !pump->reviving(); }),
              "accepted-invalid recovery exposes a sticky terminal error instead of waiting forever");
        std::this_thread::sleep_for(1150ms);
        check(script.start_count() == 2, "accepted-invalid worker stops and never performs the ordinary one-second retry");
        std::string error; check(!pump->finish_recording(error) && error == pump->capture_error(),
                                 "capture finish preserves the terminal first error");
    } else {
        check(!pump->wait_ready(0) && pump->reviving(), "new epoch must establish its own IDR readiness");
        int width = -1, height = -1; pump->size(width, height);
        check(width == 0 && height == 0, "old epoch coded dimensions are not published as current readiness");
        script.enqueue(rtp(40, 4800, idr, true));
        check(wait([&] { return pump->stats().aus >= 3; }) && !pump->wait_ready(0),
              "IDR without new epoch parameters cannot borrow the previous parser configuration");
        sequence = parameters(41, 7200); script.enqueue(rtp(sequence, 7200, idr, true));
        check(pump->wait_ready(2000) && !pump->reviving(), "new epoch parameters and valid IDR complete recovery");
        no_decode(*pump); std::string error; check(pump->finish_recording(error), "healthy epoch restoration finishes raw recording");
    }
}
#ifdef XDB_HAVE_LIBAVFORMAT
std::unique_ptr<Recorder> recorder_at(const std::filesystem::path& path, std::size_t budget = 16u*1024u*1024u) {
    Recorder::Options options; options.path = path.string(); options.encoded_budget = budget;
    std::string error; auto recorder = Recorder::start(options, error);
    check(recorder != nullptr && error.empty(), "actual video-only Recorder starts"); return recorder;
}
std::unique_ptr<Pump> container_pump(xdb::remote::Device& device, Recorder& recorder, bool decoded = false) {
    Pump::Options options; options.recorder = &recorder; options.decode_video = decoded;
    options.stall_restart_ms = 0; options.silence_restart_ms = 0;
    std::string error; auto pump = Pump::start(device, options, error);
    check(pump != nullptr && error.empty(), "real container consumer starts the video pump"); return pump;
}
void container_capture(const Directory& directory, bool missing_source) {
    script.reset(); script.local_present = !missing_source;
    xdb::remote::Device device; const auto path = directory.path / (missing_source ? "bound.mkv" : "zero.mkv");
    auto recorder = recorder_at(path); if (!recorder) return;
    auto pump = container_pump(device, *recorder); if (!pump) return;
    // An SR cannot bind a missing media source; foreign packets cannot pollute sequence tracking.
    script.enqueue(sr(0, 1000));
    auto sequence = parameters(65534);
    script.enqueue(rtp(sequence++, 0, idr, true));
    check(pump->wait_ready(2000), "container capture requires a checked IDR before readiness");
    for (const auto [ticks, seconds] : {std::pair{0u, 1000u}, std::pair{24000u, 1001u}}) {
        auto compound = sr(ticks, seconds, 42);
        const auto own = sr(ticks, seconds);
        compound.insert(compound.end(), own.begin(), own.end());
        script.enqueue(compound);
    }
    script.enqueue(rtp(1, 0, idr, true)); script.enqueue(rtp(0, 0, idr, true));
    script.enqueue(rtp(sequence, 2400, idr, true, 43));
    script.enqueue(rtp(sequence++, 2400, idr, true)); script.enqueue(rtp(sequence, 4800, idr, true));
    check(wait([&] { return pump->stats().aus == 3; }), "complete current-source AU samples reach real Recorder without decoding");
    no_decode(*pump); std::string error;
    check(pump->finish_recording(error) && pump->capture_error().empty(), "producer stops before its real Recorder owner finishes");
    check(recorder->finish(error) && error.empty(), "real Recorder seals shared SR clock and finishes Matroska output");
    AVFormatContext* input = nullptr;
    check(avformat_open_input(&input, path.string().c_str(), nullptr, nullptr) >= 0 && input,
          "encoded-only output opens as a real Matroska container");
    if (!input) return;
    check(input->nb_streams == 1 && input->streams[0]->codecpar->codec_id == AV_CODEC_ID_HEVC,
          "container holds only the original HEVC video track");
    AVPacket* packet = av_packet_alloc(); unsigned count = 0; int status = AVERROR_EOF;
    check(packet != nullptr, "demux packet allocation succeeds");
    if (packet) while ((status = av_read_frame(input, packet)) >= 0) {
        Bytes expected;
        const std::vector<const Bytes*> nals = count == 0 ? std::vector<const Bytes*>{&vps,&sps,&pps,&idr} : std::vector<const Bytes*>{&idr};
        for (const auto* nal : nals) {
            const auto offset = expected.size(); expected.resize(offset+4); put32(expected, offset, static_cast<uint32_t>(nal->size()));
            expected.insert(expected.end(), nal->begin(), nal->end());
        }
        const Bytes actual(packet->data, packet->data + packet->size);
        check(actual == expected, "real container retains every original HEVC NAL byte without decode or reencode");
        const auto pts = av_rescale_q(packet->pts, input->streams[packet->stream_index]->time_base, AVRational{1,1000000});
        check(pts == static_cast<int64_t>(count) * 100000 && packet->pts == packet->dts,
              "SR-derived PTS and no-reorder DTS remain exact");
        ++count; av_packet_unref(packet);
    }
    check(status == AVERROR_EOF && count == 3, "late/duplicate/foreign packets do not add container samples");
    av_packet_free(&packet); avformat_close_input(&input);
}
void shared_media_stop(const Directory& directory) {
    using Audio = xdb::media::AudioPump;
    for (bool audio_first : {false, true}) for (bool stop_audio_first : {false, true}) {
        script.reset(); xdb::remote::Device device;
        auto group = std::make_shared<xdb::media::SessionGroup>();
        Recorder::Options record_options; record_options.include_video = false; record_options.include_audio = true;
        record_options.path = (directory.path / "shared-stop.mkv").string();
        std::string error;
        auto recorder = Recorder::start(record_options, error);
        check(recorder != nullptr, "audio-only capture consumer starts for stop isolation"); if (!recorder) continue;
        Audio::Options audio_options; audio_options.decode_pcm = false; audio_options.recorder = recorder.get();
        audio_options.stop_device_on_exit = true; audio_options.session_group = group;
        std::unique_ptr<Audio> audio;
        std::unique_ptr<Pump> video;
        const auto start_audio = [&] { audio = Audio::start(device, audio_options, error); };
        const auto start_video = [&] {
            video = raw_pump(device, directory, "shared-stop-" + std::to_string(audio_first) +
                             std::to_string(stop_audio_first) + ".hevc", (!audio_first && !stop_audio_first) ? 20 : 0, group);
        };
        if (audio_first) { start_audio(); start_video(); }
        else { start_video(); start_audio(); }
        check(audio && video, "audio and video pumps start in either order");
        if (!audio || !video) continue;
        if (!audio_first && !stop_audio_first) {
            auto sequence = parameters(200);
            script.enqueue(rtp(sequence++, 0, idr, true));
            check(video->wait_ready(2000), "shared video receives its first IDR before recovery");
            ++sequence; script.enqueue(rtp(sequence, 2400, Bytes{0x02,0x01,0x80,0x00}, true));
            check(wait([] { return script.start_count() >= 3; }), "shared video recreates its session after a real sequence gap");
            check(script.stops == 0 && audio->terminal_error().empty(),
                  "video recovery skips stopAll while the audio pump remains active");
        }
        if (stop_audio_first) audio.reset(); else video.reset();
        check(script.stops == 0, "stopping the first pump does not send stopAll to the remaining media stream");
        if (stop_audio_first) video.reset(); else audio.reset();
        check(script.stops == 1, "the last pump performs device cleanup once, independent of start/stop order");
        (void)recorder->finish(error);
    }
}

void container_loss_recovery(const Directory& directory, bool malformed_idr, bool decoded) {
    script.reset(); script.decoder_available = script.pixels = decoded;
    xdb::remote::Device device;
    const auto path = directory.path / (std::string(decoded ? "decoded-" : "encoded-") +
        (malformed_idr ? "damaged-idr.mkv" : "packet-gap.mkv"));
    auto recorder = recorder_at(path); if (!recorder) return;
    auto pump = container_pump(device, *recorder, decoded); if (!pump) return;
    auto sequence = parameters(10);
    script.enqueue(rtp(sequence++,0,idr,true));
    check(pump->wait_ready(2000), "recording begins with a valid IDR");
    script.enqueue(sr(0,1000)); script.enqueue(sr(24000,1001));
    check(wait([&] { return !recorder->awaiting_video_keyframe(); }), "initial IDR passes real worker validation");
    const auto pli_before = pump->stats().pli_sent;
    auto before = pump->stats().aus;
    if (malformed_idr) {
        script.enqueue(rtp(sequence++,2400,Bytes{0x28,0x01,0xc0},true));
    } else {
        ++sequence;
        script.enqueue(rtp(sequence++,2400,Bytes{0x02,0x01,0x80,0x00},true));
    }
    check(wait([&] { return pump->stats().aus > before && recorder->awaiting_video_keyframe(); }) &&
          recorder->error().empty(), "damaged video requests recovery without a terminal recording error");
    check(wait([&] { return pump->stats().pli_sent > pli_before; }),
          "worker validation and RTP loss both request a fresh IDR");
    before = pump->stats().aus;
    script.enqueue(rtp(sequence++,4800,Bytes{0x02,0x01,0x80,0x00},true));
    check(wait([&] { return pump->stats().aus > before; }) && recorder->awaiting_video_keyframe(),
          "dependent video does not restore recording or enter the container");
    script.enqueue(rtp(sequence++,7200,idr,true));
    check(wait([&] { return !recorder->awaiting_video_keyframe(); }) && pump->wait_ready(2000) && recorder->error().empty(),
          "a checked IDR restores recording in the same session");
    std::string error;
    check(pump->finish_recording(error) && recorder->finish(error) && error.empty(),
          "real RTP loss recovery finalizes a successful container");
    AVFormatContext* input = nullptr;
    check(avformat_open_input(&input,path.string().c_str(),nullptr,nullptr) >= 0,
          "recovered RTP capture is a readable container");
    if (!input) return;
    AVPacket* packet = av_packet_alloc(); std::vector<int64_t> pts;
    if (packet) while (av_read_frame(input,packet) >= 0) {
        pts.push_back(av_rescale_q(packet->pts,input->streams[packet->stream_index]->time_base,AVRational{1,1000000}));
        av_packet_unref(packet);
    }
    check(pts == std::vector<int64_t>{0,300000}, "RTP recovery keeps only intact IDRs and preserves the source time gap");
    av_packet_free(&packet); avformat_close_input(&input);
}

void container_first_errors(const Directory& directory) {
    script.reset(); xdb::remote::Device device;
    auto recorder = recorder_at(directory.path / "budget-error.mkv",1024u);
    if (!recorder) return;
    auto pump = container_pump(device, *recorder); if (!pump) return;
    auto sequence = parameters(10);
    for (unsigned i = 0; i < 64; ++i) script.enqueue(rtp(sequence++,i*2400,idr,true));
    check(wait([&] { return !recorder->error().empty(); }), "shared queue exhaustion still ends recording");
    const auto first = recorder->error(); recorder->fail("later controlled failure");
    check(!first.empty() && recorder->error() == first && pump->capture_error() == first,
          "capture error copies the original consumer failure without replacing it");
    no_decode(*pump); std::string error; (void)pump->finish_recording(error);
    check(!recorder->finish(error) && error == first, "owner finish preserves the same recording first error");
}

void audio_loss_recording(const Directory& directory) {
    script.reset(); xdb::remote::Device device;
    const auto path = directory.path / "audio-loss.mkv";
    Recorder::Options options; options.path = path.string();
    options.include_video = false; options.include_audio = true;
    std::string error; auto recorder = Recorder::start(options,error);
    check(recorder != nullptr,"audio loss recorder starts"); if (!recorder) return;
    xdb::media::AudioPump::Options audio_options;
    audio_options.decode_pcm = false; audio_options.recorder = recorder.get();
    auto pump = xdb::media::AudioPump::start(device,audio_options,error);
    check(pump != nullptr,"actual AudioPump starts without PCM decoding"); if (!pump) return;
    const Bytes silence{0x00,0x68,0x34,0x00};
    script.enqueue_audio(rtp(10,0,silence,true));
    script.enqueue_audio(sr(0,1000)); script.enqueue_audio(sr(48000,1001));
    script.enqueue_audio(rtp(12,960,silence,true));
    script.enqueue_audio(rtp(13,1440,silence,true));
    check(wait([&] { return pump->stats().packets == 3 && pump->stats().other_payload == 2; }),
          "actual audio pump observes packets and trusted reports across a sequence gap");
    check(pump->stats().seq_gaps == 1 && recorder->error().empty(),
          "audio packet loss is counted without sealing recording");
    pump.reset();
    check(recorder->finish(error) && error.empty(),"audio gap recording finishes successfully");
    AVFormatContext* input = nullptr;
    check(avformat_open_input(&input,path.string().c_str(),nullptr,nullptr) >= 0,
          "audio gap recording demuxes successfully");
    if (!input) return;
    AVPacket* packet = av_packet_alloc(); std::vector<int64_t> pts;
    if (packet) while (av_read_frame(input,packet) >= 0) {
        pts.push_back(av_rescale_q(packet->pts,input->streams[packet->stream_index]->time_base,AVRational{1,1000000}));
        av_packet_unref(packet);
    }
    check(pts == std::vector<int64_t>{0,20000,30000},"audio recording retains the missing packet's time interval");
    av_packet_free(&packet); avformat_close_input(&input);
}
#endif
} // namespace
namespace xdb {
std::unique_ptr<Decoder> create_software_decoder() {
    ++script.factories;
    return script.decoder_available ? std::make_unique<::Decoder>() : nullptr;
}
std::unique_ptr<Decoder> create_platform_decoder() {
    ++script.factories;
    return script.decoder_available ? std::make_unique<::Decoder>(script.platform_limit) : nullptr;
}
} // namespace xdb
namespace xdb::media {
std::unique_ptr<StreamSession> StreamSession::start(remote::Device&, const Request& request,
        std::string& error, bool, remote::ServiceConnection*, StartStatus* status) {
    std::unique_lock lock(script.mutex); script.starts.push_back(request);
    if (script.block_recovery && script.starts.size() > 1) {
        std::stop_callback notify(request.cancel, [] { script.ready.notify_all(); });
        script.recovery_waiting = true;
        script.ready.wait_for(lock, 5s, [&] { return request.cancel.stop_requested(); });
        error = "controlled start cancelled"; return nullptr;
    }
    if (script.fail_next) {
        if (status) *status = *script.fail_next;
        script.fail_next.reset(); error = "controlled video negotiation failure"; return nullptr;
    }
    Started started; started.sender_port = 24680; started.payload_type = 101;
    if (!request.audio) started.max_bitrate = 1200000;
    started.local_ssrc = script.local; started.has_local_ssrc = script.local_present;
    started.remote_ssrc = 123; started.has_remote_ssrc = true;
    if (request.audio) {
        auto config = xpc::make_dict();
        xpc::dict_set(config, "AudioStreamMode", xpc::make_int64(request.offer.audio_dup ? 8 : 10));
        auto connection = xpc::make_dict(); xpc::dict_set(connection, "streamConfig", std::move(config));
        started.answer = xpc::make_dict(); xpc::dict_set(started.answer, "connection", std::move(connection));
    }
    started.session_uuid.assign(16, static_cast<uint8_t>(script.starts.size()));
    error.clear(); if (status) *status = StartStatus::Started;
    return std::unique_ptr<StreamSession>(new StreamSession(nullptr, std::move(started)));
}
StreamSession::~StreamSession() = default;
uint16_t StreamSession::receiver_port() const { return 49152; }
bool StreamSession::next_packet(Bytes& packet, int timeout, std::string& error) {
    std::unique_lock lock(script.mutex);
    if (!script.ready.wait_for(lock, std::chrono::milliseconds(timeout), [] { return !script.packets.empty(); })) {
        error = "controlled timeout"; return false;
    }
    packet = std::move(script.packets.front()); script.packets.pop_front(); error.clear(); return true;
}
bool StreamSession::next_packet(Bytes& packet, uint16_t& peer, int timeout, std::string& error) {
    peer = 24680;
    std::unique_lock lock(script.mutex);
    if (!script.ready.wait_for(lock,std::chrono::milliseconds(std::min(timeout,5)),
            [] { return !script.audio_packets.empty(); })) {
        error = "controlled audio timeout"; return false;
    }
    packet = std::move(script.audio_packets.front()); script.audio_packets.pop_front();
    error.clear(); return true;
}
bool StreamSession::send_rtp(const Bytes& payload, uint16_t port, std::string& error) {
    std::lock_guard lock(script.mutex); script.sent.emplace_back(payload, port);
    error = script.send_ok ? "" : "controlled send failure"; return script.send_ok;
}
bool StreamSession::stop(remote::Device&, std::string& error, bool, std::stop_token) const { ++script.stops; error.clear(); return true; }
StreamSession::ServerState StreamSession::probe(remote::Device&, const Bytes&, std::string& error, bool, std::stop_token) {
    ++script.probes; error.clear(); return ServerState::Alive;
}
} // namespace xdb::media
int main() {
    const Directory directory; std::string error;
    const bool available = xdb::media::recording_idr_checks_available(error);
    startup_and_default(directory, available);
    decoded_software_fallback();
    decoded_output_rate();
    if (available) {
        stop_during_recovery(directory);
        healthy_static_capture(directory); loss_and_validation(directory); alive_without_idr(directory); raw_file_failure(directory);
        epoch_and_terminal(directory, false); epoch_and_terminal(directory, true);
#ifdef XDB_HAVE_LIBAVFORMAT
        container_capture(directory, false); container_capture(directory, true); shared_media_stop(directory); container_first_errors(directory);
        for (const bool decoded : {false,true}) {
            container_loss_recovery(directory,false,decoded); container_loss_recovery(directory,true,decoded);
        }
        audio_loss_recording(directory);
#else
        std::puts("SKIP: container capture requires libavformat; actual raw loop and syntax checks ran");
#endif
    } else std::puts("SKIP: actual encoded capture requires the public HEVC parameter/IDR checker; startup gates ran");
    std::printf("frame_capture: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
