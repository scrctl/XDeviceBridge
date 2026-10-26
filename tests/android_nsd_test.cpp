// SPDX-License-Identifier: LGPL-3.0-only
#include "wifi/AndroidNsd.h"
#include "util/Base64.h"
#include <nlohmann/json.hpp>
#include <net/if.h>
#include <cstdio>
using namespace xdb::wifi::mdns;
using nlohmann::json;
int main() {
    unsigned index = if_nametoindex("lo");
    const char *iface = "lo";
    if (!index) { iface = "lo0"; index = if_nametoindex(iface); }
    int failures = 0;
    auto check = [&](bool ok, const char *why) { if (!ok) { ++failures; std::printf("FAIL: %s\n", why); } };
    check(index > 0, "local interface exists");
    json entry = {{"name", "phone.with.dot"}, {"index", index}, {"interface", iface}, {"port", 49152},
        {"addresses", {"192.0.2.7", "fe80::1234", "::1", "224.0.0.1"}},
        {"txt", {{"identifier", xdb::util::base64_encode("peer")},
                 {"authTag", xdb::util::base64_encode(std::string("a\0b", 3))}}}};
    json root = {{"available", true}, {"services", {entry}}};
    auto read = [&] { return detail::android_nsd_snapshot(root.dump()); };
    auto first = read();
    check(first.available && first.advertisements.size() == 1, "resolved native record");
    if (first.advertisements.size() == 1) {
        const auto &ad = first.advertisements[0];
        check(ad.identifier == "peer" && ad.auth_tag == std::string("a\0b", 3), "binary TXT preserved");
        check(ad.instance.starts_with("phone\\046with\\046dot."), "decoded instance label escaped");
        check(ad.endpoints.size() == 2, "unsafe addresses filtered");
        check(ad.endpoints.back().address == "fe80::1234%" + std::to_string(index), "IPv6 local scope");
    }
    root["services"][0]["addresses"] = {"192.0.2.8"};
    root["services"][0]["txt"]["identifier"] = xdb::util::base64_encode("changed");
    auto updated = read();
    check(updated.advertisements[0].endpoints.size() == 1 &&
          updated.advertisements[0].endpoints[0].address == "192.0.2.8" &&
          updated.advertisements[0].identifier == "changed", "updates replace addresses and TXT");
    root["services"] = json::array();
    check(read().advertisements.empty(), "loss removes cached peer");
    root["services"] = {entry}; root["services"][0]["interface"] = "wrong-interface";
    check(read().advertisements.empty(), "interface index/name must agree");
    root["services"] = {entry}; root["services"][0]["port"] = 65536;
    check(read().advertisements.empty(), "invalid SRV port rejected");
    root["services"] = {entry}; root["services"][0]["txt"]["bad"] = "???";
    check(read().advertisements.empty(), "invalid TXT base64 rejected");
    root["services"] = {entry}; root["services"][0]["addresses"] = {"fe80::1234%999"};
    check(read().advertisements[0].endpoints.empty(), "foreign zone is not trusted");
    root["available"] = false;
    check(!read().available, "provider failure allows fallback");
    check(!detail::android_nsd_snapshot("{}").available, "malformed snapshot allows fallback");
    root["services"] = json::array(); for (int n=0; n<257; ++n) root["services"].push_back(entry);
    check(!read().available, "bounded service count");
    return failures ? 1 : 0;
}
