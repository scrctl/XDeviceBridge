#pragma once

#include "wifi/Mdns.h"

namespace xdb::wifi::mdns::detail {

BrowseResult avahi_browse(const BrowseOptions &options);

class AvahiAdvertiser {
  public:
    static std::unique_ptr<AvahiAdvertiser> start(const HostAdvertisement &host, std::string &error);
    ~AvahiAdvertiser();
    bool poll(int timeout_ms, std::string &error);
  private:
    struct Impl;
    explicit AvahiAdvertiser(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace xdb::wifi::mdns::detail
