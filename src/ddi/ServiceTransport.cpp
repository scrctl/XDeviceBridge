#include "ddi/ServiceTransport.h"
#include "transport/IoBudget.h"
#include "remote/Device.h"
#include "transport/Lockdown.h"
#include "transport/Usbmux.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace xdb::ddi {
namespace {
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
using V = plist::Value;
constexpr uint32_t kMaxFrame = static_cast<uint32_t>(plist::kMaxInput);
constexpr char kUsbService[] = "com.apple.mobile.mobile_image_mounter";
constexpr char kRemoteService[] = "com.apple.mobile.mobile_image_mounter.shim.remote";
constexpr Ms kHangupTimeout{1000};

Clock::time_point deadline(Ms timeout) {
    return Clock::now() + std::clamp(timeout, Ms(0), Ms(std::chrono::hours(1)));
}
Ms remaining(Clock::time_point until) {
    return std::max(Ms(0), std::chrono::duration_cast<Ms>(until - Clock::now()));
}
} // namespace

struct ServiceTransport::Impl {
    enum class Phase { Failed, Ready, Uploading, Finished };
    Phase phase = Phase::Failed;
    uint64_t upload_remaining = 0;
    transport::Lockdown *lockdown = nullptr;
    remote::Device *device = nullptr;
    uint32_t device_id = 0;
    std::function<bool()> cancelled;
    transport::Socket socket;
    transport::TlsChannel tls;
    std::unique_ptr<net::TcpStream> tcp;
    net::ByteStream *test_stream = nullptr;
    std::function<void()> test_interrupt;
    std::vector<uint8_t> pending;
    size_t offset = 0;
    ~Impl() { hangup(kHangupTimeout); socket.interrupt(); }

    // Closing TCP alone leaves some devices' image-mounter session busy for
    // about 30 seconds. Send Hangup only at a completed plist boundary, never
    // into a raw upload or after an interrupted/partially read operation.
    void hangup(Ms timeout) noexcept {
        if (phase != Phase::Ready) return;
        phase = Phase::Finished;
        try {
            transport::IoBudget budget({std::min(timeout, kHangupTimeout), cancelled});
            transport::BudgetWatchdog watch(budget, interrupter());
            if (!watch.active()) return;
            auto request = V::Dict(); request.set("Command", V::Str("Hangup"));
            std::string ignored;
            if (write_frame(request, ignored)) (void)read_frame(ignored);
            // Best effort: a missing/rejected reply must not replace the
            // result of the completed operation or escape this destructor.
        } catch (...) {}
    }

    void accept_reply(Phase previous, const V &reply, const V *request = nullptr) {
        if (previous == Phase::Failed || previous == Phase::Finished) return;
        const auto *command = request ? request->find("Command") : nullptr;
        const auto *status = reply.find("Status");
        if (command && command->is_string() && command->string == "Hangup") {
            phase = Phase::Finished;
        } else if (previous == Phase::Uploading) {
            if (!request && !upload_remaining && !reply.find("Error") && status &&
                status->is_string() && status->string == "Complete") phase = Phase::Ready;
        } else if (command && command->is_string() && command->string == "ReceiveBytes") {
            const auto *size = request->find("ImageSize");
            if (!reply.find("Error") && status && status->is_string() && status->string == "ReceiveBytesAck" &&
                size && ((size->is_int() && size->integer > 0) || (size->is_uint() && size->unsigned_integer > 0))) {
                upload_remaining = size->as_uint_or();
                phase = Phase::Uploading;
            }
        } else if (!status || !status->is_string() || status->string != "ReceiveBytesAck") {
            phase = Phase::Ready;
        }
    }

