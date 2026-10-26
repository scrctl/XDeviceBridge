#pragma once

#include "wifi/Mdns.h"

namespace xdb::wifi::mdns::detail {

// Only linked on macOS. No native DNS-SD dependency is exposed to SDK clients.
BrowseResult bonjour_browse(const BrowseOptions &options);

class BonjourAdvertiser {
  public:
    static std::unique_ptr<BonjourAdvertiser> start(const HostAdvertisement &host, std::string &error);
    ~BonjourAdvertiser();
    bool poll(int timeout_ms, std::string &error);
  private:
    struct Impl;
    explicit BonjourAdvertiser(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace xdb::wifi::mdns::detail
