// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include "remote/Discovery.h"
#include <nlohmann/json.hpp>
#include <set>
#include <algorithm>
#include <iterator>

namespace xdb::server {
inline const char *pairing_state(remote::DiscoveryPairing value) {
    using remote::DiscoveryPairing;
    switch (value) {
    case DiscoveryPairing::ready: return "record_present";
    case DiscoveryPairing::needs_pairing: return "incomplete_record";
    case DiscoveryPairing::identifier_hint: return "identifier_hint";
    case DiscoveryPairing::ambiguous: return "ambiguous";
    case DiscoveryPairing::invalid_advertisement: return "invalid_advertisement";
    case DiscoveryPairing::crypto_error: return "crypto_error";
    case DiscoveryPairing::unmatched: return "unknown";
    }
    return "unknown";
}
inline nlohmann::json discovery_json(const remote::DiscoveryResult &value,
                                     const std::set<std::string> &connected) {
    using Json = nlohmann::json;
    Json devices = Json::array();
    for (const auto &device : value.devices) {
        Json candidates = Json::array();
        bool paired = false;
        for (const auto &c : device.candidates) {
            const bool record_present = c.pairing == remote::DiscoveryPairing::ready;
            paired |= record_present;
            candidates.push_back(
                {{"transport", c.transport == remote::DiscoveryTransport::usbmux ? "usb" : "wifi"},
                 {"address", c.address}, {"port", c.port}, {"interface", c.interface_name},
                 {"paired", record_present}, {"pairing", pairing_state(c.pairing)},
                 {"connection_type", c.connection_type}});
        }
        const bool active = connected.contains(device.udid);
        devices.push_back({{"udid", device.udid}, {"name", device.name}, {"candidates", candidates},
                           {"state", active ? "connected" : paired ? "paired" : "discovered"},
                           {"pairing", paired ? "record_present" : "unknown"},
                           {"connection", active ? "connected" : "unverified"}});
    }
    return {{"devices", devices}, {"warnings", value.warnings}, {"cancelled", value.cancelled}};
}
// A live authenticated endpoint is useful even without a multicast advertisement.
inline void merge_manual_connection(nlohmann::json &result, const std::string &udid, const std::string &name,
                                    const std::string &address, uint16_t port) {
    using Json = nlohmann::json;
    auto &devices = result["devices"];
    auto device = std::find_if(devices.begin(), devices.end(), [&](const auto &d) {
        return d.value("udid", std::string()) == udid;
    });
    if (device == devices.end()) {
        devices.push_back({{"udid", udid}, {"name", name}, {"candidates", Json::array()}});
        device = std::prev(devices.end());
    }
    (*device)["state"] = "connected";
    (*device)["connection"] = "connected";
    (*device)["pairing"] = "record_present";
    (*device)["manual_connection"] = {{"address", address}, {"port", port}};
    auto &candidates = (*device)["candidates"];
    if (std::none_of(candidates.begin(), candidates.end(), [&](const auto &c) {
            return c.value("transport", std::string()) == "wifi" &&
                   c.value("address", std::string()) == address && c.value("port", 0) == port;
        })) candidates.push_back({{"transport", "wifi"}, {"address", address}, {"port", port},
                                   {"interface", ""}, {"paired", true}, {"pairing", "record_present"},
                                   {"connection_type", "RemotePairing"}});
}
} // namespace xdb::server
