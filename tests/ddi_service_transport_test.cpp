#include "ddi/ServiceTransport.h"
#include "transport/TcpConnect.h"
#include "net/ByteStream.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <deque>

namespace {
namespace ddi = xdb::ddi;
using Socket = xdb::transport::Socket;
using V = xdb::plist::Value;
using Ms = std::chrono::milliseconds;
int failures = 0;
void check(bool ok, const char *what) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }
bool pair(Socket &client, Socket &peer) {
    std::string error;
    if (!xdb::transport::initialize_sockets(error)) return false;
    Socket server(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    if (!server.valid() || ::bind(server.fd(), reinterpret_cast<sockaddr *>(&address), length) ||
        ::getsockname(server.fd(), reinterpret_cast<sockaddr *>(&address), &length) || ::listen(server.fd(), 1)) return false;
    auto connected = xdb::transport::connect_tcp("127.0.0.1", ntohs(address.sin_port), 1000, error);
    if (!connected) return false;
    client = std::move(*connected); peer.reset(::accept(server.fd(), nullptr, nullptr));
    return peer.valid();
}
std::string frame(const V &value) {
    auto body = xdb::plist::write(value); const auto length = static_cast<uint32_t>(body.size());
    std::string data{char(length >> 24), char(length >> 16), char(length >> 8), char(length)};
    return data + body;
}
V command(const char *name) { auto v = V::Dict(); v.set("Command", V::Str(name)); return v; }
V status(const char *name) { auto v = V::Dict(); v.set("Status", V::Str(name)); return v; }
class Stream final : public xdb::net::ByteStream {
public:
    std::deque<std::vector<uint8_t>> packets;
    std::string sent;
    bool fail_send = false;
    void reply(const V &v) { const auto data = frame(v); packets.emplace_back(data.begin(), data.end()); }
    bool send(std::string_view data, std::string &error) override {
        if (fail_send) { error = "fake write failure"; return false; }
        sent += data; return true;
    }
    bool recv(std::vector<uint8_t> &out, int, std::string &error, bool *timed_out) override {
        if (timed_out) *timed_out = false;
        if (packets.empty()) { error = "fake stream exhausted"; return false; }
        out = std::move(packets.front()); packets.pop_front(); return true;
    }
};
void graceful_close() {
    for (int mode = 0; mode < 4; ++mode) {
        Stream stream;
        stream.reply(V::Dict());
        if (mode == 0 || mode == 3) stream.reply(V::Dict());
        if (mode == 1) { auto rejected = V::Dict(); rejected.set("Error", V::Str("UnknownCommand")); stream.reply(rejected); }
        if (mode == 2) stream.packets.push_back({0, 0, 0, 0});
        auto adapter = ddi::detail::service_transport_with_byte_stream(stream, [] {}, {});
        auto service = adapter->service(); std::string error;
        const auto query = command("LookupImage");
        const auto response = service.request(query, Ms(1000), error);
        const auto hangup = command("Hangup");
        if (mode == 3) check(bool(service.request(hangup, Ms(1000), error)), "Explicit Hangup receives its reply");
        error = "completed operation diagnostic";
        adapter.reset();
        check(response && stream.sent == frame(query) + frame(hangup) && stream.packets.empty() &&
            error == "completed operation diagnostic",
            "Teardown sends exactly one Hangup; empty, rejected or malformed replies cannot replace the operation result");
    }
}
void upload_close() {
    // Complete upload; partial upload; bytes sent but no completion; write
    // failure; premature Complete despite missing bytes; invalid upload ACK.
    for (int mode = 0; mode < 6; ++mode) {
        Stream stream; stream.reply(status(mode == 5 ? "Unexpected" : "ReceiveBytesAck"));
        auto adapter = ddi::detail::service_transport_with_byte_stream(stream, [] {}, {});
        auto service = adapter->service(); std::string error;
        auto upload = command("ReceiveBytes"); upload.set("ImageSize", V::Int(4));
        check(bool(service.request(upload, Ms(1000), error)), "ReceiveBytes returns a framed reply");
        stream.fail_send = mode == 3;
        const uint8_t bytes[]{1,2,3,4};
        const auto size = mode == 1 || mode == 4 ? 2 : 4;
        if (mode != 5) {
            const auto written = service.send_bytes(bytes, size, Ms(1000), error);
            check(written == (mode != 3), "Raw upload write outcome is preserved");
        }
        if (mode == 0 || mode == 4) {
            stream.reply(status("Complete"));
            check(bool(service.receive(Ms(1000), error)), "Upload completion is received");
        }
        if (mode == 0) stream.reply(V::Dict());
        const auto before = stream.sent;
        adapter.reset();
        check(stream.sent == before + (mode == 0 ? frame(command("Hangup")) : ""),
            mode == 0 ? "Complete upload permits graceful Hangup" : "Incomplete or failed upload never receives plist Hangup bytes");
    }
}
void failed_close() {
    Stream stream; stream.packets.push_back({0,0,0,0});
    auto adapter = ddi::detail::service_transport_with_byte_stream(stream, [] {}, {});
    auto service = adapter->service(); std::string error;
    check(!service.request(command("LookupImage"), Ms(1000), error), "Malformed reply fails the query");
    const auto sent = stream.sent, saved_error = error;
    adapter.reset();
    check(stream.sent == sent && error == saved_error, "Failed framed operation closes without further I/O or changing its error");
}
void reconnect_close() {
    Stream stream; stream.reply(V::Dict()); stream.reply(V::Dict());
    auto adapter = ddi::detail::service_transport_with_byte_stream(stream, [] {}, {});
    auto service = adapter->service(); std::string error;
    const auto query = command("LookupImage");
    check(bool(service.request(query, Ms(1000), error)), "Query succeeds before reconnect");
    // This seam has no USB/RSD endpoint, so opening the replacement fails.
    check(!service.reconnect(Ms(1000), error), "Missing replacement endpoint fails reconnect");
    adapter.reset();
    check(stream.sent == frame(query) + frame(command("Hangup")) && stream.packets.empty(),
        "Reconnect finishes the old session once; failed replacement adds no Hangup");
}
void bounded_close(bool cancel, bool already_cancelled = false) {
    Socket client, peer; check(pair(client, peer), "Create Hangup silent-peer connection"); if (!peer.valid()) return;
    std::atomic<bool> cancelled{false};
    auto adapter = ddi::detail::service_transport_with_socket(std::move(client), [&] { return cancelled.load(); });
    std::string error; const auto reply = frame(V::Dict());
    peer.write_all(reply.data(), reply.size(), error);
    check(bool(adapter->service().request(command("LookupImage"), Ms(1000), error)), "Initial query succeeds before teardown");
    std::vector<uint8_t> query; peer.read_len_prefixed_be(query, error);
    std::thread trigger;
    if (already_cancelled) cancelled = true;
    else if (cancel) trigger = std::thread([&] { std::this_thread::sleep_for(Ms(40)); cancelled = true; });
    const auto start = std::chrono::steady_clock::now();
    adapter.reset();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (trigger.joinable()) trigger.join();
    check(elapsed < (cancel ? Ms(500) : Ms(1500)), "Silent Hangup peer cannot exceed cleanup deadline or cancellation budget");
    if (already_cancelled) {
        std::vector<uint8_t> extra;
        check(!peer.read_len_prefixed_be(extra, error), "Already cancelled operation sends no Hangup");
    }
}
void fragmentation() {
    Socket client, peer; check(pair(client, peer), "Create local byte-protocol connection");
    if (!peer.valid()) return;
    auto adapter = ddi::detail::service_transport_with_socket(std::move(client), {}); auto service = adapter->service();
    auto first = V::Dict(); first.set("Status", V::Str("ReceiveBytesAck"));
    auto second = V::Dict(); second.set("Status", V::Str("Complete"));
    auto expected = frame(first) + frame(second);
    bool peer_ok = false;
    std::thread writer([&] {
        std::string error; std::vector<uint8_t> request;
        if (!peer.read_len_prefixed_be(request, error)) return;
        auto parsed = xdb::plist::parse(std::string_view(reinterpret_cast<const char *>(request.data()), request.size()));
        if (!parsed || !parsed->find("Command") || parsed->find("Command")->string != "ReceiveBytes") return;
        // Split every length-header byte; combine the second frame into a single write.
        for (size_t i = 0; i < 4; ++i) {
            if (!peer.write_all(expected.data() + i, 1, error)) return;
            std::this_thread::sleep_for(Ms(2));
        }
        peer_ok = peer.write_all(expected.data() + 4, expected.size() - 4, error);
    });
    auto request = V::Dict(); request.set("Command", V::Str("ReceiveBytes")); std::string error;
    auto response1 = service.request(request, Ms(2000), error);
    auto response2 = service.receive(Ms(2000), error);
    writer.join();
    check(peer_ok && response1 && response1->find("Status")->string == "ReceiveBytesAck" &&
        response2 && response2->find("Status")->string == "Complete", "Fragmented BE32 headers and coalesced replies retain independent frame boundaries");
    const std::string raw("image\0tail", 10);
    check(service.send_bytes(reinterpret_cast<const uint8_t *>(raw.data()), raw.size(), Ms(1000), error), "Upload accepts binary raw bytes without plist framing");
    char actual[10]; check(peer.read_exact(actual, sizeof(actual), error) && std::string(actual, 10) == raw, "Raw upload remains byte-for-byte identical");
}
void bounded(bool cancel) {
    Socket client, peer; check(pair(client, peer), "Create silent-peer connection"); if (!peer.valid()) return;
    std::atomic<bool> cancelled{false};
    auto adapter = ddi::detail::service_transport_with_socket(std::move(client), [&] { return cancelled.load(); });
    auto service = adapter->service(); std::string error;
    std::thread trigger;
    if (cancel) trigger = std::thread([&] { std::this_thread::sleep_for(Ms(40)); cancelled = true; });
    const auto start = std::chrono::steady_clock::now();
    auto response = service.receive(cancel ? Ms(1000) : Ms(50), error);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (trigger.joinable()) trigger.join();
    check(!response && elapsed < Ms(500) && error.find(cancel ? "cancelled" : "timed out") != std::string::npos,
        cancel ? "Cancellation interrupts a blocked receive and joins watcher before teardown" : "Deadline interrupts a silent peer rather than waiting for TCP EOF");
}
void invalid_frame() {
    Socket client, peer; check(pair(client, peer), "Create invalid-frame connection"); if (!peer.valid()) return;
    auto adapter = ddi::detail::service_transport_with_socket(std::move(client), {}); auto service = adapter->service();
    std::string error; const uint8_t header[]{0xff,0xff,0xff,0xff}; peer.write_all(header, 4, error);
    check(!service.receive(Ms(1000), error) && error.find("frame length") != std::string::npos,
        "Oversized peer length is rejected before allocation");
}
void tcp_fragments() {
    Stream stream;
    auto first = V::Dict(); first.set("Status", V::Str("ReceiveBytesAck"));
    auto second = V::Dict(); second.set("Status", V::Str("Complete"));
    const auto body = frame(first) + frame(second);
    for (size_t offset = 0; offset < body.size(); offset += 3)
        stream.packets.emplace_back(body.begin() + offset, body.begin() + std::min(offset + 3, body.size()));
    auto adapter = ddi::detail::service_transport_with_byte_stream(stream, [] {}, {}); auto service = adapter->service();
    std::string error;
    auto a = service.receive(Ms(1000), error), b = service.receive(Ms(1000), error);
    check(a && b && a->find("Status")->string == "ReceiveBytesAck" && b->find("Status")->string == "Complete" && stream.packets.empty(),
        "TCP recv chunks can split all headers and bodies while preserving leftover bytes for the next frame");
}
}
int main() {
    fragmentation(); bounded(false); bounded(true); invalid_frame(); tcp_fragments();
    graceful_close(); upload_close(); failed_close(); reconnect_close();
    bounded_close(false); bounded_close(true); bounded_close(true, true);
    return failures ? 1 : 0;
}
