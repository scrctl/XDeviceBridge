#include "Decoder.h"
#include "util/Bytes.h"

#include <CoreMedia/CMBlockBuffer.h>
#include <CoreMedia/CMFormatDescription.h>
#include <CoreMedia/CMSampleBuffer.h>
#include <CoreVideo/CVPixelBuffer.h>
#include <VideoToolbox/VideoToolbox.h>

#include <cstdio>
#include <cstring>
#include <limits>

namespace xdb {
namespace {

/// 一次提交对应一个输出槽，通过 source_frame_ref_con 传给输出回调。
/// 回调可能晚于 DecodeFrame 返回，槽必须覆盖回调的生命周期。当前成功提交路径
/// 调用 WaitForAsynchronousFrames 后才读取和释放槽；同一实例由调用方串行使用。
/// 回调 retain 输出像素缓冲，decode 在复制到 Frame 后负责 release。
struct OutputSlot {
    CVPixelBufferRef pb = nullptr;
    OSStatus status = noErr;
};

void output_callback(void *ref_con, void *source_frame_ref_con, OSStatus status, VTDecodeInfoFlags,
                     CVImageBufferRef image_buffer, CMTime, CMTime) {
    (void)ref_con;
    auto *slot = static_cast<OutputSlot *>(source_frame_ref_con);
    if (slot != nullptr) slot->status = status;
    if (slot == nullptr || status != noErr || image_buffer == nullptr) {
        return;
    }
    slot->pb = CVPixelBufferRetain(image_buffer);
}

// Four-byte prefixes support device NALs larger than 65535 bytes.
constexpr int kNalLengthSize = 4;
constexpr size_t kMaxNalSize = std::numeric_limits<uint32_t>::max();

class VideoToolboxDecoder final : public Decoder {
public:
    ~VideoToolboxDecoder() override { teardown(); }

    bool configure(const Nal &vps, const Nal &sps, const Nal &pps) override {
        teardown();

        const uint8_t *sets[3] = {vps.data(), sps.data(), pps.data()};
        const size_t sizes[3] = {vps.size(), sps.size(), pps.size()};

        OSStatus st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
            nullptr, 3, sets, sizes, kNalLengthSize, nullptr, &format_desc_);
        if (st != noErr) {
            std::fprintf(stderr, "Failed to parse HEVC parameter sets: %d\n", static_cast<int>(st));
            return false;
        }

        // Match the software scaler's BT.601 default only when the source did not
        // declare a matrix. Preserve any matrix already parsed from the SPS.
        if (CMFormatDescriptionGetExtension(format_desc_, kCMFormatDescriptionExtension_YCbCrMatrix) == nullptr) {
            const void *key = kCMFormatDescriptionExtension_YCbCrMatrix;
            const void *value = kCVImageBufferYCbCrMatrix_ITU_R_601_4;
            CFDictionaryRef extensions = CFDictionaryCreate(nullptr, &key, &value, 1,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CMFormatDescriptionRef corrected = nullptr;
            if (extensions == nullptr) { teardown(); return false; }
            st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                nullptr, 3, sets, sizes, kNalLengthSize, extensions, &corrected);
            CFRelease(extensions);
            if (st != noErr) { teardown(); return false; }
            CFRelease(format_desc_);
            format_desc_ = corrected;
        }

