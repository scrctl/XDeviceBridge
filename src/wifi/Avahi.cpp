#include "wifi/Avahi.h"
#include "wifi/DnsSd.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>
#include <dlfcn.h>
#include <net/if.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <tuple>

namespace xdb::wifi::mdns::detail {
namespace {
using Clock = std::chrono::steady_clock;

#define XDB_AVAHI_SYMBOLS(X) \
    X(avahi_simple_poll_new) X(avahi_simple_poll_free) X(avahi_simple_poll_get) \
    X(avahi_simple_poll_iterate) X(avahi_client_new) X(avahi_client_free) \
    X(avahi_client_errno) X(avahi_strerror) X(avahi_record_browser_new) \
    X(avahi_entry_group_new) X(avahi_entry_group_add_record) X(avahi_entry_group_commit)

class Library {
  public:
    ~Library() { if (handle) dlclose(handle); }
    Library() = default;
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;
    bool open(std::string &error) {
        // Do not make the server library fail to load on machines without Avahi.
        handle = dlopen("libavahi-client.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!handle) { error = "Avahi client library is unavailable"; return false; }
#define LOAD(name) \
        name = reinterpret_cast<decltype(name)>(dlsym(handle, #name)); \
        if (!name) { error = "Avahi symbol is unavailable: " #name; return false; }
        XDB_AVAHI_SYMBOLS(LOAD)
#undef LOAD
        return true;
    }
#define DECLARE(name) decltype(&::name) name = nullptr;
    XDB_AVAHI_SYMBOLS(DECLARE)
#undef DECLARE
  private:
    void *handle = nullptr;
};
#undef XDB_AVAHI_SYMBOLS

struct Connection {
    Library api;
    AvahiSimplePoll *loop = nullptr;
    AvahiClient *client = nullptr;
    bool failed = false;
    std::string error;

    ~Connection() {
        // The client owns its browsers and entry groups; free it before the loop/library.
        if (client) api.avahi_client_free(client);
        if (loop) api.avahi_simple_poll_free(loop);
    }
    void fail(AvahiClient *source, std::string_view operation) {
        failed = true;
        error = "Avahi " + std::string(operation) + ": " + api.avahi_strerror(api.avahi_client_errno(source));
    }
    static void changed(AvahiClient *source, AvahiClientState state, void *context) noexcept {
        if (state != AVAHI_CLIENT_FAILURE) return;
        auto &connection = *static_cast<Connection *>(context);
        try { connection.fail(source, "connection"); }
        catch (...) { connection.failed = true; }
    }
    bool open() {
        if (!api.open(error)) return false;
        loop = api.avahi_simple_poll_new();
        if (!loop) { error = "Cannot allocate Avahi event loop"; return false; }
        int code = 0;
        client = api.avahi_client_new(api.avahi_simple_poll_get(loop), static_cast<AvahiClientFlags>(0),
                                      &changed, this, &code);
        if (!client || failed) {
            if (error.empty()) error = std::string("Avahi connect: ") + api.avahi_strerror(code);
            return false;
        }
        return true;
    }
    bool poll(int timeout_ms) {
        if (!failed && api.avahi_simple_poll_iterate(loop, timeout_ms) == 0 && !failed) return true;
        if (error.empty()) error = "Avahi event loop stopped";
        return false;
    }
};

struct Browser {
    Connection connection;
    RecordCache cache;
    std::set<uint32_t> interfaces;
    std::set<std::tuple<uint32_t, uint16_t, std::string>> queries;
    using RecordKey = std::tuple<uint32_t, std::string, uint16_t, std::vector<uint8_t>>;
    // Avahi can report the same record separately over IPv4 and IPv6.
    std::map<RecordKey, std::set<AvahiProtocol>> protocols;
    std::string error;
    size_t replies = 0;

    static void reply(AvahiRecordBrowser *, AvahiIfIndex index, AvahiProtocol protocol,
                      AvahiBrowserEvent event, const char *name, uint16_t record_class, uint16_t type,
                      const void *data, size_t length, AvahiLookupResultFlags, void *context) noexcept {
        auto &browser = *static_cast<Browser *>(context);
        try {
            if (++browser.replies > 2048) { browser.error = "Avahi reply limit reached"; return; }
            if (event == AVAHI_BROWSER_FAILURE) {
                browser.connection.fail(browser.connection.client, "browse");
                return;
            }
            if (event != AVAHI_BROWSER_NEW && event != AVAHI_BROWSER_REMOVE) return;
            if (!name || index <= 0 || record_class != 1 || length > 16384 || (length && !data) ||
                (protocol != AVAHI_PROTO_INET && protocol != AVAHI_PROTO_INET6)) return;
            std::array<char, IF_NAMESIZE> interface_name{};
            if (!if_indextoname(static_cast<unsigned>(index), interface_name.data())) return;
            if (!browser.interfaces.contains(index) && browser.interfaces.size() >= 64) {
                browser.error = "Avahi interface limit reached";
                return;
            }
            ResourceRecord record{name, type, {}, event == AVAHI_BROWSER_NEW ? 120U : 0U};
            if (length) {
                const auto *bytes = static_cast<const uint8_t *>(data);
                record.data.assign(bytes, bytes + length);
            }
            const RecordKey key{index, record.name, type, record.data};
            if (event == AVAHI_BROWSER_NEW) {
                if (!browser.protocols.contains(key) && browser.protocols.size() >= 512) {
                    browser.error = "Avahi record limit reached";
                    return;
                }
                browser.protocols[key].insert(protocol);
            } else {
                const auto found = browser.protocols.find(key);
                if (found == browser.protocols.end()) return;
                found->second.erase(protocol);
                if (!found->second.empty()) return;
                browser.protocols.erase(found);
            }
            const auto packet = record_packet(record);
            // Native subscriptions own TTL expiry and explicitly report removals.
            if (!packet.empty() && browser.cache.add_packet(index, interface_name.data(), packet))
                browser.interfaces.insert(index);
        } catch (...) { browser.replies = 2049; }
    }

    bool query(uint32_t index, uint16_t type, const std::string &name) {
        const auto key = std::tuple(index, type, name);
        if (queries.contains(key)) return true;
        if (queries.size() >= 512) { error = "Avahi query limit reached"; return false; }
        auto &api = connection.api;
        if (!api.avahi_record_browser_new(connection.client, index ? static_cast<int>(index) : AVAHI_IF_UNSPEC,
            AVAHI_PROTO_UNSPEC, name.c_str(), 1, type, AVAHI_LOOKUP_USE_MULTICAST, &reply, this)) {
            error = std::string("Avahi start query: ") + api.avahi_strerror(api.avahi_client_errno(connection.client));
            return false;
        }
        queries.insert(key);
        return true;
    }
};
} // namespace

BrowseResult avahi_browse(const BrowseOptions &options) {
    BrowseResult result;
    Browser browser;
    const auto deadline = Clock::now() + options.timeout;
    const auto cancelled = [&] {
        result.cancelled = options.should_cancel && options.should_cancel();
        return result.cancelled;
    };
    if (cancelled() || options.timeout.count() <= 0) return result;
    if (!browser.connection.open() || !browser.query(0, 12, "_remotepairing._tcp.local.")) {
        result.warnings.push_back(browser.connection.error.empty() ? browser.error : browser.connection.error);
        return result;
    }
    result.available = true;
    while (!cancelled() && Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (!browser.connection.poll(static_cast<int>(std::clamp<int64_t>(remaining, 0, 50)))) {
            result.available = false;
            break;
        }
        if (!browser.error.empty() || browser.replies > 2048) break;
        for (const auto index : browser.interfaces) {
            for (const auto &[type, name] : browser.cache.queries(index)) {
                if (cancelled() || Clock::now() >= deadline || !browser.query(index, type, name)) break;
            }
            if (!browser.error.empty()) break;
        }
    }
    result.advertisements = browser.cache.snapshot();
    result.warnings = browser.cache.warnings();
    if (!browser.connection.error.empty()) result.warnings.push_back(browser.connection.error);
    if (!browser.error.empty()) result.warnings.push_back(browser.error);
    else if (browser.replies > 2048) result.warnings.push_back("Avahi callback failed");
    return result;
}

struct AvahiAdvertiser::Impl {
    Connection connection;
    AvahiEntryGroup *group = nullptr;
    bool failed = false;
    std::string error;

    static void changed(AvahiEntryGroup *, AvahiEntryGroupState state, void *context) noexcept {
        auto &impl = *static_cast<Impl *>(context);
        if (state != AVAHI_ENTRY_GROUP_COLLISION && state != AVAHI_ENTRY_GROUP_FAILURE) return;
        impl.failed = true;
        try { impl.error = "Avahi pairing advertisement failed or its name conflicts"; }
        catch (...) {}
    }
};

AvahiAdvertiser::AvahiAdvertiser(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AvahiAdvertiser::~AvahiAdvertiser() = default;

std::unique_ptr<AvahiAdvertiser> AvahiAdvertiser::start(const HostAdvertisement &host, std::string &error) {
    error.clear();
    auto impl = std::make_unique<Impl>();
    if (!impl->connection.open()) { error = impl->connection.error; return {}; }
    auto &api = impl->connection.api;
    auto *client = impl->connection.client;
    impl->group = api.avahi_entry_group_new(client, &Impl::changed, impl.get());
    if (!impl->group) { error = "Cannot create Avahi pairing entry group"; return {}; }
    const auto interfaces = local_host_records(host, error);
    if (interfaces.empty()) return {};
    for (const auto &[index, host_records] : interfaces) {
        const auto records = host_resource_records(host_records, error);
        if (records.empty()) return {};
        for (const auto &record : records) {
            const auto flags = static_cast<AvahiPublishFlags>(AVAHI_PUBLISH_USE_MULTICAST |
                (record.type == 12 ? 0 : AVAHI_PUBLISH_UNIQUE));
            if (api.avahi_entry_group_add_record(impl->group, index, AVAHI_PROTO_UNSPEC, flags,
                record.name.c_str(), 1, record.type, record.ttl, record.data.data(), record.data.size()) < 0) {
                error = std::string("Avahi add pairing record: ") + api.avahi_strerror(api.avahi_client_errno(client));
                return {};
            }
        }
    }
    if (api.avahi_entry_group_commit(impl->group) < 0) {
        error = std::string("Avahi commit pairing records: ") + api.avahi_strerror(api.avahi_client_errno(client));
        return {};
    }
    return std::unique_ptr<AvahiAdvertiser>(new AvahiAdvertiser(std::move(impl)));
}

bool AvahiAdvertiser::poll(int timeout_ms, std::string &error) {
    error.clear();
    if (timeout_ms < 0 || timeout_ms > 60000) { error = "Avahi timeout must be between 0 and 60000 ms"; return false; }
    if (!impl_->failed && impl_->connection.poll(timeout_ms) && !impl_->failed) return true;
    error = impl_->failed ? impl_->error : impl_->connection.error;
    if (error.empty()) error = "Avahi advertisement callback failed";
    return false;
}

} // namespace xdb::wifi::mdns::detail
