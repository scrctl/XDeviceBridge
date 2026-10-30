#include "decode/Decoder.h"

// 仅被测 Decoder 的这两个 libav 调用重命名；测试边界仍可调用真实库，兼容静态链接。
#undef avcodec_send_packet
#undef avcodec_receive_frame
extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstdio>
#include <cstring>
#include <deque>
#include <string_view>

// 执行真实软件 Decoder、packet 生命周期及 swscale，只在公开 libav 收发边界
// 确定性注入积压；正常路径仍交回 libav，并用现有 HEVC 样例实际解码。
namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { ++failures; std::fprintf(stderr,"FAIL: %s\n",message); }
}
enum class Mode { Real, Backpressure, Multiple, NoOutput, Broken };
struct Script {
    Mode mode = Mode::Real;
    unsigned sends = 0, receives = 0;
    std::deque<uint8_t> frames;
    std::vector<uint8_t> packet;
} script;

xdb::Nal hex(std::string_view text) {
    xdb::Nal bytes;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned byte = 0; std::sscanf(text.data()+i,"%2x",&byte);
        bytes.push_back(static_cast<uint8_t>(byte));
    }
    return bytes;
}
const auto vps = hex("40010c01ffff01600000030090000003000003001eba0240");
const auto sps = hex("42010101600000030090000003000003001ea020810596e92930bc05a020000003002000000303c1");
const auto pps = hex("4401c073c089");
const auto idr = hex("2801ac21800e7ffeebf349ac");
bool picture(const xdb::Frame& frame, uint8_t gray) {
    return frame.width == 2 && frame.height == 2 && frame.pixels.size() == 16 &&
        frame.pixels[0] == gray && frame.pixels[1] == gray && frame.pixels[2] == gray && frame.pixels[3] == 255;
}
}

extern "C" int xdb_test_send_packet(AVCodecContext* context, const AVPacket* packet) {
    if (script.mode == Mode::Real) {
        return avcodec_send_packet(context,packet);
    }
    ++script.sends;
    check(packet && packet->data && packet->size > 0,"a live encoded packet reaches send");
    if (!packet || !packet->data || packet->size <= 0) return AVERROR(EINVAL);
    const std::vector<uint8_t> bytes(packet->data,packet->data+packet->size);
    if (script.mode == Mode::Backpressure && script.sends == 1) {
        script.packet = bytes; script.frames = {10,20}; return AVERROR(EAGAIN);
    }
    check(script.frames.empty(),"all pending output is drained before sending more input");
    if (script.mode == Mode::Backpressure && script.sends == 2) {
        check(bytes == script.packet,"the rejected packet is retained and retried unchanged");
        script.frames = {30,40};
    } else if (script.mode == Mode::Multiple) {
        script.frames = {static_cast<uint8_t>(script.sends*10),static_cast<uint8_t>(script.sends*10+5)};
    } else if (script.mode == Mode::Broken) return AVERROR(EAGAIN);
    return 0;
}

extern "C" int xdb_test_receive_frame(AVCodecContext* context, AVFrame* frame) {
    if (script.mode == Mode::Real) {
        return avcodec_receive_frame(context,frame);
    }
    ++script.receives;
    if (script.frames.empty()) return AVERROR(EAGAIN);
    av_frame_unref(frame);
    frame->format = AV_PIX_FMT_GRAY8; frame->width = frame->height = 2;
    const int status = av_frame_get_buffer(frame,32);
    if (status < 0) return status;
    const auto gray = script.frames.front(); script.frames.pop_front();
    for (int row = 0; row < 2; ++row) std::memset(frame->data[0]+row*frame->linesize[0],gray,2);
    return 0;
}

int main() {
    for (const auto mode : {Mode::Backpressure,Mode::Multiple,Mode::NoOutput,Mode::Broken}) {
        script = {}; script.mode = mode;
        auto decoder = xdb::create_software_decoder();
        check(decoder && decoder->configure(vps,sps,pps),"actual software decoder configures");
        if (!decoder) continue;
        xdb::Frame frame;
        const bool produced = decoder->decode({idr},frame);
        if (mode == Mode::Backpressure) {
            check(produced && script.sends == 2 && script.receives == 6 && picture(frame,40),
                  "send EAGAIN drains backlog, retries input and publishes the latest new frame");
        } else if (mode == Mode::Multiple) {
            check(produced && script.frames.empty() && picture(frame,15),"multiple outputs are fully drained");
            check(decoder->decode({idr},frame) && picture(frame,25),"the next AU has no output backlog or frame lag");
        } else if (mode == Mode::NoOutput) {
            check(!produced && script.sends == 1 && script.receives == 1,"temporary lack of output is not a spin loop");
        } else {
            check(!produced && script.sends == 2 && script.receives == 1,"contradictory EAGAIN is bounded to one retry");
        }
    }
    script = {}; script.mode = Mode::Multiple;
    auto limited = xdb::create_software_decoder();
    check(limited && limited->configure(vps,sps,pps),"limited decoder configures");
    if (limited) {
        xdb::Frame untouched; untouched.width = 7; untouched.pixels = {1,2,3};
        check(limited->decode({idr},untouched,false) && script.frames.empty() &&
              untouched.width == 7 && untouched.pixels == std::vector<uint8_t>({1,2,3}),
              "rate-limited decode drains all pictures without materializing or replacing pixels");
        check(limited->decode({idr},untouched) && picture(untouched,25),
              "output resumes on the next AU with no stale decoder backlog");
    }
    script = {};
    auto decoder = xdb::create_software_decoder();
    check(decoder && decoder->configure(vps,sps,pps),"real HEVC decoder configures");
    unsigned frames = 0; xdb::Frame frame;
    if (decoder) for (unsigned i = 0; i < 64; ++i) {
        if (decoder->decode({idr},frame)) {
            ++frames;
            check(frame.width == 64 && frame.height == 64 && frame.pixels.size() == 64*64*4,
                  "actual HEVC packets produce complete BGRA frames");
        }
    }
    check(frames > 0,"the unchanged real libav path still decodes pictures");
    std::printf("ffmpeg_decoder: %d failures\n",failures);
    return failures ? 1 : 0;
}
