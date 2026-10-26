#include "wifi/WindowsDnsSd.h"
#include "wifi/DnsSd.h"
#include "transport/SocketPlatform.h"

#include <windows.h>
#include <windns.h>

#include <cstdio>
#include <cstring>
#include <functional>

namespace {
using namespace xdb::wifi::mdns;
int failures = 0;
void check(bool value, const char *label) {
    if (!value) { ++failures; std::printf("FAIL: %s\n", label); }
}
struct Browse {
    DNS_SERVICE_BROWSE_REQUEST request;
    bool cancelled = false;
};
struct Instance {
    DNS_SERVICE_INSTANCE value{};
    std::wstring name, host;
    std::vector<std::wstring> keys, values;
    std::vector<PWSTR> key_pointers, value_pointers;
    IP4_ADDRESS ipv4{};
    IP6_ADDRESS ipv6{};
    void bind() {
        value.pszInstanceName = name.data(); value.pszHostName = host.data();
        for (auto &key : keys) key_pointers.push_back(key.data());
        for (auto &item : values) value_pointers.push_back(item.data());
        value.dwPropertyCount = static_cast<DWORD>(keys.size());
        value.keys = key_pointers.data(); value.values = value_pointers.data();
        value.ip4Address = &ipv4; value.ip6Address = &ipv6;
    }
};
struct Fake {
    bool missing = false, fail_browse = false;
    bool late_cancel = false, late_resolve = false, withdrawn = false, failed_resolution = false;
    bool unsupported_encoding = false, unrelated_service = false;
    size_t loaded = 0, unloaded = 0, records_freed = 0, instances_freed = 0;
    size_t resolutions = 0;
    std::string missing_symbol;
    std::vector<std::unique_ptr<Browse>> browsers;
    std::vector<std::function<void()>> resolution_replies;
} fake;
const std::wstring phone = L"phone._remotepairing._tcp.local.";

void emit(Browse &browse, DWORD ttl = 120) {
    auto *record = new DNS_RECORDW{};
    record->pName = const_cast<PWSTR>(L"_remotepairing._tcp.local");
    record->wType = DNS_TYPE_PTR;
    record->Flags.S.CharSet = DnsCharSetUnicode;
    if (fake.unsupported_encoding) record->Flags.S.CharSet = DnsCharSetUtf8;
    record->dwTtl = ttl;
    record->Data.PTR.pNameHost = const_cast<PWSTR>(phone.c_str());
    if (fake.unrelated_service) record->Data.PTR.pNameHost = const_cast<PWSTR>(L"printer._ipp._tcp.local.");
    browse.request.pBrowseCallback(ERROR_SUCCESS, browse.request.pQueryContext,
                                   reinterpret_cast<PDNS_RECORD>(record));
}

DWORD WINAPI fake_browse(PDNS_SERVICE_BROWSE_REQUEST request, PDNS_SERVICE_CANCEL cancel) {
    check(request->Version == DNS_QUERY_REQUEST_VERSION1 &&
          std::wstring(request->QueryName) == L"_remotepairing._tcp.local",
          "only the pairing device service is browsed, without the unsupported root dot");
    if (fake.fail_browse) return ERROR_NOT_SUPPORTED;
    auto item = std::make_unique<Browse>(); item->request = *request;
    cancel->reserved = item.get(); fake.browsers.push_back(std::move(item));
    emit(*fake.browsers.back());
    return DNS_REQUEST_PENDING;
}
DWORD WINAPI cancel_browse(PDNS_SERVICE_CANCEL cancel) {
    auto *item = static_cast<Browse *>(cancel->reserved);
    if (!item || item->cancelled) return ERROR_INVALID_PARAMETER;
    item->cancelled = true;
    if (!fake.late_cancel)
        item->request.pBrowseCallback(ERROR_CANCELLED, item->request.pQueryContext, nullptr);
    return ERROR_SUCCESS;
}
DWORD WINAPI resolve(PDNS_SERVICE_RESOLVE_REQUEST request, PDNS_SERVICE_CANCEL cancel) {
    ++fake.resolutions;
    check(std::wstring(request->QueryName) == L"phone._remotepairing._tcp.local" && request->InterfaceIndex != 0,
          "resolution preserves the instance and interface");
    cancel->reserved = nullptr;
    if (fake.failed_resolution) {
        request->pResolveCompletionCallback(ERROR_TIMEOUT, request->pQueryContext, nullptr);
        return DNS_REQUEST_PENDING;
    }
    auto *out = new Instance;
    out->name = phone; out->host = L"phone.local";
    out->keys = {L"identifier", L"authTag", L"name"};
    out->values = {L"phone-identifier", L"AQIDBAUG", L"Test Phone"};
    out->value.wPort = 49152;
    inet_pton(AF_INET, "192.0.2.7", &out->ipv4);
    inet_pton(AF_INET6, "fe80::7", &out->ipv6);
    out->bind();
    if (fake.late_resolve) {
        auto copy = *request;
        fake.resolution_replies.push_back([copy, out] {
            copy.pResolveCompletionCallback(ERROR_SUCCESS, copy.pQueryContext, &out->value);
        });
    } else request->pResolveCompletionCallback(ERROR_SUCCESS, request->pQueryContext, &out->value);
    if (fake.withdrawn) for (auto &item : fake.browsers) emit(*item, 0);
    return DNS_REQUEST_PENDING;
}
DWORD WINAPI cancel_resolve(PDNS_SERVICE_CANCEL) { return ERROR_INVALID_PARAMETER; }
void WINAPI free_records(PVOID records, DNS_FREE_TYPE type) {
    check(type == DnsFreeRecordList, "native record lists are freed with the matching allocator");
    ++fake.records_freed;
    delete static_cast<DNS_RECORDW *>(records);
}
void WINAPI free_instance(PDNS_SERVICE_INSTANCE instance) {
    ++fake.instances_freed;
    delete reinterpret_cast<Instance *>(instance);
}
} // namespace

