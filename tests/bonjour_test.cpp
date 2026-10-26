#include "wifi/Bonjour.h"
#include "wifi/DnsSd.h"
#include "util/Bytes.h"

#include <dns_sd.h>
#include <net/if.h>
#include <unistd.h>

#include <cstdio>
#include <deque>
#include <functional>
#include <tuple>

namespace {
using namespace xdb::wifi::mdns;
int failures = 0;
void check(bool ok, const char *label) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}

struct Fake {
    int fd[2]{-1, -1};
    unsigned opened = 0, closed = 0;
    bool unavailable = false, fail_receive = false, async_failure = false, withdraw = false;
    int fail_registration = -1;
    std::deque<std::function<void()>> events;
    std::vector<std::tuple<uint32_t, uint16_t, std::string>> queries;
    std::vector<std::pair<uint32_t, detail::ResourceRecord>> records;

    void enqueue(std::function<void()> event) {
        events.push_back(std::move(event));
        const char byte = 1;
        check(::write(fd[1], &byte, 1) == 1, "fake daemon signals an event");
    }
} fake;

const std::string service = "phone._remotepairing._tcp.local.";
HostAdvertisement host{"unit-host", "xdb - unit", "xdb - Mac", "AQIDBAUG", 49152};
} // namespace

extern "C" {
DNSServiceErrorType DNSSD_API DNSServiceCreateConnection(DNSServiceRef *ref) {
    ++fake.opened;
    if (fake.unavailable) return kDNSServiceErr_ServiceNotRunning;
    if (::pipe(fake.fd) != 0) return kDNSServiceErr_Unknown;
    *ref = reinterpret_cast<DNSServiceRef>(&fake);
    return kDNSServiceErr_NoError;
}
void DNSSD_API DNSServiceRefDeallocate(DNSServiceRef) {
    ++fake.closed;
    ::close(fake.fd[0]); ::close(fake.fd[1]);
    fake.fd[0] = fake.fd[1] = -1;
    fake.events.clear();
}
dnssd_sock_t DNSSD_API DNSServiceRefSockFD(DNSServiceRef) { return fake.fd[0]; }
DNSServiceErrorType DNSSD_API DNSServiceProcessResult(DNSServiceRef) {
    if (fake.fail_receive) return kDNSServiceErr_ServiceNotRunning;
    char byte;
    if (::read(fake.fd[0], &byte, 1) != 1 || fake.events.empty()) return kDNSServiceErr_Unknown;
    auto event = std::move(fake.events.front());
    fake.events.pop_front();
    event();
    return kDNSServiceErr_NoError;
}
DNSServiceErrorType DNSSD_API DNSServiceQueryRecord(DNSServiceRef *ref, DNSServiceFlags flags,
    uint32_t interface_index, const char *name, uint16_t type, uint16_t record_class,
    DNSServiceQueryRecordReply callback, void *context) {
    check((flags & kDNSServiceFlagsShareConnection) && (flags & kDNSServiceFlagsForceMulticast),
          "queries use one connection and local multicast scope");
    check(record_class == 1, "queries retain the IN class");
    fake.queries.emplace_back(interface_index, type, name);
    const uint32_t index = if_nametoindex("lo0");
    check(index != 0 && (!interface_index || interface_index == index), "dependent queries preserve interface scope");
    std::vector<uint8_t> data;
    if (type == 12) data = detail::dns_name_data(service);
    if (type == 33) {
        data.resize(6, 0);
        xdb::util::write_be16(data.data() + 4, 49152);
        const auto target = detail::dns_name_data("phone.local.");
        data.insert(data.end(), target.begin(), target.end());
    }
    if (type == 16) {
        const std::string field = "identifier=peer";
        data.push_back(static_cast<uint8_t>(field.size()));
        data.insert(data.end(), field.begin(), field.end());
    }
    if (type == 1) data = {198, 51, 100, 7};
    if (type == 28) { data.resize(16, 0); data[0] = 0xfe; data[1] = 0x80; data[15] = 7; }
    const auto reference = *ref;
    const std::string owner = name;
    fake.enqueue([=] {
        callback(reference, kDNSServiceFlagsAdd, index, kDNSServiceErr_NoError, owner.c_str(), type, 1,
                 static_cast<uint16_t>(data.size()), data.data(), 120, context);
    });
    if (type == 28 && fake.withdraw) {
        fake.enqueue([=] {
            const auto removed = detail::dns_name_data(service);
            callback(reference, 0, index, kDNSServiceErr_NoError, "_remotepairing._tcp.local.", 12, 1,
                     static_cast<uint16_t>(removed.size()), removed.data(), 0, context);
        });
    }
    return kDNSServiceErr_NoError;
}
DNSServiceErrorType DNSSD_API DNSServiceRegisterRecord(DNSServiceRef ref, DNSRecordRef *record,
    DNSServiceFlags flags, uint32_t index, const char *name, uint16_t type, uint16_t record_class,
    uint16_t length, const void *data, uint32_t ttl, DNSServiceRegisterRecordReply callback, void *context) {
    if (fake.fail_registration == static_cast<int>(fake.records.size())) return kDNSServiceErr_BadParam;
    check(flags == (type == 12 ? kDNSServiceFlagsShared : kDNSServiceFlagsUnique) && record_class == 1,
          "pairing PTR is shared and identity records are unique");
    const auto *bytes = static_cast<const uint8_t *>(data);
    fake.records.push_back({index, {name, type, {bytes, bytes + length}, ttl}});
    *record = reinterpret_cast<DNSRecordRef>(&fake);
    const auto code = fake.async_failure ? kDNSServiceErr_NameConflict : kDNSServiceErr_NoError;
    const auto value = *record;
    fake.enqueue([=] { callback(ref, value, 0, code, context); });
    return kDNSServiceErr_NoError;
}
} // extern "C"

