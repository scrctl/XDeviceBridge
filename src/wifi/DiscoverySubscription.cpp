#include "wifi/DiscoverySubscription.h"
#ifdef __APPLE__
#include "wifi/Bonjour.h"
#elif defined(XDB_HAVE_AVAHI)
#include "wifi/Avahi.h"
#elif defined(_WIN32)
#include "wifi/WindowsDnsSd.h"
#endif
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace xdb::wifi::mdns::detail {
struct DiscoverySubscription::Impl {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<BrowseResult> snapshot;
    bool running = true, retry = false;
    std::jthread worker;

    explicit Impl(DiscoveryRun source) : worker([this, source = std::move(source)](std::stop_token stop) {
        std::stop_callback wake(stop, [this] { std::lock_guard lock(mutex); changed.notify_all(); });
        while (!stop.stop_requested()) {
            try {
                source(stop, [this](BrowseResult value) {
                    std::lock_guard lock(mutex);
                    snapshot = std::move(value);
                    changed.notify_all();
                });
            } catch (...) {
                std::lock_guard lock(mutex);
                snapshot = BrowseResult{{}, false, false, {"Native discovery subscription failed"}};
            }
            std::unique_lock lock(mutex);
            running = false;
            if (snapshot) { snapshot->available = false; snapshot->advertisements.clear(); }
            else snapshot = BrowseResult{};
            changed.notify_all();
            changed.wait(lock, [&] { return retry || stop.stop_requested(); });
            retry = false; running = true; snapshot.reset();
        }
    }) {}
    ~Impl() { worker.request_stop(); changed.notify_all(); worker.join(); }
};

DiscoverySubscription::DiscoverySubscription(DiscoveryRun source) : impl_(std::make_unique<Impl>(std::move(source))) {}
DiscoverySubscription::~DiscoverySubscription() = default;

BrowseResult DiscoverySubscription::read(const BrowseOptions &options) {
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    std::unique_lock lock(impl_->mutex);
    if (!impl_->running) {
        // Daemon recovery is retried by a request, never by an idle timer loop.
        impl_->retry = true; impl_->snapshot.reset(); impl_->changed.notify_all();
    }
    while (true) {
        const bool cancelled = options.should_cancel && options.should_cancel();
        const auto &snapshot = impl_->snapshot;
        const bool ready = snapshot && (!snapshot->available || std::any_of(snapshot->advertisements.begin(),
            snapshot->advertisements.end(), [](const auto &ad) { return !ad.endpoints.empty() && !ad.txt.empty(); }));
        if (cancelled || ready || std::chrono::steady_clock::now() >= deadline) {
            auto result = snapshot.value_or(BrowseResult{});
            result.cancelled = cancelled;
            return result;
        }
        impl_->changed.wait_until(lock, std::min(deadline,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(25)));
    }
}

namespace {
std::mutex owner_mutex;
std::weak_ptr<DiscoverySubscription> active;
}
std::shared_ptr<DiscoverySubscription> acquire_discovery_subscription() {
    std::lock_guard lock(owner_mutex);
    if (auto current = active.lock()) return current;
#ifdef __APPLE__
    auto current = std::make_shared<DiscoverySubscription>(bonjour_subscribe);
#elif defined(XDB_HAVE_AVAHI)
    auto current = std::make_shared<DiscoverySubscription>(avahi_subscribe);
#elif defined(_WIN32)
    auto current = std::make_shared<DiscoverySubscription>(windows_subscribe);
#endif
#if defined(__APPLE__) || defined(XDB_HAVE_AVAHI) || defined(_WIN32)
    active = current;
    return current;
#else
    return {};
#endif
}
std::optional<BrowseResult> subscribed_browse(const BrowseOptions &options) {
    std::shared_ptr<DiscoverySubscription> current;
    { std::lock_guard lock(owner_mutex); current = active.lock(); }
    return current ? std::optional(current->read(options)) : std::nullopt;
}
} // namespace xdb::wifi::mdns::detail
