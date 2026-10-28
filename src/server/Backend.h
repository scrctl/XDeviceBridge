// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include "ipc/Wire.h"
#include "remote/Device.h"
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <map>
namespace xdb::server {
using Json = nlohmann::json;
using Cancel = std::function<bool()>;
using Event = std::function<void(const std::string &, const Json &)>;
struct PrepareOptions {
    std::string serial, wifi, ddi_directory, data_directory;
    uint16_t wifi_port = 49152;
    bool replace_ddi = false;
    int ddi_download_timeout = 300, ddi_install_timeout = 300;
};
bool prepare_ddi(const PrepareOptions &, std::string &, const Event &, const Cancel &);
std::optional<Json> manage_ddi(const PrepareOptions &, bool remove, std::string &, const Cancel &);
std::optional<remote::Device> open_device(const std::string &, const std::string &, std::string &, uint16_t,
                                          const Cancel &, const std::string &);
struct DeviceConnection;
struct Registry {
    std::mutex mutex;
    std::set<std::string> devices;
    std::map<std::string, std::weak_ptr<void>> sessions;
    std::map<std::string, std::shared_ptr<DeviceConnection>> connections;
    bool acquire(const std::string &udid, bool use_connection = false);
    void release(const std::string &udid) {
        std::lock_guard lock(mutex);
        devices.erase(udid);
    }
};
struct Reply {
    bool ok = true;
    Json result = Json::object();
    std::string code, error;
    std::vector<uint8_t> binary;
    static Reply failure(std::string code, std::string message) {
        Reply r;
        r.ok = false;
        r.code = std::move(code);
        r.error = std::move(message);
        return r;
    }
};
class Backend {
  public:
    Backend(Registry &, std::string data_directory);
    ~Backend();
    bool has_session() const;
    // Drop dead/manual connections without occupying a client worker.
    static void maintain_connections(Registry &, bool stop_all = false);
    Reply handle(const std::string &method, const Json &params, const Event &, const Cancel &);

  private:
    struct Session;
    Registry &registry_;
    std::string directory_, lease_;
    std::shared_ptr<Session> session_;
    void close();
    Reply handle_management(const std::string &, const Json &, const Event &, const Cancel &);
    static void disconnect_connection(Registry &, const std::shared_ptr<DeviceConnection> &);
    Reply handle_persistent_connection(const std::string &, const Json &, const Cancel &);
    Reply handle_connection(const std::string &, const Json &, const Cancel &);
    Reply handle_input(const std::string &, const Json &, const Cancel &, Session &);
};
} // namespace xdb::server
