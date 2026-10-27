#include "wifi/DiscoverySubscription.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace {
using namespace xdb::wifi::mdns;
using namespace std::chrono_literals;
int failures = 0;
void check(bool ok, const char *what) { if (!ok) { ++failures; std::printf("FAIL: %s\n", what); } }
struct Source {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<BrowseResult> updates;
    std::atomic<int> starts{0}, stopped{0};
    void publish(BrowseResult value) { std::lock_guard lock(mutex); updates.push_back(std::move(value)); changed.notify_all(); }
    bool wait_for_stop() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 2s, [&] { return stopped.load() > 0; });
    }
    void run(std::stop_token stop, const detail::DiscoveryPublish &publish) {
        ++starts;
        std::stop_callback wake(stop, [&] { std::lock_guard lock(mutex); changed.notify_all(); });
        publish({{}, false, true, {}});
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            changed.wait(lock, [&] { return !updates.empty() || stop.stop_requested(); });
            if (stop.stop_requested()) break;
            auto value = std::move(updates.front()); updates.pop_front();
            const bool available = value.available;
            lock.unlock(); publish(std::move(value)); lock.lock();
            if (!available) break;
        }
        ++stopped;
        changed.notify_all();
    }
};
BrowseResult phone(const char *address = "192.0.2.7") {
    Advertisement ad; ad.instance = "phone._remotepairing._tcp.local.";
    ad.txt["identifier"] = "phone";
    ad.endpoints.push_back({address, 49152, 4, "en4"});
    return {{ad}, false, true, {}};
}
}
int main() {
    {
        Source partial;
        detail::DiscoverySubscription subscription([&](auto stop, const auto &publish) { partial.run(stop, publish); });
        auto unresolved = phone();
        unresolved.advertisements[0].txt.clear();
        partial.publish(std::move(unresolved));
        auto reader = std::async(std::launch::async, [&] { return subscription.read({1s, {}}); });
        check(reader.wait_for(30ms) == std::future_status::timeout,
              "an address without TXT identity does not prematurely finish a cold read");
        partial.publish(phone());
        const auto resolved = reader.get();
        check(resolved.advertisements.size() == 1 && !resolved.advertisements[0].txt.empty(),
              "the cold reader wakes when both endpoint and TXT are available");
    }
    Source source;
    auto teardown = std::chrono::steady_clock::now();
    {
        detail::DiscoverySubscription subscription([&](auto stop, const auto &publish) { source.run(stop, publish); });
        source.publish(phone());
        BrowseOptions options{1s, {}};
        check(subscription.read(options).advertisements.size() == 1, "cold reader wakes on a resolved update");
        std::atomic<bool> failed{false};
        std::vector<std::jthread> readers;
        const auto warm = std::chrono::steady_clock::now();
        for (int i = 0; i < 8; ++i) readers.emplace_back([&] {
            for (int n = 0; n < 100; ++n) if (subscription.read(options).advertisements.size() != 1) failed = true;
        });
        readers.clear();
        check(!failed && source.starts == 1 && std::chrono::steady_clock::now() - warm < 500ms,
              "concurrent warm readers share one source and do not wait a scan window");
        options.should_cancel = [] { return true; };
        check(subscription.read(options).cancelled, "cancellation is checked even for a warm snapshot");
        options.should_cancel = {};
        source.publish({{}, false, true, {}});
        // Read after the queued withdrawal has reached the source.
        source.publish({{}, false, false, {"daemon disconnected"}});
        check(source.wait_for_stop(), "failed source has returned before inspecting recovery");
        auto unavailable = subscription.read({10ms, {}});
        check(unavailable.advertisements.empty(), "source failure clears previously usable addresses");
        source.publish(phone("192.0.2.8"));
        BrowseResult recovered;
        const auto recovery_deadline = std::chrono::steady_clock::now() + 1s;
        do {
            recovered = subscription.read(options);
            if (recovered.available && !recovered.advertisements.empty()) break;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < recovery_deadline);
        check(recovered.available && recovered.advertisements.size() == 1 &&
              recovered.advertisements[0].endpoints[0].address == "192.0.2.8" && source.starts == 2,
              "a later request restarts a failed daemon subscription without stale addresses");
        check(source.starts == 2, "warm reads never restart the native browser");
        teardown = std::chrono::steady_clock::now();
    }
    check(source.stopped == 2 && std::chrono::steady_clock::now() - teardown < 500ms,
          "destruction interrupts the idle source and joins its callbacks promptly");
    std::printf("%s: synchronized discovery snapshots, recovery and teardown\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