namespace xdb::wifi::mdns::detail {
std::vector<std::pair<uint32_t, HostRecords>> local_host_records(const HostAdvertisement &value, std::string &error) {
    error.clear();
    return {{4, {value, value.instance_identifier + "._remotepairing-pairable-host._tcp.local.",
                value.instance_identifier + ".local.", {"198.51.100.7", "fe80::7"}}}};
}
} // namespace xdb::wifi::mdns::detail

int main() {
    using namespace detail;
    BrowseOptions options;
    options.timeout = std::chrono::milliseconds(100);
    const auto result = bonjour_browse(options);
    const auto index = if_nametoindex("lo0");
    check(result.available && !result.cancelled && result.warnings.empty() && result.advertisements.size() == 1,
          "native discovery resolves records from asynchronous daemon replies");
    if (!result.advertisements.empty()) {
        const auto &ad = result.advertisements[0];
        check(ad.identifier == "peer" && ad.endpoints.size() == 2 &&
              ad.endpoints[0].port == 49152 && ad.endpoints[1].address == "fe80::7%" + std::to_string(index),
              "native discovery retains both address families and the SRV port");
    }
    check(fake.opened == 1 && fake.closed == 1 && fake.queries.size() == 5,
          "dependent queries share one owned connection and close together");
    fake = {};
    fake.withdraw = true;
    check(bonjour_browse(options).advertisements.empty() && fake.closed == 1,
          "a daemon remove callback withdraws a fully resolved service");
    fake = {};
    int cancel_checks = 0;
    options.should_cancel = [&] { return ++cancel_checks >= 4; };
    check(bonjour_browse(options).cancelled && fake.closed == 1, "in-progress cancellation closes pending queries");
    fake = {};
    options.should_cancel = [] { return true; };
    check(bonjour_browse(options).cancelled && fake.opened == 0, "pre-cancelled discovery never opens Bonjour");
    options.should_cancel = {};
    options.timeout = std::chrono::milliseconds(0);
    check(!bonjour_browse(options).available && fake.opened == 0, "zero budget never opens Bonjour");
    options.timeout = std::chrono::milliseconds(100);
    fake.unavailable = true;
    const auto missing = bonjour_browse(options);
    check(!missing.available && !missing.warnings.empty() && fake.closed == 0,
          "missing daemon reports unavailable for the direct-mDNS fallback");
    fake = {};
    fake.fail_receive = true;
    check(!bonjour_browse(options).available && fake.closed == 1, "daemon disconnect closes pending queries");
    fake = {};
    std::string error;
    {
        auto advertiser = BonjourAdvertiser::start(host, error);
        check(advertiser && error.empty() && fake.records.size() == 5, "pairing publishes all five native record types");
        if (advertiser) while (!fake.events.empty()) check(advertiser->poll(0, error), "registration callback accepted");
        check(!fake.records.empty() && fake.records[0].second.name == "_remotepairing-pairable-host._tcp.local." &&
              fake.records[0].second.data == dns_name_data("unit-host._remotepairing-pairable-host._tcp.local."),
              "native pairing preserves the original PTR identity");
    }
    check(fake.closed == 1 && fake.events.empty(), "advertiser destruction withdraws every registration");
    fake = {};
    fake.fail_registration = 2;
    check(!BonjourAdvertiser::start(host, error) && !error.empty() && fake.closed == 1 && fake.events.empty(),
          "partial registration failure withdraws earlier records before fallback");
    fake = {};
    fake.async_failure = true;
    {
        auto advertiser = BonjourAdvertiser::start(host, error);
        check(advertiser && !advertiser->poll(0, error) && !error.empty(), "asynchronous identity conflicts stop pairing");
    }
    check(fake.closed == 1, "failed advertiser is withdrawn");
    if (!failures) std::puts("PASS: Bonjour discovery, scoped queries, registration and cancellation cleanup");
    return failures ? 1 : 0;
}
