#include "wifi/WindowsDnsSd.h"
#include "wifi/DnsSd.h"
#include "transport/SocketPlatform.h"
#include "util/Bytes.h"

#include <windows.h>
#include <windns.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <set>

#ifdef XDB_TEST_WINDOWS_DNSSD
// Test only: inject the optional system library without replacing Win32 imports.
extern HMODULE xdb_test_dns_load();
extern FARPROC xdb_test_dns_symbol(HMODULE, const char *);
extern BOOL xdb_test_dns_free(HMODULE);
#define LoadLibraryExW(...) xdb_test_dns_load()
#define GetProcAddress xdb_test_dns_symbol
#define FreeLibrary xdb_test_dns_free
#endif

namespace xdb::wifi::mdns::detail {
namespace {
using Clock = std::chrono::steady_clock;

// Optional system APIs: do not add a load-time DNS-SD dependency to the SDK.
struct Library {
    HMODULE module = nullptr;
    decltype(&::DnsServiceBrowse) browse = nullptr;
    decltype(&::DnsServiceBrowseCancel) cancel_browse = nullptr;
    decltype(&::DnsServiceResolve) resolve = nullptr;
    decltype(&::DnsServiceResolveCancel) cancel_resolve = nullptr;
    decltype(&::DnsFree) free_records = nullptr;
    decltype(&::DnsServiceFreeInstance) free_instance = nullptr;
    ~Library() { if (module) FreeLibrary(module); }
    bool open(std::string &error) {
        module = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) { error = "Windows DNS-SD library is unavailable"; return false; }
#define LOAD(member, symbol) \
        member = reinterpret_cast<decltype(member)>(GetProcAddress(module, #symbol)); \
        if (!member) { error = "Windows DNS-SD symbol is unavailable: " #symbol; return false; }
        LOAD(browse, DnsServiceBrowse)
        LOAD(cancel_browse, DnsServiceBrowseCancel)
        LOAD(resolve, DnsServiceResolve)
        LOAD(cancel_resolve, DnsServiceResolveCancel)
        LOAD(free_records, DnsFree)
        LOAD(free_instance, DnsServiceFreeInstance)
#undef LOAD
#ifndef XDB_TEST_WINDOWS_DNSSD
        // Cancellation may complete after the scan returns. The last context can
        // be released inside a dnsapi callback; keep its return address mapped.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(browse), &pinned)) {
            error = "Cannot retain Windows DNS-SD callback module";
            return false;
        }
#endif
        return true;
    }
};

std::string utf8(const wchar_t *text, size_t limit = 1024) {
    if (!text) return {};
    size_t length = 0;
    while (length <= limit && text[length]) ++length;
    if (!length || length > limit) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                       static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length),
                            result.data(), size, nullptr, nullptr) != size) return {};
    return result;
}

std::wstring wide(std::string_view text) {
    if (text.empty() || text.size() > 4096 || text.find('\0') != std::string_view::npos) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            result.data(), size) != size) return {};
    return result;
}

std::string name(const wchar_t *text) {
    auto result = utf8(text);
    if (!result.empty() && result.back() != '.') result.push_back('.');
    return result;
}

std::string normalized_name(std::string value) {
    for (auto &ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return value;
}

std::optional<ResourceRecord> convert_record(const DNS_RECORDW &input) {
    // Request/response strings use Unicode with this Windows API. Never guess an
    // ANSI code page for authentication-related TXT values.
    if (input.Flags.S.CharSet != DnsCharSetUnicode) return {};
    ResourceRecord record{name(input.pName), input.wType, {}, input.Flags.S.Delete ? 0U : input.dwTtl};
    if (record.name.empty()) return {};
    if (input.wType == DNS_TYPE_A) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(&input.Data.A.IpAddress);
        record.data.assign(bytes, bytes + 4);
    } else if (input.wType == DNS_TYPE_AAAA) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(&input.Data.AAAA.Ip6Address);
        record.data.assign(bytes, bytes + 16);
    } else if (input.wType == DNS_TYPE_PTR) {
        record.data = dns_name_data(name(input.Data.PTR.pNameHost));
        if (record.data.empty()) return {};
    } else if (input.wType == DNS_TYPE_SRV) {
        util::append_be16(record.data, input.Data.SRV.wPriority);
        util::append_be16(record.data, input.Data.SRV.wWeight);
        util::append_be16(record.data, input.Data.SRV.wPort);
        auto target = dns_name_data(name(input.Data.SRV.pNameTarget));
        if (target.empty()) return {};
        record.data.insert(record.data.end(), target.begin(), target.end());
    } else if (input.wType == DNS_TYPE_TEXT) {
        if (input.Data.TXT.dwStringCount > 64) return {};
        for (DWORD i = 0; i < input.Data.TXT.dwStringCount; ++i) {
            const auto text = utf8(input.Data.TXT.pStringArray[i], 255);
            if (text.empty() || text.size() > 255 || record.data.size() + 1 + text.size() > 4096) return {};
            record.data.push_back(static_cast<uint8_t>(text.size()));
            record.data.insert(record.data.end(), text.begin(), text.end());
        }
    } else return {};
    return record;
}

