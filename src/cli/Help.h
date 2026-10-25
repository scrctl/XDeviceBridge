// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <span>
#include <string>

namespace xdb::cli {
void print_help(std::span<const std::string> command);
}
