#pragma once

#include "wifi/Mdns.h"
#include <stop_token>

namespace xdb::wifi::mdns::detail {

using DiscoveryPublish = std::function<void(BrowseResult)>;
using DiscoveryRun = std::function<void(std::stop_token, const DiscoveryPublish &)>;

// A single event-driven native browser, shared by desktop servers in this process.
// The source owns native callbacks; readers only copy a synchronized snapshot.
class DiscoverySubscription {
  public:
    explicit DiscoverySubscription(DiscoveryRun source);
    ~DiscoverySubscription();
    DiscoverySubscription(const DiscoverySubscription &) = delete;
    DiscoverySubscription &operator=(const DiscoverySubscription &) = delete;
    BrowseResult read(const BrowseOptions &options);
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::shared_ptr<DiscoverySubscription> acquire_discovery_subscription();
std::optional<BrowseResult> subscribed_browse(const BrowseOptions &options);

} // namespace xdb::wifi::mdns::detail