    std::function<void()> interrupter() {
        if (tcp) { auto *stream = tcp.get(); return [stream] { stream->close(); }; }
        if (test_stream) return test_interrupt;
        return [this] { socket.interrupt(); };
    }
    bool write(const uint8_t *bytes, size_t count, std::string &error) {
        auto *stream = tcp ? static_cast<net::ByteStream *>(tcp.get()) : test_stream;
        if (stream) return stream->send(std::string_view(reinterpret_cast<const char *>(bytes), count), error);
        auto *ssl = tls.handle();
        if (!ssl) return socket.write_all(bytes, count, error);
        size_t offset = 0;
        while (offset < count) {
            const auto length = static_cast<int>(std::min(count - offset, size_t(INT_MAX)));
            const auto written = SSL_write(ssl, bytes + offset, length);
            if (written > 0) { offset += static_cast<size_t>(written); continue; }
            const auto code = SSL_get_error(ssl, written);
            if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
                if (transport::wait_socket(socket.fd(), code == SSL_ERROR_WANT_WRITE, 25) >= 0) continue;
            }
            error = "Cannot write DDI service TLS data"; return false;
        }
        return true;
    }
    bool read(uint8_t *bytes, size_t count, std::string &error) {
        auto *stream = tcp ? static_cast<net::ByteStream *>(tcp.get()) : test_stream;
        if (stream) {
            size_t copied = 0;
            while (copied < count) {
                const auto available = pending.size() - offset;
                if (available) {
                    const auto n = std::min(available, count - copied);
                    std::memcpy(bytes + copied, pending.data() + offset, n); copied += n; offset += n;
                    if (offset == pending.size()) { pending.clear(); offset = 0; }
                    continue;
                }
                bool timed_out = false;
                if (!stream->recv(pending, 25, error, &timed_out)) {
                    if (timed_out) continue;
                    return false;
                }
                if (pending.empty()) { error = "DDI service closed the connection"; return false; }
            }
            return true;
        }
        auto *ssl = tls.handle();
        if (!ssl) return socket.read_exact(bytes, count, error);
        size_t copied = 0;
        while (copied < count) {
            const auto length = static_cast<int>(std::min(count - copied, size_t(INT_MAX)));
            const auto n = SSL_read(ssl, bytes + copied, length);
            if (n > 0) { copied += static_cast<size_t>(n); continue; }
            const auto code = SSL_get_error(ssl, n);
            if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
                if (transport::wait_socket(socket.fd(), code == SSL_ERROR_WANT_WRITE, 25) >= 0) continue;
            }
            error = "Cannot read DDI service TLS data"; return false;
        }
        return true;
    }
    bool write_frame(const V &request, std::string &error) {
        const auto body = plist::write(request);
        if (body.empty() || body.size() > kMaxFrame) { error = "DDI service request exceeds its frame limit"; return false; }
        const auto length = static_cast<uint32_t>(body.size());
        const uint8_t header[]{static_cast<uint8_t>(length >> 24), static_cast<uint8_t>(length >> 16),
            static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
        return write(header, sizeof(header), error) && write(reinterpret_cast<const uint8_t *>(body.data()), body.size(), error);
    }
    std::optional<V> read_frame(std::string &error) {
        uint8_t header[4]; if (!read(header, sizeof(header), error)) return {};
        const auto length = uint32_t(header[0]) << 24 | uint32_t(header[1]) << 16 | uint32_t(header[2]) << 8 | header[3];
        if (!length || length > kMaxFrame) { error = "DDI service response has an invalid frame length"; return {}; }
        std::string body(length, '\0'); if (!read(reinterpret_cast<uint8_t *>(body.data()), body.size(), error)) return {};
        auto result = plist::parse(body);
        if (!result || !result->is_dict()) { error = "DDI service response is not a plist dictionary"; return {}; }
        return result;
    }
    bool connect(Ms timeout, std::string &error) {
        error.clear(); const auto until = deadline(timeout);
        hangup(remaining(until));
        phase = Phase::Failed; upload_remaining = 0;
        // No watchdog is running while replacing ownership.
        tcp.reset(); socket.interrupt(); tls = transport::TlsChannel{}; socket.close(); pending.clear(); offset = 0;
        if (lockdown) {
            std::optional<transport::Lockdown::ServiceEndpoint> endpoint;
            {
                transport::IoBudget operation_budget({remaining(until), cancelled});
                transport::BudgetWatchdog watch(operation_budget, [this] { lockdown->interrupt(); });
                if (watch.active()) endpoint = lockdown->start_service(kUsbService, error);
                if (!watch.finish(error, "DDI service operation") || !endpoint || !endpoint->port) return false;
            }
            auto mux = transport::Usbmux::open_for_service(remaining(until), cancelled, error);
            if (!mux) return false;
            auto opened = mux->connect_for_service(device_id, endpoint->port, remaining(until), cancelled, error);
            if (!opened) return false;
            socket = std::move(*opened);
            if (endpoint->requires_tls) {
                transport::IoBudget operation_budget({remaining(until), cancelled});
                transport::BudgetWatchdog watch(operation_budget, [this] { socket.interrupt(); });
                const auto ok = watch.active() && tls.handshake(socket, lockdown->identity(), error);
                if (!watch.finish(error, "DDI service operation") || !ok) return false;
            }
            phase = Phase::Ready;
            return true;
        }
        if (!device || !device->stack()) { error = "DDI remote session is unavailable"; return false; }
        const auto endpoint = device->rsd().service(kRemoteService);
        if (!endpoint || !endpoint->port) { error = "DDI remote image-mounter service is unavailable"; return false; }
        if (endpoint->uses_remote_xpc || endpoint->encrypt_socket_data) {
            error = "DDI remote image-mounter service uses an unsupported transport"; return false;
        }
        tcp = std::make_unique<net::TcpStream>(*device->stack());
        bool ok = false;
        {
            transport::IoBudget operation_budget({remaining(until), cancelled});
            transport::BudgetWatchdog watch(operation_budget, interrupter());
            ok = watch.active() && tcp->connect(endpoint->port, error);
            if (!watch.finish(error, "DDI service operation") || !ok) return false;
        }
        transport::IoBudget operation_budget({remaining(until), cancelled});
        transport::BudgetWatchdog watch(operation_budget, interrupter());
        if (!watch.active()) return watch.finish(error, "DDI service operation");
        auto checkin = V::Dict(); checkin.set("Label", V::Str("xdb"));
        checkin.set("ProtocolVersion", V::Str("2")); checkin.set("Request", V::Str("RSDCheckin"));
        ok = write_frame(checkin, error);
        for (const auto *expected : {"RSDCheckin", "StartService"}) {
            auto response = ok ? read_frame(error) : std::optional<V>{};
            const auto *request = response ? response->find("Request") : nullptr;
            if (!response || response->find("Error") || !request || !request->is_string() || request->string != expected) {
                if (response) error = "DDI remote image-mounter check-in was rejected";
                ok = false; break;
            }
        }
        if (!watch.finish(error, "DDI service operation") || !ok) return false;
        phase = Phase::Ready;
        return true;
    }
};

