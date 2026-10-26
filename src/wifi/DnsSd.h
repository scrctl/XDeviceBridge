#pragma once

#include "wifi/Mdns.h"

namespace xdb::wifi::mdns::detail {

// Uncompressed DNS-SD records, shared by native providers and the packet cache.
struct ResourceRecord {
    std::string name;
    uint16_t type = 0;
    std::vector<uint8_t> data;
    uint32_t ttl = 0;
};

std::vector<uint8_t> dns_name_data(std::string_view name);
std::vector<uint8_t> record_packet(const ResourceRecord &record);
std::vector<ResourceRecord> host_resource_records(const HostRecords &records, std::string &error);
std::vector<std::pair<uint32_t, HostRecords>> local_host_records(
    const HostAdvertisement &host, std::string &error);

} // namespace xdb::wifi::mdns::detail
