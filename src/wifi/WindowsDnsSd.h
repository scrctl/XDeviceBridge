#pragma once

#include "wifi/Mdns.h"

namespace xdb::wifi::mdns::detail {

BrowseResult windows_browse(const BrowseOptions &options);

} // namespace xdb::wifi::mdns::detail
