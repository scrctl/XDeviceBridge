#pragma once

#include "wifi/Mdns.h"
#include "wifi/DiscoverySubscription.h"

namespace xdb::wifi::mdns::detail {

BrowseResult windows_browse(const BrowseOptions &options);
void windows_subscribe(std::stop_token stop, const DiscoveryPublish &publish);

} // namespace xdb::wifi::mdns::detail