        // 属性值使用 CFNumber 对象。字典通过 CF 类型回调保留该对象，创建字典后
        // 可释放本地引用；不能把整数地址当作 CFNumberRef 传入。
        const uint32_t bgra = kCVPixelFormatType_32BGRA;
        CFNumberRef bgra_num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &bgra);
        if (bgra_num == nullptr) {
            teardown();
            return false;
        }
        const void *keys[] = {kCVPixelBufferPixelFormatTypeKey};
        const void *vals[] = {bgra_num};
        CFDictionaryRef dest = CFDictionaryCreate(nullptr, keys, vals, 1,
                                                 &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
        CFRelease(bgra_num);
        if (dest == nullptr) {
            teardown();
            return false;
        }

        const VTDecompressionOutputCallbackRecord cb{output_callback, nullptr};
        const void *spec_key = kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder;
        const void *spec_value = kCFBooleanTrue;
        CFDictionaryRef spec = CFDictionaryCreate(nullptr, &spec_key, &spec_value, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (spec == nullptr) { CFRelease(dest); teardown(); return false; }
        st = VTDecompressionSessionCreate(nullptr, format_desc_, spec, dest, &cb, &session_);
        CFRelease(spec);
        CFRelease(dest);

        if (st != noErr || session_ == nullptr) {
            std::fprintf(stderr, "Failed to create VideoToolbox session: %d\n", static_cast<int>(st));
            teardown();
            return false;
        }
        update_hardware_status();
        return true;
    }

    bool decode(const std::vector<Nal> &au, Frame &out, bool output_pixels) override {
        if (session_ == nullptr || failed_) {
            return false;
        }

        // 当前适配器仅将 VCL NAL 写入样本，参数集来自 configure 的 format
        // description。已有样本中重复内联参数集会导致 bad data，不外推其他配置。
        std::vector<const Nal *> slices;
        slices.reserve(au.size());
        size_t total = 0;
        for (const auto &n : au) {
            if (n.size() < 2) {
                continue;
            }
            const uint8_t type = static_cast<uint8_t>((n[0] >> 1) & 0x3F);
            if (type >= 32) {
                continue;  // 非 VCL：参数集 / SEI / 保留
            }
            if (n.size() > kMaxNalSize || total > std::numeric_limits<size_t>::max() - kNalLengthSize ||
                n.size() > std::numeric_limits<size_t>::max() - kNalLengthSize - total) {
                return false;
            }
            slices.push_back(&n);
            total += kNalLengthSize + n.size();
        }
        if (total == 0) {
            return false;
        }

        std::vector<uint8_t> buf;
        buf.reserve(total);
        // 每个 VCL NAL 前写四字节大端长度，长度仅计 NAL 本身，不含前缀。
        // NAL 保留 emulation prevention 字节，不能先转成去转义的 RBSP；长度和
        // 样本内容均按 AnnexB.h 中原始 Nal 的语义提交。
        for (const Nal *n : slices) {
            util::append_be32(buf, static_cast<uint32_t>(n->size()));
            buf.insert(buf.end(), n->begin(), n->end());
        }

        // CoreMedia 分配独立存储，再将 buf 复制进去；样本不引用局部 vector 的内存。
        CMBlockBufferRef bb = nullptr;
        OSStatus st = CMBlockBufferCreateWithMemoryBlock(
            nullptr, nullptr, buf.size(), kCFAllocatorDefault, nullptr, 0, buf.size(),
            kCMBlockBufferAssureMemoryNowFlag, &bb);
        if (st != kCMBlockBufferNoErr) {
            return false;
        }
        st = CMBlockBufferReplaceDataBytes(buf.data(), bb, 0, buf.size());
        if (st != kCMBlockBufferNoErr) {
            CFRelease(bb);
            return false;
        }

        CMSampleTimingInfo timing{};
        timing.duration = kCMTimeInvalid;
        timing.decodeTimeStamp = kCMTimeInvalid;
        timing.presentationTimeStamp = CMTimeMake(pts_num_++, 1000);

        const size_t sample_size = buf.size();
        CMSampleBufferRef sb = nullptr;
        st = CMSampleBufferCreateReady(nullptr, bb, format_desc_, 1, 1, &timing, 1, &sample_size,
                                       &sb);
        // 成功创建的 sample buffer 保留 block buffer，本地引用可在此释放。
        CFRelease(bb);
        if (st != noErr || sb == nullptr) {
            return false;
        }

        OutputSlot slot;
        st = VTDecompressionSessionDecodeFrame(session_, sb, 0, &slot, nullptr);
        CFRelease(sb);
        if (st != noErr) {
            failed_ = true;
            if (slot.pb != nullptr) {
                CVPixelBufferRelease(slot.pb);
            }
            return false;
        }
        // 等待已提交帧的异步回调完成，之后才读取 slot 并结束其栈上生命周期。
        // 当前适配器每次提交后等待，不在多个 decode 调用之间建立异步流水。
        const OSStatus waited = VTDecompressionSessionWaitForAsynchronousFrames(session_);
        if (waited != noErr || slot.status != noErr) {
            failed_ = true;
            if (slot.pb) CVPixelBufferRelease(slot.pb);
            return false;
        }
        if (slot.pb == nullptr) return false;
        if (!verified_output_) {
            update_hardware_status();
            verified_output_ = true;
        }
        const bool copied = !output_pixels || copy_out(slot.pb, out);
        CVPixelBufferRelease(slot.pb);
        slot.pb = nullptr;
        if (!copied) failed_ = true;
        return copied;
    }

    [[nodiscard]] const char *backend_name() const override { return "VideoToolbox"; }

    [[nodiscard]] std::optional<bool> hardware_accelerated() const override { return hardware_; }
    [[nodiscard]] bool failed() const override { return failed_; }

    /// Four-byte sample length, excluding the prefix itself.
    [[nodiscard]] size_t max_nal_size() const override { return kMaxNalSize; }

private:
    void update_hardware_status() {
        CFTypeRef value = nullptr;
        if (VTSessionCopyProperty(session_, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
                                  nullptr, &value) == noErr && value && CFGetTypeID(value) == CFBooleanGetTypeID())
            hardware_ = CFBooleanGetValue(static_cast<CFBooleanRef>(value));
        if (value) CFRelease(value);
    }

    void teardown() {
        hardware_.reset();
        failed_ = false;
        verified_output_ = false;
        pts_num_ = 0;
        if (session_ != nullptr) {
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (format_desc_ != nullptr) {
            CFRelease(format_desc_);
            format_desc_ = nullptr;
        }
    }

    /// 锁定像素缓冲后按源行跨度读取，复制为紧凑 BGRA；Frame 拥有复制后的像素。
    /// 锁定失败或源数据无效时不更新 out，调用方不能将旧内容视为本次新输出。
    static bool copy_out(CVPixelBufferRef pb, Frame &out) {
        if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
            return false;
        }
        const auto *src = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(pb));
        const size_t width = CVPixelBufferGetWidth(pb);
        const size_t rows = CVPixelBufferGetHeight(pb);
        const size_t src_pitch = CVPixelBufferGetBytesPerRow(pb);
        const size_t row_bytes = width * 4;

        const bool valid = src && rows > 0 && width > 0 && width <= UINT32_MAX / 4 &&
            rows <= UINT32_MAX && src_pitch >= row_bytes && row_bytes <= std::numeric_limits<size_t>::max() / rows;
        if (valid) {
            out.width = static_cast<uint32_t>(width);
            out.height = static_cast<uint32_t>(rows);
            out.row_pitch = static_cast<uint32_t>(row_bytes);
            out.bytes_per_pixel = 4;
            out.pixels.resize(row_bytes * rows);
            for (size_t r = 0; r < rows; ++r) {
                std::memcpy(out.pixels.data() + r * row_bytes, src + r * src_pitch, row_bytes);
            }
        }
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        return valid;
    }

    CMFormatDescriptionRef format_desc_ = nullptr;
    VTDecompressionSessionRef session_ = nullptr;
    int64_t pts_num_ = 0;
    std::optional<bool> hardware_;
    bool failed_ = false;
    bool verified_output_ = false;
};

}  // namespace

std::unique_ptr<Decoder> create_platform_decoder() {
    return std::make_unique<VideoToolboxDecoder>();
}

}  // namespace xdb
