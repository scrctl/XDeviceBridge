// SPDX-License-Identifier: LGPL-3.0-only
#include "Backend.h"
#include "ipc/Json.h"
#include "transport/IoBudget.h"
#include "xdb/xdb.h"
#include "wifi/DiscoverySubscription.h"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <arpa/inet.h>
#include <unistd.h>
#endif
namespace {
thread_local std::string diagnostic;
struct Worker {
    xdb::transport::Socket socket;
    std::jthread thread;
    std::atomic<bool> done{false};
    enum State { Busy, Idle, Evicted };
    std::atomic<State> state{Busy};
    void interrupt() {
#ifdef _WIN32
        // Stop discards in-flight calls; avoid lingering connections blocking a new listener.
        linger abort{1, 0};
        setsockopt(socket.fd(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&abort), sizeof(abort));
#endif
        socket.interrupt();
    }
};
} // namespace
struct xdb_server {
    xdb::transport::Socket listener;
    uint16_t port = 0;
    std::string directory;
    xdb::server::Registry registry;
    std::shared_ptr<xdb::wifi::mdns::detail::DiscoverySubscription> discovery;
    std::atomic<bool> stopping{false};
    std::jthread accept_thread;
    std::jthread connection_monitor;
    // Only accept_thread mutates workers; stop first joins it, then interrupts workers.
    std::vector<std::unique_ptr<Worker>> workers;
    void run_client(Worker &worker) noexcept;
    void accept_loop();
    ~xdb_server();
};
void xdb_server::run_client(Worker &worker) noexcept {
    using namespace xdb;
    using Json = nlohmann::json;
    std::string error;
    try {
        ipc::Message message;
        uint64_t id = 0;
        {
        transport::IoBudget budget({std::chrono::seconds(5), [&] { return stopping.load(); }});
        transport::SocketBudgetGuard handshake(worker.socket, budget);
        worker.socket.set_read_timeout(5000, error);
        if (!ipc::read(worker.socket, message, error) || !message.binary.empty()) {
            worker.done = true;
            return;
        }
        auto hello = xdb::ipc::parse_json(message.json);
        id = hello.value("id", uint64_t(0));
        auto params = hello.value("params", Json::object());
        if (hello.value("method", std::string()) != "hello" || params.value("protocol", 0) != ipc::protocol_version) {
            ipc::write(worker.socket,
                       {Json{{"id", id},
                             {"ok", false},
                             {"error",
                              {{"code", "handshake_rejected"},
                               {"message", "Protocol version or hello method rejected"}}}}
                            .dump(),
                        {}},
                       error);
            worker.done = true;
            return;
        }
        if (!ipc::write(
                worker.socket,
                {Json{{"id", id}, {"ok", true}, {"result", {{"protocol", ipc::protocol_version}, {"version", XDB_VERSION_STRING}}}}.dump(),
                 {}},
                error)) {
            worker.done = true;
            return;
        }
        if (!handshake.finish(error)) {
            worker.done = true;
            return;
        }
        }
        // Idle waits happen below. Once a frame starts, stalled chunks time out.
        worker.socket.set_read_timeout(5000, error);
        server::Backend backend(registry, directory);
        while (!stopping) {
            bool owns_session = backend.has_session();
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            if (!owns_session) worker.state = Worker::Idle;
            bool readable = false;
            while (!stopping && worker.state != Worker::Evicted) {
                if (owns_session && !backend.has_session()) {
                    owns_session = false;
                    worker.state = Worker::Idle;
                    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                }
                const int ready = transport::wait_socket(worker.socket.fd(), false, 100);
                if (ready != 0) { readable = ready > 0; break; }
                if (!owns_session && std::chrono::steady_clock::now() >= deadline) break;
            }
            if (!readable || !ipc::read(worker.socket, message, error)) break;
            if (!owns_session) {
                auto expected = Worker::Idle;
                if (!worker.state.compare_exchange_strong(expected, Worker::Busy)) break;
            }
            Json request = xdb::ipc::parse_json(message.json);
            if (!request.is_object() || !request.contains("id") || !request["id"].is_number_unsigned() ||
                !request.contains("method") || !request["method"].is_string() ||
                !request.value("params", Json::object()).is_object() || !message.binary.empty())
                break;
            id = request["id"].get<uint64_t>();
            std::atomic<bool> disconnected{false};
            std::mutex monitor_mutex;
            std::condition_variable monitor_changed;
            // Detect EOF without consuming protocol bytes. Also cancels in-flight pairing/DDI.
            std::jthread monitor([&](std::stop_token stop) {
                std::stop_callback wake(stop, [&] {
                    std::lock_guard lock(monitor_mutex);
                    monitor_changed.notify_all();
                });
                std::unique_lock lock(monitor_mutex);
                while (!stop.stop_requested() && !stopping) {
                    auto ready = transport::wait_socket(worker.socket.fd(), false, 0);
                    if (ready < 0) {
                        disconnected = true;
                        break;
                    }
                    if (ready > 0) {
                        char c;
                        auto n = ::recv(worker.socket.fd(), &c, 1, MSG_PEEK);
                        if (n <= 0) {
                            disconnected = true;
                            break;
                        }
                    }
                    // Keep the disconnect polling budget, but let request completion
                    // wake this wait immediately instead of delaying every reply.
                    monitor_changed.wait_for(lock, std::chrono::milliseconds(50),
                                             [&] { return stop.stop_requested() || stopping.load(); });
                }
            });
            auto cancelled = [&] { return stopping.load() || disconnected.load(); };
            auto event = [&](const std::string &name, const Json &data) {
                if (!ipc::write(worker.socket, {Json{{"id", id}, {"event", name}, {"data", data}}.dump(), {}},
                                error))
                    disconnected = true;
            };
            server::Reply result;
            const bool stop_requested = request["method"] == "server.stop" && !backend.read_only();
            try {
                auto method = request["method"].get<std::string>();
                if (method == "server.info")
                    result.result = {
                        {"protocol", ipc::protocol_version}, {"version", XDB_VERSION_STRING}, {"device_transport", "usb-or-wifi"},
                        {"capabilities", {{"video_max_fps", true}, {"video_max_bitrate", true}, {"mdns_diagnostics", true}, {"pairing_display_brand", true}, {"manual_connections", true},
                                          {"read_only_sessions", true}, {"shared_control_sessions", true}}},
#ifdef _WIN32
                        {"pid", GetCurrentProcessId()}};
#else
                        {"pid", getpid()}};
#endif
                else if (!stop_requested)
                    result =
                        backend.handle(method, request.value("params", Json::object()), event, cancelled);
            } catch (const std::exception &e) {
                result = server::Reply::failure("invalid_params", e.what());
            }
            monitor.request_stop();
            monitor.join();
            if (cancelled())
                break;
            Json reply = {{"id", id}, {"ok", result.ok}};
            if (result.ok)
                reply["result"] = std::move(result.result);
            else
                reply["error"] = {{"code", result.code}, {"message", result.error}};
            auto written = ipc::write(worker.socket, {reply.dump(), std::move(result.binary)}, error);
            if (stop_requested) {
#ifdef _WIN32
                // Let the requester receive the reply and close before aborting the other clients.
                worker.socket.set_read_timeout(1000, error);
                char ignored;
                worker.socket.read_exact(&ignored, 1, error);
#endif
                stopping = true;
            }
            if (!written)
                break;
        }
    } catch (...) { /* A malformed client must not terminate the server. */
    }
    worker.socket.interrupt();
    worker.done = true;
}
void xdb_server::accept_loop() {
    using namespace xdb;
    while (!stopping) {
        std::erase_if(workers, [](const auto &worker) { return worker->done.load(); });
        if (transport::wait_socket(listener.fd(), false, 100) <= 0)
            continue;
        transport::Socket socket(::accept(listener.fd(), nullptr, nullptr));
        if (!socket.valid())
            continue;
        if (workers.size() >= 16) {
            // Reclaim only sessionless idle clients. CAS prevents racing a new RPC.
            for (auto it = workers.begin(); it != workers.end(); ++it) {
                auto expected = Worker::Idle;
                if (!(*it)->state.compare_exchange_strong(expected, Worker::Evicted)) continue;
                (*it)->interrupt();
                (*it)->thread.join();
                workers.erase(it);
                break;
            }
            if (workers.size() >= 16) continue;
        }
        auto worker = std::make_unique<Worker>();
        worker->socket = std::move(socket);
        auto *raw = worker.get();
        worker->thread = std::jthread([this, raw] { run_client(*raw); });
        workers.push_back(std::move(worker));
    }
    listener.close();
    for (auto &worker : workers)
        worker->interrupt();
}
xdb_server::~xdb_server() {
    stopping = true;
    if (connection_monitor.joinable()) connection_monitor.join();
    xdb::server::Backend::maintain_connections(registry, true);
    if (accept_thread.joinable())
        accept_thread.join();
    for (auto &worker : workers)
        worker->interrupt();
    for (auto &worker : workers)
        if (worker->thread.joinable())
            worker->thread.join();
}
extern "C" {
const char *xdb_server_last_error() { return diagnostic.c_str(); }
xdb_server *xdb_server_start(uint16_t port, const char *directory) {
    diagnostic.clear();
    try {
        const std::string text = directory ? directory : "";
        if (!std::filesystem::path(std::u8string(text.begin(), text.end())).is_absolute()) {
            diagnostic = "Server data directory must be absolute";
            return nullptr;
        }
        auto server = std::make_unique<xdb_server>();
        server->directory = directory;
        auto *owner = server.get();
        server->connection_monitor = std::jthread([owner] {
            while (!owner->stopping) {
                xdb::server::Backend::maintain_connections(owner->registry);
                for (int i = 0; i < 10 && !owner->stopping; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            xdb::server::Backend::maintain_connections(owner->registry, true);
        });
        if (!xdb::transport::initialize_sockets(diagnostic))
            return nullptr;
        server->listener = xdb::transport::Socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (!server->listener.valid()) {
            diagnostic = xdb::transport::socket_error_message();
            return nullptr;
        }
        int enabled = 1;
#ifdef _WIN32
        const int option = SO_EXCLUSIVEADDRUSE;
#else
        const int option = SO_REUSEADDR;
#endif
        if (::setsockopt(server->listener.fd(), SOL_SOCKET, option,
                         reinterpret_cast<const char *>(&enabled), sizeof(enabled)) != 0) {
            diagnostic = xdb::transport::socket_error_message();
            return nullptr;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(server->listener.fd(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
            ::listen(server->listener.fd(), 16) != 0) {
            diagnostic = xdb::transport::socket_error_message();
            return nullptr;
        }
#ifdef _WIN32
        int size = sizeof(addr);
#else
        socklen_t size = sizeof(addr);
#endif
        if (getsockname(server->listener.fd(), reinterpret_cast<sockaddr *>(&addr), &size) != 0) {
            diagnostic = xdb::transport::socket_error_message();
            return nullptr;
        }
        server->port = ntohs(addr.sin_port);
        server->discovery = xdb::wifi::mdns::detail::acquire_discovery_subscription();
        server->accept_thread = std::jthread([raw = server.get()] { raw->accept_loop(); });
        return server.release();
    } catch (const std::exception &e) {
        diagnostic = e.what();
        return nullptr;
    }
}
uint16_t xdb_server_port(const xdb_server *server) { return server ? server->port : 0; }
int xdb_server_is_running(const xdb_server *server) { return server && !server->stopping; }
void xdb_server_stop(xdb_server *server) { delete server; }
}
