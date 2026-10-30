// SPDX-License-Identifier: LGPL-3.0-only
#include "decode/AndroidMediaCodec.h"
#include <jni.h>
extern "C" {
#include <libavcodec/jni.h>
}

namespace xdb::detail {
namespace {
class Environment {
public:
    Environment() : vm_(static_cast<JavaVM *>(av_jni_get_java_vm(nullptr))) {
        if (!vm_) return;
        const int status = vm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) attached_ = vm_->AttachCurrentThread(&env, nullptr) == JNI_OK;
        else if (status != JNI_OK) env = nullptr;
        if (env && env->PushLocalFrame(32) < 0) { env->ExceptionClear(); env = nullptr; }
    }
    ~Environment() {
        if (env) { if (env->ExceptionCheck()) env->ExceptionClear(); env->PopLocalFrame(nullptr); }
        if (attached_) vm_->DetachCurrentThread();
    }
    JNIEnv *env = nullptr;
private:
    JavaVM *vm_ = nullptr;
    bool attached_ = false;
};
}

bool android_mediacodec_supports_size(int width, int height) {
    Environment context;
    auto *env = context.env;
    if (!env) return false;
    auto list = env->FindClass("android/media/MediaCodecList");
    auto info = env->FindClass("android/media/MediaCodecInfo");
    auto caps = env->FindClass("android/media/MediaCodecInfo$CodecCapabilities");
    auto video = env->FindClass("android/media/MediaCodecInfo$VideoCapabilities");
    if (env->ExceptionCheck() || !list || !info || !caps || !video) return false;
    auto count = env->GetStaticMethodID(list, "getCodecCount", "()I");
    auto at = env->GetStaticMethodID(list, "getCodecInfoAt", "(I)Landroid/media/MediaCodecInfo;");
    auto encoder = env->GetMethodID(info, "isEncoder", "()Z");
    auto capabilities = env->GetMethodID(info, "getCapabilitiesForType", "(Ljava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;");
    auto get_video = env->GetMethodID(caps, "getVideoCapabilities", "()Landroid/media/MediaCodecInfo$VideoCapabilities;");
    auto supports = env->GetMethodID(video, "isSizeSupported", "(II)Z");
    if (env->ExceptionCheck() || !count || !at || !encoder || !capabilities || !get_video || !supports) return false;
    // isSoftwareOnly was added in API 29. Older systems can still use software
    // decoding; avoid treating a component name as proof of hardware support.
    auto software = env->GetMethodID(info, "isSoftwareOnly", "()Z");
    if (env->ExceptionCheck() || !software) return false;
    auto mime = env->NewStringUTF("video/hevc");
    const int size = env->CallStaticIntMethod(list, count);
    if (env->ExceptionCheck() || !mime || size < 0 || size > 1024) return false;
    bool found = false;
    for (int i = 0; i < size; ++i) {
        if (env->PushLocalFrame(8) < 0) return false;
        auto codec = env->CallStaticObjectMethod(list, at, i);
        if (env->ExceptionCheck() || !codec) { env->PopLocalFrame(nullptr); return false; }
        const bool skip = env->CallBooleanMethod(codec, encoder) || env->CallBooleanMethod(codec, software);
        if (env->ExceptionCheck()) { env->PopLocalFrame(nullptr); return false; }
        if (skip) { env->PopLocalFrame(nullptr); continue; }
        auto capability = env->CallObjectMethod(codec, capabilities, mime);
        if (env->ExceptionCheck()) {
            // A codec for another MIME type rejects getCapabilitiesForType.
            env->ExceptionClear(); env->PopLocalFrame(nullptr); continue;
        }
        auto range = capability ? env->CallObjectMethod(capability, get_video) : nullptr;
        const bool accepted = range && env->CallBooleanMethod(range, supports, width, height);
        const bool error = env->ExceptionCheck();
        env->PopLocalFrame(nullptr);
        // FFmpeg chooses a component internally; only proceed if every hardware
        // HEVC candidate supports this size, rather than selecting an unsafe one.
        if (error || !accepted) return false;
        found = true;
    }
    return found;
}
}