HMODULE xdb_test_dns_load() {
    if (fake.missing) return nullptr;
    ++fake.loaded; return reinterpret_cast<HMODULE>(1);
}
BOOL xdb_test_dns_free(HMODULE) { ++fake.unloaded; return TRUE; }
FARPROC xdb_test_dns_symbol(HMODULE, const char *symbol) {
    if (fake.missing_symbol == symbol) return nullptr;
#define SYMBOL(name, function) if (std::strcmp(symbol, #name) == 0) return reinterpret_cast<FARPROC>(&function);
    SYMBOL(DnsServiceBrowse, fake_browse)
    SYMBOL(DnsServiceBrowseCancel, cancel_browse)
    SYMBOL(DnsServiceResolve, resolve)
    SYMBOL(DnsServiceResolveCancel, cancel_resolve)
    SYMBOL(DnsFree, free_records)
    SYMBOL(DnsServiceFreeInstance, free_instance)
#undef SYMBOL
    return nullptr;
}

namespace xdb::wifi::mdns::detail {
std::vector<NativeInterface> native_interfaces(std::vector<std::string> &) {
    return {{4, 14, "ethernet"}, {8, 18, "wifi"}};
}
} // namespace xdb::wifi::mdns::detail

int main() {
    using namespace xdb::wifi::mdns;
    BrowseOptions options; options.timeout = std::chrono::milliseconds(60);
    auto result = detail::windows_browse(options);
    check(result.available && result.warnings.empty() && result.advertisements.size() == 1,
          "native browse resolves a PTR-only response");
    if (!result.advertisements.empty()) {
        const auto &phone = result.advertisements[0];
        check(phone.identifier == "phone-identifier" && phone.auth_tag == "AQIDBAUG" &&
              phone.name == "Test Phone" && phone.endpoints.size() == 4, "TXT and per-interface addresses survive");
        bool scope14 = false, scope18 = false;
        for (const auto &endpoint : phone.endpoints) {
            scope14 |= endpoint.address == "fe80::7%14";
            scope18 |= endpoint.address == "fe80::7%18";
        }
        check(scope14 && scope18,
              "IPv6 link-local scope uses the IPv6 index rather than the cache index");
    }
    check(fake.loaded == fake.unloaded && fake.records_freed == 2 && fake.resolutions == 2,
          "browse requests, records and native library are released");
    fake = {}; fake.missing = true;
    check(!detail::windows_browse(options).available, "missing library permits direct fallback");
    fake = {}; fake.missing_symbol = "DnsServiceBrowse";
    check(!detail::windows_browse(options).available && fake.loaded == fake.unloaded,
          "missing API permits fallback and releases the library");
    fake = {}; fake.fail_browse = true;
    check(!detail::windows_browse(options).available && fake.loaded == fake.unloaded,
          "query startup failure cleans up before fallback");
    fake = {}; fake.withdrawn = true;
    check(detail::windows_browse(options).advertisements.empty(), "TTL zero removes a service despite late resolution");
    fake = {}; fake.unsupported_encoding = true;
    check(!detail::windows_browse(options).available && fake.loaded == fake.unloaded,
          "unexpected record encoding permits fallback rather than silently losing identities");
    fake = {}; fake.unrelated_service = true;
    check(detail::windows_browse(options).advertisements.empty() && fake.resolutions == 0,
          "unrelated service types are never resolved");
    fake = {}; fake.failed_resolution = true;
    result = detail::windows_browse(options);
    bool has_endpoint = false;
    for (const auto &ad : result.advertisements) has_endpoint |= !ad.endpoints.empty();
    check(!has_endpoint && fake.loaded == fake.unloaded,
          "failed resolution never invents an address or retains finished requests");
    fake = {}; fake.late_resolve = true;
    detail::windows_browse(options);
    check(fake.loaded > fake.unloaded && fake.resolution_replies.size() == 2,
          "pending resolutions outlive the bounded scan safely");
    for (auto &reply : fake.resolution_replies) reply();
    fake.resolution_replies.clear();
    check(fake.loaded == fake.unloaded && fake.instances_freed == 2,
          "late resolution results are freed after the scan stopped");
    fake = {}; fake.late_cancel = true;
    detail::windows_browse(options);
    check(fake.loaded > fake.unloaded, "pending cancellation retains its native library and callback state");
    for (auto &item : fake.browsers)
        item->request.pBrowseCallback(ERROR_CANCELLED, item->request.pQueryContext, nullptr);
    check(fake.loaded == fake.unloaded, "late cancellation releases the retained state");
    fake = {};
    options.should_cancel = [] { return true; };
    check(detail::windows_browse(options).cancelled && fake.loaded == 0, "pre-cancelled scan does not load DNS-SD");
    std::printf("%s: Windows DNS-SD discovery, scope and asynchronous cancellation\n",
                failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