ServiceTransport::ServiceTransport() : impl_(std::make_unique<Impl>()) {}
ServiceTransport::~ServiceTransport() = default;
std::unique_ptr<ServiceTransport> ServiceTransport::open_usb(transport::Lockdown &lockdown, uint32_t id,
        Ms timeout, const std::function<bool()> &cancelled, std::string &error) {
    try {
        auto adapter = std::unique_ptr<ServiceTransport>(new ServiceTransport);
        adapter->impl_->lockdown = &lockdown; adapter->impl_->device_id = id; adapter->impl_->cancelled = cancelled;
        if (!adapter->impl_->connect(timeout, error)) return {};
        return adapter;
    } catch (const std::exception &e) { error = e.what(); return {}; }
}
std::unique_ptr<ServiceTransport> ServiceTransport::open_remote(remote::Device &device, Ms timeout,
        const std::function<bool()> &cancelled, std::string &error) {
    try {
        auto adapter = std::unique_ptr<ServiceTransport>(new ServiceTransport);
        adapter->impl_->device = &device; adapter->impl_->cancelled = cancelled;
        if (!adapter->impl_->connect(timeout, error)) return {};
        return adapter;
    } catch (const std::exception &e) { error = e.what(); return {}; }
}
PlistService ServiceTransport::service() {
    PlistService service;
    service.request = [this](const V &request, Ms timeout, std::string &error) -> std::optional<V> {
        error.clear();
        const auto previous = impl_->phase; impl_->phase = Impl::Phase::Failed;
        try {
            transport::IoBudget operation_budget({timeout, impl_->cancelled});
            transport::BudgetWatchdog watch(operation_budget, impl_->interrupter());
            auto result = watch.active() && impl_->write_frame(request, error) ? impl_->read_frame(error) : std::optional<V>{};
            if (!watch.finish(error, "DDI service operation")) return {};
            if (result) impl_->accept_reply(previous, *result, &request);
            return result;
        } catch (const std::exception &e) { error = e.what(); return {}; }
    };
    service.receive = [this](Ms timeout, std::string &error) -> std::optional<V> {
        error.clear();
        const auto previous = impl_->phase; impl_->phase = Impl::Phase::Failed;
        try {
            transport::IoBudget operation_budget({timeout, impl_->cancelled});
            transport::BudgetWatchdog watch(operation_budget, impl_->interrupter());
            auto result = watch.active() ? impl_->read_frame(error) : std::optional<V>{};
            if (!watch.finish(error, "DDI service operation")) return {};
            if (result) impl_->accept_reply(previous, *result);
            return result;
        } catch (const std::exception &e) { error = e.what(); return {}; }
    };
    service.send_bytes = [this](const uint8_t *data, size_t size, Ms timeout, std::string &error) {
        error.clear();
        const auto previous = impl_->phase; impl_->phase = Impl::Phase::Failed;
        try {
            if (!data && size) { error = "DDI service raw upload data is missing"; return false; }
            transport::IoBudget operation_budget({timeout, impl_->cancelled});
            transport::BudgetWatchdog watch(operation_budget, impl_->interrupter());
            const auto ok = watch.active() && impl_->write(data, size, error);
            if (!watch.finish(error, "DDI service operation") || !ok) return false;
            if (previous == Impl::Phase::Uploading && size <= impl_->upload_remaining) {
                impl_->upload_remaining -= size;
                impl_->phase = Impl::Phase::Uploading;
            }
            return true;
        } catch (const std::exception &e) { error = e.what(); return false; }
    };
    service.reconnect = [this](Ms timeout, std::string &error) {
        try { return impl_->connect(timeout, error); }
        catch (const std::exception &e) { error = e.what(); return false; }
    };
    return service;
}
std::unique_ptr<ServiceTransport> detail::service_transport_with_socket(
        transport::Socket socket, const std::function<bool()> &cancelled) {
    auto adapter = std::unique_ptr<ServiceTransport>(new ServiceTransport);
    adapter->impl_->socket = std::move(socket); adapter->impl_->cancelled = cancelled;
    adapter->impl_->phase = ServiceTransport::Impl::Phase::Ready;
    return adapter;
}
std::unique_ptr<ServiceTransport> detail::service_transport_with_byte_stream(
        net::ByteStream &stream, const std::function<void()> &interrupt, const std::function<bool()> &cancelled) {
    auto adapter = std::unique_ptr<ServiceTransport>(new ServiceTransport);
    adapter->impl_->test_stream = &stream; adapter->impl_->test_interrupt = interrupt; adapter->impl_->cancelled = cancelled;
    adapter->impl_->phase = ServiceTransport::Impl::Phase::Ready;
    return adapter;
}
} // namespace xdb::ddi
