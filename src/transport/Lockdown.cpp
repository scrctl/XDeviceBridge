#include "Lockdown.h"

#include <cstring>
#include <climits>
#include <algorithm>

namespace xdb::transport {
namespace {

constexpr uint16_t kLockdownPort = 62078;
constexpr const char *kLabel = "xdb";

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

bool tls_exact(SSL *ssl, uint8_t *data, size_t length, bool writing) {
    size_t offset = 0;
    while (offset < length) {
        const auto count = static_cast<int>(std::min(length - offset, size_t(INT_MAX)));
        const auto n = writing ? SSL_write(ssl, data + offset, count) : SSL_read(ssl, data + offset, count);
        if (n > 0) { offset += static_cast<size_t>(n); continue; }
        const auto code = SSL_get_error(ssl, n);
        if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) continue;
        return false;
    }
    return true;
}

bool write_frame(SSL *ssl, Socket &sock, std::string_view body, std::string &err) {
    uint8_t hdr[4];
    const uint32_t n = static_cast<uint32_t>(body.size());
    hdr[0] = static_cast<uint8_t>(n >> 24);
    hdr[1] = static_cast<uint8_t>(n >> 16);
    hdr[2] = static_cast<uint8_t>(n >> 8);
    hdr[3] = static_cast<uint8_t>(n);
    if (ssl != nullptr) {
        if (!tls_exact(ssl, hdr, 4, true)) {
            return err = "Failed to write TLS header", false;
        }
        if (!tls_exact(ssl, reinterpret_cast<uint8_t *>(const_cast<char *>(body.data())), body.size(), true)) {
            return err = "Failed to write TLS body", false;
        }
        return true;
    }
    return sock.write_len_prefixed_be(body, err);
}

bool read_frame(SSL *ssl, Socket &sock, std::vector<uint8_t> &out, std::string &err) {
    uint8_t hdr[4];
    if (ssl != nullptr) {
        if (!tls_exact(ssl, hdr, 4, false)) {
            return err = "Failed to read TLS header", false;
        }
    } else if (!sock.read_exact(hdr, 4, err)) {
        return false;
    }
    const uint32_t len =
        uint32_t(hdr[0]) << 24 | uint32_t(hdr[1]) << 16 | uint32_t(hdr[2]) << 8 | hdr[3];
    if (len < 1 || len > (32u << 20)) {
        return err = "Invalid lockdown frame length: " + std::to_string(len), false;
    }
    out.resize(len);
    if (ssl == nullptr) {
        return sock.read_exact(out.data(), len, err);
    }
    size_t got = 0;
    while (got < len) {
        const int n = SSL_read(ssl, out.data() + got, static_cast<int>(len - got));
        if (n <= 0) {
            return err = "Failed to read TLS body", false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

Lockdown::Lockdown() = default;
Lockdown::~Lockdown() { if (bounded_io_) sock_.interrupt(); }
Lockdown::Lockdown(Lockdown &&) noexcept = default;
Lockdown &Lockdown::operator=(Lockdown &&) noexcept = default;
void Lockdown::interrupt() { sock_.interrupt(); }

bool Lockdown::request(const plist::Value &req, plist::Value &reply, std::string &err) {
    plist::Value full = req;
    if (full.find("Label") == nullptr) {
        full.set("Label", plist::Value::Str(kLabel));
    }
    SSL *ssl = tls_.handle();
    if (!write_frame(ssl, sock_, plist::write(full), err)) {
        return false;
    }
    std::vector<uint8_t> frame;
    if (!read_frame(ssl, sock_, frame, err)) {
        return false;
    }
    auto parsed = plist::parse(
        std::string_view(reinterpret_cast<const char *>(frame.data()), frame.size()));
    if (!parsed) {
        return err = "lockdown response is not a valid plist", false;
    }
    reply = std::move(*parsed);
    return true;
}

bool Lockdown::request_bounded(const plist::Value &req, plist::Value &reply, Ms timeout,
        const std::function<bool()> &cancelled, std::string &err) {
    err.clear();
    IoBudget operation_budget({timeout, cancelled});
    BudgetWatchdog watch(operation_budget, [this] { sock_.interrupt(); });
    const auto ok = watch.active() && request(req, reply, err);
    return watch.finish(err, "Bounded lockdown operation") && ok;
}

std::optional<Lockdown> Lockdown::establish_bounded(uint32_t device_id, std::string_view udid,
        Ms timeout, const std::function<bool()> &cancelled, std::string &err) {
    IoBudget budget(StartupOptions{timeout, cancelled});
    return establish_bounded(device_id, udid, budget, err);
}

std::optional<Lockdown> Lockdown::establish_bounded(uint32_t device_id, std::string_view udid,
        const IoBudget &budget, std::string &err, DeviceError *failure) {
    err.clear();
    if (failure) *failure = DeviceError::other;
    if (!budget.check(err)) return {};
    auto cancelled = [&budget] { return budget.stopped(); };
    auto rec_mux = Usbmux::open_for_service(budget.remaining(), cancelled, err);
    if (!rec_mux) { budget.check(err); return {}; }
    std::vector<uint8_t> record;
    if (!rec_mux->read_pair_record_for_service(udid, record, budget.remaining(), cancelled, err)) { budget.check(err); return {}; }
    auto parsed = plist::parse(std::string_view(reinterpret_cast<const char *>(record.data()), record.size()));
    if (!parsed || !parsed->is_dict()) { err = "Cannot parse pairing record"; return {}; }
    auto take = [&](std::string_view key) {
        const auto *value = parsed->find(key);
        return value && value->kind == plist::Kind::Data ? value->data : std::vector<uint8_t>{};
    };
    Lockdown ld;
    ld.bounded_io_ = true;
    if (const auto *value = parsed->find("HostID")) ld.host_id_ = value->as_string_or();
    if (const auto *value = parsed->find("SystemBUID")) ld.system_buid_ = value->as_string_or();
    ld.identity_ = PemIdentity{take("HostCertificate"), take("HostPrivateKey"), take("RootCertificate")};
    if (ld.host_id_.empty() || !ld.identity_.complete()) { err = "Pairing record missing required fields"; return {}; }
    auto mux = Usbmux::open_for_service(budget.remaining(), cancelled, err);
    if (!mux) { budget.check(err); return {}; }
    auto socket = mux->connect_for_service(device_id, kLockdownPort, budget.remaining(), cancelled, err);
    if (!socket) { budget.check(err); return {}; }
    ld.sock_ = std::move(*socket);
    auto start = plist::Value::Dict(); start.set("Request", plist::Value::Str("StartSession"));
    start.set("HostID", plist::Value::Str(ld.host_id_)); start.set("SystemBUID", plist::Value::Str(ld.system_buid_));
    plist::Value reply;
    {
        SocketBudgetGuard guard(ld.sock_, budget);
        const bool ok = budget.check(err) && ld.request(start, reply, err);
        if (!guard.finish(err) || !ok) return {};
    }
    if (const auto *value = reply.find("Error")) {
        err = lockdown_rejection("StartSession", value->as_string_or("?"), failure); return {};
    }
    if (const auto *value = reply.find("SessionID")) ld.session_id_ = value->as_string_or();
    const auto *want_ssl = reply.find("EnableSessionSSL");
    if (!want_ssl || want_ssl->as_bool_or(true)) {
        if (!ld.tls_.handshake_bounded(ld.sock_, ld.identity_, budget, err)) return {};
    }
    // Every scoped watcher above has joined before ld is moved into optional.
    if (!budget.check(err)) return {};
    return ld;
}

std::optional<Lockdown> Lockdown::establish(uint32_t device_id, std::string_view udid,
                                            std::string &err) {
    // 配对记录必须经 usbmuxd 读：/var/db/lockdown 是 root-only。
    // 且必须在 Connect 之前用另一条 mux 连接。
    auto rec_mux = Usbmux::open(err);
    if (!rec_mux) {
        return std::nullopt;
    }
    std::vector<uint8_t> record;
    if (!rec_mux->read_pair_record(udid, record, err)) {
        return std::nullopt;
    }
    auto parsed = plist::parse(
        std::string_view(reinterpret_cast<const char *>(record.data()), record.size()));
    if (!parsed) {
        err = "Cannot parse pairing record";
        return std::nullopt;
    }
    auto take = [&](std::string_view key) -> std::vector<uint8_t> {
        const auto *v = parsed->find(key);
        return v != nullptr ? v->data : std::vector<uint8_t>{};
    };

    Lockdown ld;
    if (const auto *h = parsed->find("HostID")) {
        ld.host_id_ = h->as_string_or("");
    }
    if (const auto *b = parsed->find("SystemBUID")) {
        ld.system_buid_ = b->as_string_or("");
    }
    ld.identity_ = PemIdentity{take("HostCertificate"), take("HostPrivateKey"),
                               take("RootCertificate")};
    if (ld.host_id_.empty() || !ld.identity_.complete()) {
        err = "Pairing record missing required fields";
        return std::nullopt;
    }

    auto conn_mux = Usbmux::open(err);
    if (!conn_mux) {
        return std::nullopt;
    }
    auto sock = conn_mux->connect(device_id, kLockdownPort, err);
    if (!sock) {
        return std::nullopt;
    }
    ld.sock_ = std::move(*sock);

    plist::Value start = plist::Value::Dict();
    start.set("Request", plist::Value::Str("StartSession"));
    start.set("HostID", plist::Value::Str(ld.host_id_));
    start.set("SystemBUID", plist::Value::Str(ld.system_buid_));
    plist::Value reply;
    if (!ld.request(start, reply, err)) {
        return std::nullopt;
    }
    if (const auto *e = reply.find("Error"); e != nullptr) {
        err = lockdown_rejection("StartSession", e->as_string_or("?"));
        return std::nullopt;
    }
    if (const auto *sid = reply.find("SessionID")) {
        ld.session_id_ = sid->as_string_or("");
    }
    const auto *want_ssl = reply.find("EnableSessionSSL");
    const bool need_tls = want_ssl != nullptr ? want_ssl->as_bool_or(true) : true;
    if (need_tls && !ld.tls_.handshake(ld.sock_, ld.identity_, err)) {
        return std::nullopt;
    }
    return ld;
}

std::optional<Lockdown::ServiceEndpoint> Lockdown::start_service(std::string_view name,
                                                                 std::string &err, DeviceError *failure) {
    if (failure) *failure = DeviceError::other;
    plist::Value req = plist::Value::Dict();
    req.set("Request", plist::Value::Str("StartService"));
    req.set("Service", plist::Value::Str(std::string(name)));
    plist::Value reply;
    if (!request(req, reply, err)) {
        return std::nullopt;
    }
    if (const auto *e = reply.find("Error"); e != nullptr) {
        err = lockdown_rejection("StartService(" + std::string(name) + ")", e->as_string_or("?"), failure);
        return std::nullopt;
    }
    const auto *port = reply.find("Port");
    if (port == nullptr) {
        err = "StartService response missing Port";
        return std::nullopt;
    }
    ServiceEndpoint ep;
    ep.port = static_cast<uint16_t>(port->as_int_or(0));
    const auto *ssl = reply.find("EnableServiceSSL");
    ep.requires_tls = ssl != nullptr && ssl->as_bool_or(false);
    return ep;
}

std::optional<Lockdown::ServiceEndpoint> Lockdown::start_service_bounded(std::string_view name,
        const IoBudget &budget, std::string &err, DeviceError *failure) {
    if (failure) *failure = DeviceError::other;
    if (!budget.check(err)) return {};
    bounded_io_ = true;
    SocketBudgetGuard guard(sock_, budget);
    auto endpoint = start_service(name, err, failure);
    if (!guard.finish(err)) {
        if (failure) *failure = DeviceError::other;
        sock_.interrupt(); return {};
    }
    if (!endpoint) { sock_.interrupt(); return {}; }
    return endpoint;
}

}  // namespace xdb::transport
