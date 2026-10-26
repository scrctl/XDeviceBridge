#include "wifi/Bonjour.h"

#include "wifi/DnsSd.h"

#include <dns_sd.h>
#include <net/if.h>
#include <poll.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <set>
#include <tuple>

namespace xdb::wifi::mdns::detail {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxQueries = 512;
constexpr size_t kMaxReplies = 2048;

std::string dns_error(std::string_view operation, DNSServiceErrorType code) {
    return "Bonjour " + std::string(operation) + ": " + std::to_string(code);
}

class Connection {
  public:
    ~Connection() { if (ref) DNSServiceRefDeallocate(ref); }
    Connection() = default;
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    bool open(std::string &error) {
        const auto code = DNSServiceCreateConnection(&ref);
        if (code == kDNSServiceErr_NoError) return true;
        ref = nullptr;
        error = dns_error("connect", code);
        return false;
    }

    bool poll(int timeout_ms, std::string &error) {
        const int fd = DNSServiceRefSockFD(ref);
        if (fd < 0) { error = "Bonjour connection has no event socket"; return false; }
        pollfd ready{fd, POLLIN, 0};
        const int count = ::poll(&ready, 1, timeout_ms);
        if (count < 0) {
            if (errno == EINTR) return true;
            error = "Bonjour receive wait: " + std::string(std::strerror(errno));
            return false;
        }
        if (count == 0) return true;
        if (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            error = "Bonjour connection closed";
            return false;
        }
        if (ready.revents & POLLIN) {
            const auto code = DNSServiceProcessResult(ref);
            if (code != kDNSServiceErr_NoError) {
                error = dns_error("receive", code);
                return false;
            }
        }
        return true;
    }

    // Deallocating the shared parent cancels its queries/records together.
    DNSServiceRef ref = nullptr;
};

struct Browser {
    Connection connection;
    RecordCache cache;
    std::set<uint32_t> interfaces;
    std::set<std::tuple<uint32_t, uint16_t, std::string>> queries;
    std::string error;
    size_t replies = 0;

    static void DNSSD_API reply(DNSServiceRef, DNSServiceFlags flags, uint32_t interface_index,
                                DNSServiceErrorType code, const char *name, uint16_t type,
                                uint16_t record_class, uint16_t length, const void *data,
                                uint32_t, void *context) noexcept {
        auto &browser = *static_cast<Browser *>(context);
        try {
            if (++browser.replies > kMaxReplies) {
                browser.error = "Bonjour reply limit reached";
                return;
            }
            if (code == kDNSServiceErr_NoSuchRecord || code == kDNSServiceErr_NoSuchName) return;
            if (code != kDNSServiceErr_NoError) {
                browser.error = dns_error("query", code);
                return;
            }
            if (!name || record_class != kDNSServiceClass_IN || !interface_index ||
                length > 16384 || (length && !data)) return;
            std::array<char, IF_NAMESIZE> interface_name{};
            if (!if_indextoname(interface_index, interface_name.data())) return;
            if (!browser.interfaces.contains(interface_index) && browser.interfaces.size() >= 64) {
                browser.error = "Bonjour interface limit reached";
                return;
            }
            // An active DNS-SD query owns expiry and reports removals. Its TTL
            // may be the original cached value, so keep adds for this <=60s scan.
            ResourceRecord record{name, type, {}, (flags & kDNSServiceFlagsAdd) ? 120U : 0U};
            if (length) {
                const auto *bytes = static_cast<const uint8_t *>(data);
                record.data.assign(bytes, bytes + length);
            }
            const auto packet = record_packet(record);
            if (!packet.empty() && browser.cache.add_packet(interface_index, interface_name.data(), packet))
                browser.interfaces.insert(interface_index);
        } catch (...) {
            // C callbacks never unwind into DNSServiceProcessResult.
            browser.replies = kMaxReplies + 1;
        }
    }

    bool query(uint32_t index, uint16_t type, const std::string &name) {
        const auto key = std::tuple(index, type, name);
        if (queries.contains(key)) return true;
        if (queries.size() >= kMaxQueries) { error = "Bonjour query limit reached"; return false; }
        DNSServiceRef child = connection.ref;
        const auto code = DNSServiceQueryRecord(&child,
            kDNSServiceFlagsShareConnection | kDNSServiceFlagsForceMulticast,
            index, name.c_str(), type, kDNSServiceClass_IN, &reply, this);
        if (code != kDNSServiceErr_NoError) {
            error = dns_error("start query", code);
            return false;
        }
        queries.insert(key);
        return true;
    }
};
} // namespace