struct BrowseState {
    std::mutex mutex;
    std::condition_variable changed;
    RecordCache cache;
    std::set<std::pair<uint32_t, std::string>> instances;
    size_t replies = 0;
    bool stopped = false;
    std::string error;
};

void add_record(BrowseState &state, const NativeInterface &iface, const ResourceRecord &record) {
    const auto packet = record_packet(record);
    if (!packet.empty()) state.cache.add_packet(iface.index, iface.name, packet,
                                                Clock::now(), iface.ipv6_index);
}

struct Query {
    std::shared_ptr<Library> api;
    std::shared_ptr<BrowseState> state;
    NativeInterface iface;
    std::wstring owner, query_name;
    DNS_SERVICE_BROWSE_REQUEST browse{};
    DNS_SERVICE_RESOLVE_REQUEST resolve{};
    DNS_SERVICE_CANCEL cancellation{};
    bool resolving = false;

    // A pending native request owns a shared callback context. Cancellation is
    // asynchronous: its final callback, not the scan's stack frame, releases it.
    static void WINAPI browsed(DWORD status, void *context, PDNS_RECORD records) noexcept {
        auto *retained = static_cast<std::shared_ptr<Query> *>(context);
        auto query = *retained;
        std::unique_ptr<std::shared_ptr<Query>> finished(status == ERROR_CANCELLED ? retained : nullptr);
        try {
            std::lock_guard lock(query->state->mutex);
            if (!query->state->stopped) {
                if (status != ERROR_SUCCESS && status != ERROR_CANCELLED)
                    query->state->error = "Windows DNS-SD browse: " + std::to_string(status);
                for (auto *rr = reinterpret_cast<DNS_RECORDW *>(records); rr; rr = rr->pNext) {
                    if (++query->state->replies > 2048) {
                        query->state->error = "Windows DNS-SD reply limit reached";
                        break;
                    }
                    if (rr->Flags.S.CharSet != DnsCharSetUnicode) {
                        query->state->error = "Unsupported Windows DNS-SD record encoding";
                        break;
                    }
                    const auto record = convert_record(*rr);
                    if (!record) continue;
                    add_record(*query->state, query->iface, *record);
                    if (rr->wType == DNS_TYPE_PTR && record->ttl &&
                        normalized_name(record->name) == "_remotepairing._tcp.local.") {
                        auto instance = name(rr->Data.PTR.pNameHost);
                        if (normalized_name(instance).ends_with("._remotepairing._tcp.local.") &&
                            query->state->instances.size() < 256)
                            query->state->instances.emplace(query->iface.index, std::move(instance));
                    }
                }
                query->state->changed.notify_one();
            }
        } catch (...) {
            std::lock_guard lock(query->state->mutex);
            query->state->replies = 2049;
            query->state->changed.notify_one();
        }
        if (records) query->api->free_records(records, DnsFreeRecordList);
    }
    static void WINAPI resolved(DWORD status, void *context, PDNS_SERVICE_INSTANCE instance) noexcept {
        std::unique_ptr<std::shared_ptr<Query>> retained(static_cast<std::shared_ptr<Query> *>(context));
        auto query = *retained;
        try {
            std::lock_guard lock(query->state->mutex);
            if (!query->state->stopped && status == ERROR_SUCCESS && instance &&
                instance->dwPropertyCount <= 64) {
                const auto owner = name(instance->pszInstanceName);
                const auto target = name(instance->pszHostName);
                if (normalized_name(owner) == normalized_name(utf8(query->owner.c_str())) &&
                    !target.empty() && instance->wPort) {
                    ResourceRecord srv{owner, DNS_TYPE_SRV, {}, 120};
                    util::append_be16(srv.data, instance->wPriority);
                    util::append_be16(srv.data, instance->wWeight);
                    util::append_be16(srv.data, instance->wPort);
                    const auto target_data = dns_name_data(target);
                    srv.data.insert(srv.data.end(), target_data.begin(), target_data.end());
                    add_record(*query->state, query->iface, srv);
                    ResourceRecord txt{owner, DNS_TYPE_TEXT, {}, 120};
                    bool valid = !instance->dwPropertyCount || (instance->keys && instance->values);
                    for (DWORD i = 0; valid && i < instance->dwPropertyCount; ++i) {
                        const auto key = utf8(instance->keys[i], 255);
                        const auto value = utf8(instance->values[i], 255);
                        const auto item = key + "=" + value;
                        valid = !key.empty() && key.find('=') == std::string::npos && item.size() <= 255 &&
                                txt.data.size() + 1 + item.size() <= 4096;
                        if (valid) {
                            txt.data.push_back(static_cast<uint8_t>(item.size()));
                            txt.data.insert(txt.data.end(), item.begin(), item.end());
                        }
                    }
                    if (valid) add_record(*query->state, query->iface, txt);
                    if (instance->ip4Address) {
                        const auto *bytes = reinterpret_cast<const uint8_t *>(instance->ip4Address);
                        add_record(*query->state, query->iface, {target, DNS_TYPE_A, {bytes, bytes + 4}, 120});
                    }
                    if (instance->ip6Address) {
                        const auto *bytes = reinterpret_cast<const uint8_t *>(instance->ip6Address);
                        add_record(*query->state, query->iface, {target, DNS_TYPE_AAAA, {bytes, bytes + 16}, 120});
                    }
                }
                query->state->changed.notify_one();
            }
        } catch (...) {
            std::lock_guard lock(query->state->mutex);
            query->state->replies = 2049;
            query->state->changed.notify_one();
        }
        if (instance) query->api->free_instance(instance);
    }
};
} // namespace

