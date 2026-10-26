#include "wifi/DnsSd.h"

#include "util/Bytes.h"
#include "transport/SocketPlatform.h"
#include <mdns.h>

#include <array>

namespace xdb::wifi::mdns::detail {

std::vector<uint8_t> dns_name_data(std::string_view name) {
    if (name.empty() || name.size() > 1005 || name.back() != '.') return {};
    std::vector<uint8_t> result{0};
    size_t label = 0;
    for (size_t at = 0; at < name.size(); ++at) {
        unsigned char byte = name[at];
        if (byte == '.') {
            const auto length = result.size() - label - 1;
            if (length == 0 || length > 63) return {};
            result[label] = static_cast<uint8_t>(length);
            label = result.size();
            result.push_back(0);
        } else {
            if (byte == '\\') {
                if (++at == name.size()) return {};
                byte = name[at];
                if (byte >= '0' && byte <= '9') {
                    if (name.size() - at < 3) return {};
                    unsigned value = 0;
                    for (size_t i = 0; i < 3; ++i) {
                        const char digit = name[at + i];
                        if (digit < '0' || digit > '9') return {};
                        value = value * 10 + unsigned(digit - '0');
                    }
                    if (value > 255) return {};
                    byte = static_cast<unsigned char>(value);
                    at += 2;
                }
            } else if (byte == 0) return {};
            result.push_back(byte);
            if (result.size() - label - 1 > 63) return {};
        }
        if (result.size() > 255) return {};
    }
    return label == result.size() - 1 ? result : std::vector<uint8_t>{};
}

std::vector<uint8_t> record_packet(const ResourceRecord &record) {
    const auto name = dns_name_data(record.name);
    if (name.empty() || record.data.size() > 16384 - 12 - name.size() - 10) return {};
    std::vector<uint8_t> result(12, 0);
    util::write_be16(result.data() + 2, 0x8400);
    util::write_be16(result.data() + 6, 1);
    result.insert(result.end(), name.begin(), name.end());
    util::append_be16(result, record.type);
    util::append_be16(result, 1);
    util::append_be32(result, record.ttl);
    util::append_be16(result, static_cast<uint16_t>(record.data.size()));
    result.insert(result.end(), record.data.begin(), record.data.end());
    return result;
}

namespace {
std::string packet_name(const void *data, size_t size, size_t offset) {
    std::array<char, 1024> buffer{};
    const auto name = mdns_string_extract(data, size, &offset, buffer.data(), buffer.size());
    return name.length && name.length < buffer.size() ? std::string(name.str, name.length) : std::string{};
}

struct Records {
    std::vector<ResourceRecord> values;
    bool valid = true;
    static int read(int, const sockaddr *, size_t, mdns_entry_type_t, uint16_t,
                    uint16_t type, uint16_t, uint32_t ttl, const void *packet, size_t size,
                    size_t name_offset, size_t, size_t offset, size_t length, void *user) {
        auto &out = *static_cast<Records *>(user);
        ResourceRecord record{packet_name(packet, size, name_offset), type, {}, ttl};
        if (record.name.empty() || offset > size || length > size - offset) {
            out.valid = false;
            return 0;
        }
        const auto *bytes = static_cast<const uint8_t *>(packet) + offset;
        if (type == MDNS_RECORDTYPE_PTR || type == MDNS_RECORDTYPE_SRV) {
            const size_t prefix = type == MDNS_RECORDTYPE_SRV ? 6 : 0;
            if (length <= prefix) { out.valid = false; return 0; }
            const auto target = dns_name_data(packet_name(packet, size, offset + prefix));
            if (target.empty()) { out.valid = false; return 0; }
            record.data.assign(bytes, bytes + prefix);
            record.data.insert(record.data.end(), target.begin(), target.end());
        } else record.data.assign(bytes, bytes + length);
        out.values.push_back(std::move(record));
        return 0;
    }
};
} // namespace

std::vector<ResourceRecord> host_resource_records(const HostRecords &records, std::string &error) {
    const auto packet = host_announcement(records, 120, error);
    if (packet.empty()) return {};
    Records result;
    size_t offset = 12;
    mdns_records_parse(-1, nullptr, 0, packet.data(), packet.size(), &offset,
                       MDNS_ENTRYTYPE_ANSWER, 0, util::read_be16(packet.data() + 6), &Records::read, &result);
    if (!result.valid || result.values.empty()) {
        error = "Cannot expand pairing DNS-SD records";
        return {};
    }
    return result.values;
}

} // namespace xdb::wifi::mdns::detail