BrowseResult bonjour_browse(const BrowseOptions &options) {
    BrowseResult result;
    Browser browser;
    const auto deadline = Clock::now() + options.timeout;
    const auto cancelled = [&] {
        result.cancelled = options.should_cancel && options.should_cancel();
        return result.cancelled;
    };
    if (cancelled() || options.timeout.count() <= 0) return result;
    if (!browser.connection.open(browser.error) ||
        !browser.query(0, kDNSServiceType_PTR, "_remotepairing._tcp.local.")) {
        result.warnings.push_back(browser.error);
        return result;
    }
    result.available = true;
    while (!cancelled() && Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (!browser.connection.poll(static_cast<int>(std::clamp<int64_t>(remaining, 0, 50)), browser.error)) {
            result.available = false;
            break;
        }
        if (!browser.error.empty() || browser.replies > kMaxReplies) break;
        for (const auto index : browser.interfaces) {
            for (const auto &[type, name] : browser.cache.queries(index)) {
                if (cancelled() || Clock::now() >= deadline || !browser.query(index, type, name)) break;
            }
            if (!browser.error.empty()) break;
        }
    }
    result.advertisements = browser.cache.snapshot();
    result.warnings = browser.cache.warnings();
    if (!browser.error.empty()) result.warnings.push_back(browser.error);
    else if (browser.replies > kMaxReplies) result.warnings.push_back("Bonjour callback failed");
    return result;
}

struct BonjourAdvertiser::Impl {
    Connection connection;
    std::string error;

    static void DNSSD_API registered(DNSServiceRef, DNSRecordRef, DNSServiceFlags,
                                     DNSServiceErrorType code, void *context) noexcept {
        if (code == kDNSServiceErr_NoError) return;
        try { static_cast<Impl *>(context)->error = dns_error("register pairing record", code); }
        catch (...) { static_cast<Impl *>(context)->failed = true; }
    }
    bool failed = false;
};

BonjourAdvertiser::BonjourAdvertiser(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BonjourAdvertiser::~BonjourAdvertiser() = default;

std::unique_ptr<BonjourAdvertiser> BonjourAdvertiser::start(const HostAdvertisement &host, std::string &error) {
    error.clear();
    auto impl = std::make_unique<Impl>();
    if (!impl->connection.open(error)) return {};
    const auto interfaces = local_host_records(host, error);
    if (interfaces.empty()) return {};
    for (const auto &[index, host_records] : interfaces) {
        const auto records = host_resource_records(host_records, error);
        if (records.empty()) return {};
        for (const auto &record : records) {
            DNSRecordRef registered = nullptr;
            const auto flags = record.type == kDNSServiceType_PTR ? kDNSServiceFlagsShared : kDNSServiceFlagsUnique;
            const auto code = DNSServiceRegisterRecord(impl->connection.ref, &registered, flags,
                index, record.name.c_str(), record.type, kDNSServiceClass_IN,
                static_cast<uint16_t>(record.data.size()), record.data.data(), record.ttl,
                &Impl::registered, impl.get());
            if (code != kDNSServiceErr_NoError) {
                error = dns_error("start pairing advertisement", code);
                return {};
            }
        }
    }
    return std::unique_ptr<BonjourAdvertiser>(new BonjourAdvertiser(std::move(impl)));
}

bool BonjourAdvertiser::poll(int timeout_ms, std::string &error) {
    error.clear();
    if (timeout_ms < 0 || timeout_ms > 60000) {
        error = "Bonjour timeout must be between 0 and 60000 ms";
        return false;
    }
    if (impl_->error.empty() && !impl_->failed && impl_->connection.poll(timeout_ms, error)) {
        if (impl_->error.empty() && !impl_->failed) return true;
    }
    if (error.empty()) error = impl_->failed ? "Bonjour registration callback failed" : impl_->error;
    return false;
}

} // namespace xdb::wifi::mdns::detail