BrowseResult windows_browse(const BrowseOptions &options) {
    BrowseResult result;
    const auto deadline = Clock::now() + options.timeout;
    const auto cancelled = [&] {
        result.cancelled = options.should_cancel && options.should_cancel();
        return result.cancelled;
    };
    if (cancelled() || options.timeout.count() <= 0) return result;
    auto api = std::make_shared<Library>();
    std::string error;
    if (!api->open(error) || !transport::initialize_sockets(error)) {
        result.warnings.push_back(error);
        return result;
    }
    const auto interfaces = native_interfaces(result.warnings);
    auto state = std::make_shared<BrowseState>();
    std::vector<std::shared_ptr<Query>> queries;
    std::set<std::pair<uint32_t, std::string>> requested;
    auto start = [&](const NativeInterface &iface, const std::string &owner, bool resolving) {
        auto item = std::make_shared<Query>();
        item->api = api; item->state = state; item->iface = iface; item->owner = wide(owner);
        item->query_name = item->owner;
        // Win32 accepts a trailing root dot but produces no browse results on
        // tested Windows 11. Keep cache names absolute, API query names undotted.
        if (!item->query_name.empty() && item->query_name.back() == L'.') item->query_name.pop_back();
        item->resolving = resolving;
        if (item->owner.empty()) { error = "Invalid Windows DNS-SD query name"; return false; }
        auto *retained = new std::shared_ptr<Query>(item);
        DWORD code;
        if (resolving) {
            item->resolve.Version = DNS_QUERY_REQUEST_VERSION1;
            item->resolve.InterfaceIndex = iface.index;
            item->resolve.QueryName = item->query_name.data();
            item->resolve.pResolveCompletionCallback = &Query::resolved;
            item->resolve.pQueryContext = retained;
            code = api->resolve(&item->resolve, &item->cancellation);
        } else {
            item->browse.Version = DNS_QUERY_REQUEST_VERSION1;
            item->browse.InterfaceIndex = iface.index;
            item->browse.QueryName = item->query_name.c_str();
            item->browse.pBrowseCallback = &Query::browsed;
            item->browse.pQueryContext = retained;
            code = api->browse(&item->browse, &item->cancellation);
        }
        if (code != DNS_REQUEST_PENDING) {
            delete retained;
            error = "Windows start DNS-SD query: " + std::to_string(code);
            return false;
        }
        queries.push_back(std::move(item));
        return true;
    };
    for (const auto &iface : interfaces) {
        if (cancelled() || Clock::now() >= deadline) break;
        if (!start(iface, "_remotepairing._tcp.local.", false)) break;
    }
    result.available = !queries.empty() && error.empty();
    while (result.available && !cancelled() && Clock::now() < deadline) {
        std::vector<std::pair<uint32_t, std::string>> missing;
        {
            std::unique_lock lock(state->mutex);
            if (!state->error.empty() || state->replies > 2048) break;
            for (const auto &instance : state->instances)
                if (requested.insert(instance).second) missing.push_back(instance);
            if (missing.empty()) state->changed.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::milliseconds(50)));
        }
        for (const auto &[index, owner] : missing) {
            const auto iface = std::find_if(interfaces.begin(), interfaces.end(),
                                           [&](const auto &entry) { return entry.index == index; });
            if (cancelled() || Clock::now() >= deadline || iface == interfaces.end() ||
                !start(*iface, owner, true)) break;
        }
        if (!error.empty()) break;
    }
    {
        std::lock_guard lock(state->mutex);
        state->stopped = true;
        result.advertisements = state->cache.snapshot();
        const auto &warnings = state->cache.warnings();
        result.warnings.insert(result.warnings.end(), warnings.begin(), warnings.end());
        if (!state->error.empty()) error = state->error;
        else if (state->replies > 2048) error = "Windows DNS-SD callback failed";
    }
    if (!error.empty()) { result.warnings.push_back(error); result.available = false; }
    for (const auto &query : queries) {
        if (query->resolving) api->cancel_resolve(&query->cancellation);
        else api->cancel_browse(&query->cancellation);
    }
    return result;
}

} // namespace xdb::wifi::mdns::detail
