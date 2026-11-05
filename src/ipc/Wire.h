// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include "transport/Socket.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace xdb::transport { class IoBudget; }
namespace xdb::ipc {
inline constexpr uint32_t protocol_version = 1;
inline constexpr std::string_view frame_magic = "XDB1";
inline constexpr size_t max_json = 1024 * 1024;
inline constexpr size_t max_binary = 64 * 1024 * 1024;
struct Message {
    std::string json;
    std::vector<uint8_t> binary;
};
bool read(transport::Socket &socket, Message &out, std::string &error);
bool write(transport::Socket &socket, const Message &message, std::string &error);
transport::Socket connect(uint16_t port, std::string &error, bool *not_running = nullptr, const transport::IoBudget *budget = nullptr);
} // namespace xdb::ipc
