// SPDX-License-Identifier: LGPL-3.0-only
#include "wifi/AndroidNsd.h"
#include "wifi/DnsSd.h"
#include "ipc/Json.h"
#include "util/Base64.h"
#include "util/Bytes.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <array>
#include <set>

namespace xdb::wifi::mdns::detail {
namespace {
constexpr std::string_view service = "_remotepairing._tcp.local.";
// NsdServiceInfo has decoded the instance label. Escape it without allowing a
// dot/backslash in a remote name to inject additional DNS labels.
std::string instance_name(std::string_view label) {
    if (label.empty() || label.size() > 63) return {};
    std::string out;
    for (unsigned char ch : label) {
        if (ch < 33 || ch > 126 || ch == '.' || ch == '\\') {
            out += '\\'; out += char('0' + ch / 100);
            out += char('0' + ch / 10 % 10); out += char('0' + ch % 10);
        } else out += char(ch);
    }
    return out + '.' + std::string(service);
}
} // namespace

BrowseResult android_nsd_snapshot(std::string_view text) {
    BrowseResult result;
    try {
        const auto root = ipc::parse_json(text);
        result.available = root.at("available").get<bool>();
        if (root.contains("warning")) {
            auto warning = root.at("warning").get<std::string>();
            if (!warning.empty()) result.warnings.push_back(warning.substr(0, 256));
        }
        const auto &services = root.at("services");
        if (!services.is_array() || services.size() > 256) throw std::invalid_argument("service limit");
        RecordCache cache;
        size_t ordinal = 0;
        for (const auto &entry : services) {
            // A malformed callback entry cannot erase valid peers in this snapshot.
            try {
                const auto instance = instance_name(entry.at("name").get<std::string>());
                if (!entry.at("index").is_number_integer() || !entry.at("port").is_number_integer()) continue;
                const auto index = entry.at("index").get<int64_t>();
                const auto port = entry.at("port").get<int64_t>();
                const auto iface = entry.at("interface").get<std::string>();
                std::array<char, IF_NAMESIZE> actual{};
                if (instance.empty() || index <= 0 || index > UINT32_MAX || port <= 0 || port > 65535 ||
                    !if_indextoname(static_cast<unsigned>(index), actual.data()) || iface != actual.data()) continue;
                // The system API does not expose SRV target on API 34/35. This
                // synthetic owner only associates addresses inside this snapshot;
                // it is never advertised and is not treated as a peer identity.
                const auto target = "nsd-" + std::to_string(++ordinal) + ".local.";
                const auto &fields = entry.at("txt");
                const auto &addresses = entry.at("addresses");
                if (!fields.is_object() || fields.size() > 128 || !addresses.is_array() || addresses.size() > 32) continue;
                std::vector<uint8_t> txt;
                std::set<std::string> keys;
                for (const auto &[key, value] : fields.items()) {
                    std::string lower = key;
                    for (char &ch : lower) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
                    if (key.empty() || !keys.insert(lower).second) throw std::invalid_argument("duplicate TXT key");
                    for (unsigned char ch : key) if (ch < 33 || ch > 126 || ch == '=') throw std::invalid_argument("TXT key");
                    std::string error;
                    auto bytes = util::base64_decode(value.get<std::string>(), error);
                    if (!bytes || key.size() + 1 + bytes->size() > 255) throw std::invalid_argument("TXT size");
                    txt.push_back(static_cast<uint8_t>(key.size() + 1 + bytes->size()));
                    txt.insert(txt.end(), key.begin(), key.end()); txt.push_back('=');
                    txt.insert(txt.end(), bytes->begin(), bytes->end());
                    if (txt.size() > 4096) throw std::invalid_argument("TXT budget");
                }
                std::vector<ResourceRecord> records;
                records.push_back({std::string(service), 12, dns_name_data(instance), 120});
                std::vector<uint8_t> srv(6, 0);
                util::write_be16(srv.data() + 4, static_cast<uint16_t>(port));
                const auto owner = dns_name_data(target); srv.insert(srv.end(), owner.begin(), owner.end());
                records.push_back({instance, 33, std::move(srv), 120});
                records.push_back({instance, 16, std::move(txt), 120});
                for (const auto &address : addresses) {
                    auto value = address.get<std::string>();
                    // Scope is supplied by the local Network/interface, never a
                    // remotely supplied zone identifier. The packet cache adds it.
                    if (value.size() > INET6_ADDRSTRLEN || value.find('%') != std::string::npos) continue;
                    std::array<uint8_t, 16> raw{};
                    if (inet_pton(AF_INET, value.c_str(), raw.data()) == 1)
                        records.push_back({target, 1, {raw.begin(), raw.begin() + 4}, 120});
                    else if (inet_pton(AF_INET6, value.c_str(), raw.data()) == 1)
                        records.push_back({target, 28, {raw.begin(), raw.end()}, 120});
                }
                for (const auto &record : records) {
                    auto packet = record_packet(record);
                    cache.add_packet(static_cast<uint32_t>(index), iface, packet);
                }
            } catch (const std::exception &) {
                if (result.warnings.empty()) result.warnings.push_back("Android NSD ignored an invalid service");
            }
        }
        result.advertisements = cache.snapshot();
        for (const auto &warning : cache.warnings()) result.warnings.push_back(warning);
    } catch (const std::exception &) {
        result.available = false;
        result.advertisements.clear();
        result.warnings = {"Android NSD returned an invalid snapshot"};
    }
    return result;
}
} // namespace xdb::wifi::mdns::detail
