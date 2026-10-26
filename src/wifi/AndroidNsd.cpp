// SPDX-License-Identifier: LGPL-3.0-only
#include "wifi/AndroidNsd.h"
#include <jni.h>
#include <mutex>
#include <thread>
#include <utility>

namespace xdb::wifi::mdns::detail {
namespace {
class Environment {
  public:
    explicit Environment(JavaVM *vm) : vm_(vm) {
        const auto result = vm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        if (result == JNI_EDETACHED) attached_ = vm_->AttachCurrentThread(&env, nullptr) == JNI_OK;
        else if (result != JNI_OK) env = nullptr;
    }
    ~Environment() { if (attached_) vm_->DetachCurrentThread(); }
    JNIEnv *env = nullptr;
  private:
    JavaVM *vm_;
    bool attached_ = false;
};
struct Provider {
    JavaVM *vm = nullptr;
    jobject object = nullptr;
    jmethodID snapshot = nullptr;
    ~Provider() { Environment e(vm); if (e.env) e.env->DeleteGlobalRef(object); }
    BrowseResult read() {
        BrowseResult result;
        Environment e(vm);
        if (!e.env) { result.warnings = {"Cannot attach Android NSD thread"}; return result; }
        auto bytes = static_cast<jbyteArray>(e.env->CallObjectMethod(object, snapshot));
        if (e.env->ExceptionCheck()) {
            e.env->ExceptionClear();
            result.warnings = {"Android NSD snapshot callback failed"}; return result;
        }
        if (bytes) {
            const auto length = e.env->GetArrayLength(bytes);
            if (length <= 1024 * 1024) {
                std::string text(static_cast<size_t>(length), '\0');
                e.env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte *>(text.data()));
                if (!e.env->ExceptionCheck()) result = android_nsd_snapshot(text);
                else e.env->ExceptionClear();
            }
            e.env->DeleteLocalRef(bytes);
        }
        return result;
    }
};
std::mutex mutex;
std::shared_ptr<Provider> provider;
} // namespace

BrowseResult android_browse(const BrowseOptions &options) {
    std::shared_ptr<Provider> current;
    { std::lock_guard lock(mutex); current = provider; }
    if (!current) return {};
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    // Only a cold/empty cache waits. A warm cache is immediate, and there is no
    // native polling thread when the application is not requesting discovery.
    while (true) {
        auto result = current->read();
        result.cancelled = options.should_cancel && options.should_cancel();
        if (!result.available || result.cancelled || !result.advertisements.empty() ||
            std::chrono::steady_clock::now() >= deadline) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_xdevicebridge_NsdDiscovery_install(JNIEnv *env, jclass, jobject instance) {
    std::shared_ptr<Provider> replacement;
    if (instance) {
        replacement = std::make_shared<Provider>();
        env->GetJavaVM(&replacement->vm);
        auto type = env->GetObjectClass(instance);
        replacement->snapshot = env->GetMethodID(type, "snapshot", "()[B");
        env->DeleteLocalRef(type);
        if (env->ExceptionCheck()) return;
        replacement->object = env->NewGlobalRef(instance);
        if (!replacement->object) return;
    }
    std::shared_ptr<Provider> old;
    { std::lock_guard lock(mutex); old = std::exchange(provider, std::move(replacement)); }
}
} // namespace xdb::wifi::mdns::detail
