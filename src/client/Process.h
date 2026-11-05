// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include <cstdint>
#include <string>

namespace xdb::transport { class IoBudget; }
namespace xdb::client {
bool launch_server(uint16_t port, const char *directory, std::string &error, const transport::IoBudget *budget = nullptr);
} // namespace xdb::client
