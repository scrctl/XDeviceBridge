#include "wifi/DnsSd.h"
#include "util/Bytes.h"

#include <algorithm>
#include <cstdio>

namespace {
using namespace xdb::wifi::mdns;
int failures = 0;
void check(bool ok, const char *label) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}
} // namespace

int main() {
    using namespace detail;
    check(dns_name_data("test.local.") == std::vector<uint8_t>({4,'t','e','s','t',5,'l','o','c','a','l',0}),
          "DNS names use independent uncompressed labels");
    check(dns_name_data("a\\032b.local.")[0] == 3 && dns_name_data("a\\.b.local.")[0] == 3,
          "DNS-SD decimal and literal escapes preserve a single label");
    for (const auto &name : {"", "test.local", "a..local.", "a\\999.local.", "a\\1.local.", "a\\x"})
        check(dns_name_data(name).empty(), "invalid DNS-SD names are rejected");
    check(dns_name_data(std::string(64, 'a') + ".local.").empty(), "label length is bounded");
    check(record_packet({"test.local.", 16, std::vector<uint8_t>(16384), 120}).empty(),
          "native record bridge retains the DNS packet budget");

    const auto now = RecordCache::Clock::now();
    RecordCache cache;
    auto add = [&](const ResourceRecord &record) {
        return cache.add_packet(4, "en4", record_packet(record), now);
    };
    const std::string service = "phone._remotepairing._tcp.local.";
    check(add({"_remotepairing._tcp.local.", 12, dns_name_data(service), 120}), "PTR bridge");
    auto srv = std::vector<uint8_t>(6, 0);
    xdb::util::write_be16(srv.data() + 4, 49152);
    const auto target = dns_name_data("phone.local.");
    srv.insert(srv.end(), target.begin(), target.end());
    check(add({service, 33, srv, 120}), "SRV bridge");
    std::vector<uint8_t> txt;
    for (const auto &field : {std::string("identifier=peer"), std::string("authTag=\0\xff", 10)}) {
        txt.push_back(static_cast<uint8_t>(field.size()));
        txt.insert(txt.end(), field.begin(), field.end());
    }
    check(add({service, 16, txt, 120}), "TXT bridge");
    std::vector<uint8_t> ipv6(16, 0);
    ipv6[0] = 0xfe; ipv6[1] = 0x80; ipv6[15] = 7;
    check(add({"phone.local.", 28, ipv6, 120}), "AAAA bridge");
    const auto snapshot = cache.snapshot(now);
    check(snapshot.size() == 1 && snapshot[0].identifier == "peer" &&
          snapshot[0].auth_tag == std::string("\0\xff", 2) && snapshot[0].endpoints.size() == 1 &&
          snapshot[0].endpoints[0].address == "fe80::7%4" && snapshot[0].endpoints[0].port == 49152,
          "native records retain binary TXT, real port and link-local interface scope");
    check(cache.snapshot(now + std::chrono::seconds(121)).empty(), "native TTLs expire normally");
    check(add({"_remotepairing._tcp.local.", 12, dns_name_data(service), 0}) && cache.snapshot(now).empty(),
          "a native remove callback withdraws its service");
    check(!add({service, 16, {5, 'a'}, 120}), "malformed native TXT is rejected before parsing");

    std::string error;
    HostRecords host{{"unit-host", "xdb - unit", "xdb - Mac", "AQIDBAUG", 49152},
        "unit-host._remotepairing-pairable-host._tcp.local.", "unit-host.local.",
        {"192.0.2.1", "fe80::1"}};
    const auto records = host_resource_records(host, error);
    check(error.empty() && records.size() == 5, "pairing native records come from the existing encoder");
    const auto ptr = std::find_if(records.begin(), records.end(), [](const auto &r) { return r.type == 12; });
    const auto sr = std::find_if(records.begin(), records.end(), [](const auto &r) { return r.type == 33; });
    check(ptr != records.end() && ptr->name == "_remotepairing-pairable-host._tcp.local." &&
          ptr->data == dns_name_data(host.service_instance), "pairing PTR target is unchanged and expanded");
    check(sr != records.end() && sr->name == host.service_instance && sr->data.size() > 6 &&
          xdb::util::read_be16(sr->data.data() + 4) == host.host.port &&
          std::vector<uint8_t>(sr->data.begin() + 6, sr->data.end()) == dns_name_data(host.target),
          "pairing SRV preserves the listener port and UUID hostname");
    host.target = "different.local.";
    check(host_resource_records(host, error).empty() && !error.empty(), "native publishing preserves host identity checks");
    if (!failures) std::puts("PASS: native DNS-SD record conversion, limits, identity and TTL withdrawal");
    return failures ? 1 : 0;
}
