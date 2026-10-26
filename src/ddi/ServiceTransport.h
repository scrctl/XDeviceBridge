#pragma once
#include "ddi/Mounter.h"
#include <memory>

namespace xdb::transport { class Lockdown; class Socket; }
namespace xdb::remote { class Device; }
namespace xdb::net { class ByteStream; }

namespace xdb::ddi {
class ServiceTransport;
namespace detail {
// Offline byte-protocol seam; production always opens USB/RSD endpoints.
std::unique_ptr<ServiceTransport> service_transport_with_socket(
    transport::Socket, const std::function<bool()> &cancelled);
std::unique_ptr<ServiceTransport> service_transport_with_byte_stream(
    net::ByteStream &, const std::function<void()> &interrupt, const std::function<bool()> &cancelled);
}
// Owns only the independent image-mounter connection. Borrowed Lockdown/Device
// and this adapter must outlive callbacks returned by service(). Operations are
// serialized by the caller; every operation uses one remaining deadline.
// Destruction/reconnect best-effort Hangup at an idle plist boundary (<= 1 s,
// cancellable); failed operations and incomplete raw uploads close directly.
class ServiceTransport {
public:
    ~ServiceTransport();
    ServiceTransport(const ServiceTransport &) = delete;
    ServiceTransport &operator=(const ServiceTransport &) = delete;
    static std::unique_ptr<ServiceTransport> open_usb(
        transport::Lockdown &, uint32_t device_id, std::chrono::milliseconds timeout,
        const std::function<bool()> &cancelled, std::string &error);
    static std::unique_ptr<ServiceTransport> open_remote(
        remote::Device &, std::chrono::milliseconds timeout,
        const std::function<bool()> &cancelled, std::string &error);
    PlistService service();
private:
    friend std::unique_ptr<ServiceTransport> detail::service_transport_with_socket(
        transport::Socket, const std::function<bool()> &);
    friend std::unique_ptr<ServiceTransport> detail::service_transport_with_byte_stream(
        net::ByteStream &, const std::function<void()> &, const std::function<bool()> &);
    ServiceTransport();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xdb::ddi
