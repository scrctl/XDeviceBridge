#include "wifi/Avahi.h"
#include "wifi/DnsSd.h"
#include "util/Bytes.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/simple-watch.h>
#include <dlfcn.h>
#include <net/if.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>

namespace {
using namespace xdb::wifi::mdns;
int failures = 0;
void check(bool ok, const char *label) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}
struct Fake {
    bool library_missing = false, daemon_missing = false, fail_poll = false;
    bool withdraw_one = false, withdraw_both = false, conflict = false, fail_commit = false;
    int fail_record = -1;
    std::string missing_symbol;
    unsigned queries = 0, commits = 0, freed = 0, loaded = 0;
    AvahiEntryGroupCallback group_callback = nullptr;
    void *group_context = nullptr;
    std::deque<std::function<void()>> events;
    std::vector<std::string> cleanup;
    std::vector<detail::ResourceRecord> records;
} fake;
const std::string service = "phone._remotepairing._tcp.local.";
HostAdvertisement host{"unit-host", "xdb - unit", "xdb - Linux", "AQIDBAUG", 49152};
} // namespace

extern "C" {
AvahiSimplePoll *avahi_simple_poll_new() { return reinterpret_cast<AvahiSimplePoll *>(&fake); }
void avahi_simple_poll_free(AvahiSimplePoll *) { fake.cleanup.push_back("poll"); }
const AvahiPoll *avahi_simple_poll_get(AvahiSimplePoll *) { return reinterpret_cast<const AvahiPoll *>(&fake); }
int avahi_simple_poll_iterate(AvahiSimplePoll *, int timeout) {
    if (fake.fail_poll) return -1;
    if (fake.events.empty()) { ::poll(nullptr, 0, timeout); return 0; }
    auto event = std::move(fake.events.front());
    fake.events.pop_front(); event();
    return 0;
}
AvahiClient *avahi_client_new(const AvahiPoll *, AvahiClientFlags flags, AvahiClientCallback callback,
                               void *context, int *error) {
    check(flags == 0, "missing daemon does not start an indefinite reconnection loop");
    if (fake.daemon_missing) { *error = -1; return nullptr; }
    auto *client = reinterpret_cast<AvahiClient *>(&fake);
    callback(client, AVAHI_CLIENT_S_RUNNING, context);
    return client;
}
void avahi_client_free(AvahiClient *) {
    ++fake.freed;
    fake.events.clear();
    fake.cleanup.push_back("client");
}
int avahi_client_errno(AvahiClient *) { return -1; }
const char *avahi_strerror(int) { return "test daemon error"; }
AvahiRecordBrowser *avahi_record_browser_new(AvahiClient *, AvahiIfIndex interface_index, AvahiProtocol protocol,
    const char *name, uint16_t record_class, uint16_t type, AvahiLookupFlags flags,
    AvahiRecordBrowserCallback callback, void *context) {
    ++fake.queries;
    const unsigned index = if_nametoindex("lo");
    check(index && (interface_index == AVAHI_IF_UNSPEC || interface_index == static_cast<int>(index)) &&
          protocol == AVAHI_PROTO_UNSPEC && record_class == 1 && flags == AVAHI_LOOKUP_USE_MULTICAST,
          "queries retain local multicast and interface scope");
    std::vector<uint8_t> data;
    if (type == 12) data = detail::dns_name_data(service);
    if (type == 33) {
        data.resize(6, 0); xdb::util::write_be16(data.data() + 4, 49152);
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
    const std::string owner = name;
    for (const auto source : {AVAHI_PROTO_INET, AVAHI_PROTO_INET6}) {
        fake.events.push_back([=] {
            callback(nullptr, index, source, AVAHI_BROWSER_NEW, owner.c_str(), 1, type, data.data(), data.size(),
                     AVAHI_LOOKUP_RESULT_MULTICAST, context);
        });
    }
    if (type == 28 && (fake.withdraw_one || fake.withdraw_both)) {
        fake.events.push_back([=] {
            const auto removed = detail::dns_name_data(service);
            callback(nullptr, index, AVAHI_PROTO_INET, AVAHI_BROWSER_REMOVE, "_remotepairing._tcp.local.",
                     1, 12, removed.data(), removed.size(), AVAHI_LOOKUP_RESULT_MULTICAST, context);
            if (fake.withdraw_both)
                callback(nullptr, index, AVAHI_PROTO_INET6, AVAHI_BROWSER_REMOVE, "_remotepairing._tcp.local.",
                         1, 12, removed.data(), removed.size(), AVAHI_LOOKUP_RESULT_MULTICAST, context);
        });
    }
    return reinterpret_cast<AvahiRecordBrowser *>(&fake);
}
AvahiEntryGroup *avahi_entry_group_new(AvahiClient *, AvahiEntryGroupCallback callback, void *context) {
    fake.group_callback = callback; fake.group_context = context;
    auto *group = reinterpret_cast<AvahiEntryGroup *>(&fake);
    callback(group, AVAHI_ENTRY_GROUP_UNCOMMITED, context);
    return group;
}
int avahi_entry_group_add_record(AvahiEntryGroup *, AvahiIfIndex index, AvahiProtocol protocol,
    AvahiPublishFlags flags, const char *name, uint16_t record_class, uint16_t type, uint32_t ttl,
    const void *data, size_t length) {
    if (fake.fail_record == static_cast<int>(fake.records.size())) return -1;
    check(index == 4 && protocol == AVAHI_PROTO_UNSPEC && record_class == 1 &&
          (flags & AVAHI_PUBLISH_USE_MULTICAST) && bool(flags & AVAHI_PUBLISH_UNIQUE) == (type != 12),
          "pairing records retain scope, multicast and uniqueness");
    const auto *bytes = static_cast<const uint8_t *>(data);
    fake.records.push_back({name, type, {bytes, bytes + length}, ttl});
    return 0;
}
int avahi_entry_group_commit(AvahiEntryGroup *group) {
    if (fake.fail_commit) return -1;
    ++fake.commits;
    fake.events.push_back([=] {
        fake.group_callback(group, fake.conflict ? AVAHI_ENTRY_GROUP_COLLISION : AVAHI_ENTRY_GROUP_ESTABLISHED,
                            fake.group_context);
    });
    return 0;
}
void *dlopen(const char *name, int flags) noexcept {
    ++fake.loaded;
    check(std::strcmp(name, "libavahi-client.so.3") == 0 && flags == (RTLD_NOW | RTLD_LOCAL),
          "Avahi is loaded locally only when requested");
    return fake.library_missing ? nullptr : &fake;
}
int dlclose(void *) noexcept { fake.cleanup.push_back("library"); return 0; }
void *dlsym(void *, const char *name) noexcept {
    if (name == fake.missing_symbol) return nullptr;
#define SYMBOL(value) if (std::strcmp(name, #value) == 0) return reinterpret_cast<void *>(&value);
    SYMBOL(avahi_simple_poll_new) SYMBOL(avahi_simple_poll_free) SYMBOL(avahi_simple_poll_get)
    SYMBOL(avahi_simple_poll_iterate) SYMBOL(avahi_client_new) SYMBOL(avahi_client_free)
    SYMBOL(avahi_client_errno) SYMBOL(avahi_strerror) SYMBOL(avahi_record_browser_new)
    SYMBOL(avahi_entry_group_new) SYMBOL(avahi_entry_group_add_record) SYMBOL(avahi_entry_group_commit)
#undef SYMBOL
    return nullptr;
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
    const auto result = avahi_browse(options);
    check(result.available && result.warnings.empty() && result.advertisements.size() == 1 && fake.queries == 5,
          "Avahi discovers and resolves a service through five scoped record queries");
    if (!result.advertisements.empty()) {
        const auto &ad = result.advertisements[0];
        check(ad.identifier == "peer" && ad.endpoints.size() == 2 && ad.endpoints[0].port == 49152 &&
              ad.endpoints[1].address == "fe80::7%" + std::to_string(if_nametoindex("lo")),
              "Avahi retains both endpoint families, TXT and actual SRV port");
    }
    check(fake.cleanup == std::vector<std::string>({"client", "poll", "library"}),
          "browsers are freed before their event loop and dynamic library");
    fake = {}; fake.withdraw_one = true;
    check(avahi_browse(options).advertisements.size() == 1, "one protocol removal keeps the other protocol's record");
    fake = {}; fake.withdraw_both = true;
    check(avahi_browse(options).advertisements.empty(), "the last protocol removal withdraws the service");
    fake = {}; options.should_cancel = [] { return true; };
    check(avahi_browse(options).cancelled && fake.loaded == 0, "pre-cancelled browsing does not load Avahi");
    options.should_cancel = {}; options.timeout = std::chrono::milliseconds(0);
    check(!avahi_browse(options).available && fake.loaded == 0, "zero-budget browsing does not load Avahi");
    options.timeout = std::chrono::milliseconds(100);
    fake = {}; fake.library_missing = true;
    check(!avahi_browse(options).available && fake.freed == 0, "missing library permits direct fallback");
    fake = {}; fake.missing_symbol = "avahi_record_browser_new";
    check(!avahi_browse(options).available && fake.cleanup == std::vector<std::string>({"library"}),
          "incomplete library is unloaded before creating a client");
    fake = {}; fake.daemon_missing = true;
    check(!avahi_browse(options).available && fake.cleanup == std::vector<std::string>({"poll", "library"}),
          "missing daemon cleans up before direct fallback");
    fake = {}; fake.fail_poll = true;
    check(!avahi_browse(options).available && fake.freed == 1, "failed event loop closes its browsers");
    std::string error;
    fake = {};
    {
        auto advertiser = AvahiAdvertiser::start(host, error);
        check(advertiser && error.empty() && fake.records.size() == 5 && fake.commits == 1,
              "all pairing records are committed as one group");
        check(!fake.records.empty() && fake.records[0].name == "_remotepairing-pairable-host._tcp.local." &&
              fake.records[0].data == dns_name_data("unit-host._remotepairing-pairable-host._tcp.local."),
              "Avahi preserves the pairing PTR identity");
        if (advertiser) check(advertiser->poll(0, error), "registration callback is accepted");
    }
    check(fake.freed == 1, "advertiser destruction withdraws the group");
    fake = {}; fake.fail_record = 2;
    check(!AvahiAdvertiser::start(host, error) && !error.empty() && fake.freed == 1 && fake.commits == 0,
          "partial record failure cannot publish an incomplete pairing group");
    fake = {}; fake.fail_commit = true;
    check(!AvahiAdvertiser::start(host, error) && !error.empty() && fake.freed == 1,
          "commit failure releases the group");
    fake = {}; fake.conflict = true;
    {
        auto advertiser = AvahiAdvertiser::start(host, error);
        check(advertiser && !advertiser->poll(0, error) && !error.empty(), "name conflicts do not silently rename identity");
    }
    if (!failures) std::puts("PASS: optional Avahi loading, scoped discovery, dual-protocol withdrawal and pairing cleanup");
    return failures ? 1 : 0;
}
