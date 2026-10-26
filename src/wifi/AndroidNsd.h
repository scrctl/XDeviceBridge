// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include "wifi/Mdns.h"

namespace xdb::wifi::mdns::detail {
// Android's service-info callbacks own expiry/removal. Each immutable snapshot
// replaces the preceding one; never append it to a cache from another network.
BrowseResult android_nsd_snapshot(std::string_view json);
#ifdef __ANDROID__
BrowseResult android_browse(const BrowseOptions &options);
#endif
} // namespace xdb::wifi::mdns::detail
