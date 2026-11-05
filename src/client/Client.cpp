// SPDX-License-Identifier: LGPL-3.0-only
#include "Process.h"
#include "transport/IoBudget.h"
#include "ipc/Json.h"
#include "ipc/Wire.h"
#include "xdb/xdb.h"
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
using Json = nlohmann::json;
struct xdb_response {
    xdb::ipc::Message message;
};
struct xdb_client {
    xdb::transport::Socket socket;
    std::mutex mutex;
    uint64_t next_id = 1;
    std::string server_version;
    xdb_event_callback event = nullptr;
    void *userdata = nullptr;
};
namespace {
thread_local std::string diagnostic;
std::timed_mutex startup_mutex;
bool resolve_port(uint16_t &port) {
    if (port)
        return true;
    port = 17423;
    if (const char *value = std::getenv("XDB_PORT"); value && *value) {
        unsigned parsed = 0;
        auto end = value + std::char_traits<char>::length(value);
        auto result = std::from_chars(value, end, parsed);
        if (result.ec != std::errc{} || result.ptr != end || !parsed || parsed > 65535) {
            diagnostic = "Invalid XDB_PORT";
            return false;
        }
        port = static_cast<uint16_t>(parsed);
    }
    return true;
}
std::unique_ptr<xdb_client> connect_existing(uint16_t port, bool &not_running, const xdb::transport::IoBudget *budget = nullptr) {
    auto client = std::make_unique<xdb_client>();
    client->socket = xdb::ipc::connect(port, diagnostic, &not_running, budget);
    if (!client->socket.valid())
        return nullptr;
    std::unique_ptr<xdb::transport::BudgetWatchdog> watch;
    if (budget) watch = std::make_unique<xdb::transport::BudgetWatchdog>(*budget, [&] { client->socket.interrupt(); });
    client->socket.set_read_timeout(5000, diagnostic);
    xdb_response *raw = nullptr;
    const auto params = Json{{"protocol", xdb::ipc::protocol_version}}.dump();
    const bool ok = xdb_client_request(client.get(), "hello", params.c_str(), &raw);
    std::unique_ptr<xdb_response> response(raw);
    if (watch && !watch->finish(diagnostic)) return nullptr;
    if (!ok) return nullptr;
    auto value = xdb::ipc::parse_json(response->message.json);
    if (!value.value("ok", false) || value.value("result", Json::object()).value("protocol", 0) != xdb::ipc::protocol_version) {
        diagnostic = "xdb handshake rejected: incompatible protocol; stop the existing server with its original xdb kill-server, then start the matching version";
        return nullptr;
    }
    client->server_version = value["result"].value("version", std::string());
    client->socket.set_read_timeout(0, diagnostic);
    return client;
}
std::unique_ptr<xdb_client> connect_or_start(uint16_t port, const char *directory, const xdb::transport::IoBudget *budget = nullptr) {
    bool not_running = false;
    auto client = connect_existing(port, not_running, budget);
    if (client || !not_running)
        return client;
    std::unique_lock lock(startup_mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(20))) {
        if (budget && !budget->check(diagnostic, "xdb startup")) return nullptr;
    }
    if (budget && !budget->check(diagnostic, "xdb startup")) return nullptr;
    client = connect_existing(port, not_running, budget);
    if (client || !not_running)
        return client;
    if (!xdb::client::launch_server(port, directory, diagnostic, budget))
        return nullptr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (budget && !budget->check(diagnostic, "xdb connection")) return nullptr;
        client = connect_existing(port, not_running, budget);
        if (client || !not_running)
            return client;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    diagnostic = "Timed out waiting for xdb server on port " + std::to_string(port);
    return nullptr;
}
} // namespace
extern "C" {
const char *xdb_client_version() { return XDB_VERSION_STRING; }
const char *xdb_client_server_version(const xdb_client *client) {
    return client ? client->server_version.c_str() : nullptr;
}
const char *xdb_last_error() { return diagnostic.c_str(); }
int xdb_client_start_server(uint16_t port, const char *directory) {
    diagnostic.clear();
    try {
        if (!resolve_port(port))
            return 0;
        if (directory && *directory) {
            std::string text(directory);
            if (!std::filesystem::path(std::u8string(text.begin(), text.end())).is_absolute()) {
                diagnostic = "Server data directory must be absolute";
                return 0;
            }
        }
        return connect_or_start(port, directory) != nullptr;
    } catch (const std::exception &e) {
        diagnostic = e.what();
        return 0;
    }
}
int xdb_client_kill_server(uint16_t port) {
    diagnostic.clear();
    try {
        if (!resolve_port(port))
            return 0;
        bool not_running = false;
        auto client = connect_existing(port, not_running);
        if (!client) {
            if (not_running)
                diagnostic.clear();
            return not_running;
        }
        client->socket.set_read_timeout(5000, diagnostic);
        xdb_response *raw = nullptr;
        if (!xdb_client_request(client.get(), "server.stop", "{}", &raw))
            return 0;
        std::unique_ptr<xdb_response> response(raw);
        auto value = xdb::ipc::parse_json(response->message.json);
        if (!value.value("ok", false)) {
            diagnostic = value.value("error", Json::object()).value("message", std::string("Server stop rejected"));
            return 0;
        }
        client.reset();
        // Wait until the listener is closed, so an immediate restart can reuse the port.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            std::string error;
            auto probe = xdb::ipc::connect(port, error, &not_running);
            if (not_running) {
                diagnostic.clear();
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (std::chrono::steady_clock::now() < deadline);
        diagnostic = "Timed out stopping xdb server";
        return 0;
    } catch (const std::exception &e) {
        diagnostic = e.what();
        return 0;
    }
}
xdb_client *xdb_client_connect(uint16_t port, xdb_event_callback event, void *userdata) {
    return xdb_client_connect_cancellable(port, event, userdata, nullptr, nullptr);
}
xdb_client *xdb_client_connect_cancellable(uint16_t port, xdb_event_callback event, void *userdata,
                                          xdb_cancel_callback cancel, void *cancel_userdata) {
    diagnostic.clear();
    try {
        if (!resolve_port(port))
            return nullptr;
        const xdb::transport::IoBudget budget({std::chrono::seconds(15),
            cancel ? std::function<bool()>([&] { return cancel(cancel_userdata) != 0; }) : std::function<bool()>()});
        if (cancel && !budget.check(diagnostic, "xdb connection")) return nullptr;
        auto client = connect_or_start(port, nullptr, cancel ? &budget : nullptr);
        if (cancel && !budget.check(diagnostic, "xdb connection")) return nullptr;
        if (!client)
            return nullptr;
        client->event = event;
        client->userdata = userdata;
        return client.release();
    } catch (const std::exception &e) {
        diagnostic = e.what();
        return nullptr;
    }
}
int xdb_client_request(xdb_client *client, const char *method, const char *params, xdb_response **out) {
    diagnostic.clear();
    if (out)
        *out = nullptr;
    if (!client || !method || !out) {
        diagnostic = "invalid request arguments";
        return 0;
    }
    try {
        std::lock_guard lock(client->mutex);
        auto p = xdb::ipc::parse_json(params ? params : "{}");
        if (!p.is_object()) {
            diagnostic = "params must be an object";
            return 0;
        }
        auto id = client->next_id++;
        xdb::ipc::Message request{Json{{"id", id}, {"method", method}, {"params", p}}.dump(), {}};
        if (!xdb::ipc::write(client->socket, request, diagnostic))
            return 0;
        for (;;) {
            auto response = std::make_unique<xdb_response>();
            if (!xdb::ipc::read(client->socket, response->message, diagnostic))
                return 0;
            auto value = xdb::ipc::parse_json(response->message.json);
            if (!value.is_object() || value.value("id", uint64_t(0)) != id) {
                diagnostic = "unexpected response id";
                return 0;
            }
            if (value.contains("event")) {
                if (client->event)
                    client->event(response->message.json.c_str(), client->userdata);
                continue;
            }
            if (!value.contains("ok") || !value["ok"].is_boolean()) {
                diagnostic = "invalid response";
                return 0;
            }
            *out = response.release();
            return 1;
        }
    } catch (const std::exception &e) {
        diagnostic = e.what();
        if (client)
            client->socket.interrupt();
        return 0;
    }
}
void xdb_client_interrupt(xdb_client *client) {
    if (client)
        client->socket.interrupt();
}
void xdb_client_destroy(xdb_client *client) { delete client; }
const char *xdb_response_json(const xdb_response *response) {
    return response ? response->message.json.c_str() : "";
}
const uint8_t *xdb_response_data(const xdb_response *response) {
    return response && !response->message.binary.empty() ? response->message.binary.data() : nullptr;
}
size_t xdb_response_size(const xdb_response *response) {
    return response ? response->message.binary.size() : 0;
}
void xdb_response_destroy(xdb_response *response) { delete response; }
}
