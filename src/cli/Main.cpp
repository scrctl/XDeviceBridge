// SPDX-License-Identifier: LGPL-3.0-only
#include "ipc/Json.h"
#include "cli/Help.h"
#include "util/DataDirectory.h"
#include "ipc/Wire.h"
#include "xdb/xdb.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
using Json = nlohmann::json;
namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
const char *env(const char *name) {
    auto *value = std::getenv(name);
    return value ? value : "";
}
void event(const char *message, void *) {
    auto value = xdb::ipc::parse_json(message);
    auto data = value.value("data", Json::object());
    if (value.value("event", std::string()) == "pin")
        std::cerr << "Pairing code: " << data.value("pin", std::string()) << '\n';
    else if (data.contains("message"))
        std::cerr << data["message"].get<std::string>() << '\n';
    else
        std::cerr << data.dump() << '\n';
}
int number(const std::string &value, int minimum, int maximum) {
    size_t used = 0;
    auto n = std::stoll(value, &used);
    if (used != value.size() || n < minimum || n > maximum)
        throw std::invalid_argument("Numeric option out of range");
    return int(n);
}

std::string endpoint_label(const Json &candidate) {
    auto label = candidate["transport"].get<std::string>();
    auto address = candidate["address"].get<std::string>();
    if (!address.empty()) {
        if (address.find(':') != std::string::npos)
            address = "[" + address + "]";
        label += " " + address + ":" + std::to_string(candidate["port"].get<int>());
    }
    return label;
}

void print_devices(const Json &result, bool detailed) {
    std::cout << "List of iOS devices\n";
    for (const auto &device : result["devices"]) {
        auto udid = device.value("udid", std::string());
        std::cout << (udid.empty() ? "(unidentified)" : udid) << "\t"
                  << device.value("state", std::string("discovered"));
        if (detailed) {
            auto name = device.value("name", std::string());
            if (!name.empty()) std::cout << "  name:" << name;
            std::cout << "  connection:" << device.value("connection", std::string("unverified"));
        }
        std::cout << '\n';
        if (!detailed) continue;

        const auto pairing = device.value("pairing", std::string("unknown"));
        size_t width = 0;
        for (const auto &candidate : device["candidates"])
            width = std::max(width, endpoint_label(candidate).size());
        for (const auto &candidate : device["candidates"]) {
            auto label = endpoint_label(candidate);
            auto interface_name = candidate.value("interface", std::string());
            auto type = candidate.value("connection_type", std::string());
            auto candidate_pairing = candidate.value("pairing", std::string("unknown"));
            const bool unchecked_usb = candidate.value("transport", std::string()) == "usb" &&
                                       candidate_pairing == "unknown";
            const bool show_pairing = !unchecked_usb && !candidate_pairing.empty() &&
                                      candidate_pairing != pairing;
            std::cout << "  " << label;
            if (!interface_name.empty() || !type.empty() || show_pairing)
                std::cout << std::string(width - label.size(), ' ');
            if (!type.empty()) std::cout << "  type:" << type;
            if (!interface_name.empty()) std::cout << "  interface:" << interface_name;
            if (show_pairing) std::cout << "  pairing:" << candidate_pairing;
            std::cout << '\n';
        }
    }
    for (const auto &warning : result["warnings"])
        std::cerr << "warning: " << warning.get<std::string>() << '\n';
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 2) {
            xdb::cli::print_help({});
            return 0;
        }
        uint16_t port = 17423;
        std::string dir, output, serial, wifi_address, cache;
        bool wifi = false, usb = false, replace = false, json_output = false, long_output = false;
        bool show_help = false, explicit_port = false;
        int timeout = -1;
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--help" || a == "-h")
                show_help = true;
            else if (a == "--wifi") {
                wifi = true;
                wifi_address = "auto";
                if (i + 1 < argc) {
                    std::string next = argv[i + 1];
                    if (next == "auto" || next.find('.') != std::string::npos ||
                        next.find(':') != std::string::npos) {
                        wifi_address = next;
                        ++i;
                    }
                }
            } else if (a == "--usb")
                usb = true;
            else if (a == "--replace")
                replace = true;
            else if (a == "--json")
                json_output = true;
            else if (a == "-l") {
                long_output = true;
            } else if (a == "--port" || a == "--data-dir" || a == "--output" || a == "-s" ||
                       a == "--serial" || a == "--timeout" || a == "--cache-dir") {
                if (++i == argc)
                    throw std::invalid_argument("Missing value for " + a);
                if (a == "--port") {
                    port = uint16_t(number(argv[i], 0, 65535));
                    explicit_port = true;
                } else if (a == "--data-dir")
                    dir = argv[i];
                else if (a == "--output")
                    output = argv[i];
                else if (a == "-s" || a == "--serial")
                    serial = argv[i];
                else if (a == "--timeout")
                    timeout = number(argv[i], 0, 300000);
                else
                    cache = argv[i];
            } else if (a.starts_with('-'))
                throw std::invalid_argument("Unknown option: " + a);
            else
                args.push_back(a);
        }
        if (show_help || (args.size() == 1 && args[0] == "ddi")) {
            xdb::cli::print_help(args);
            return 0;
        }
        if (!explicit_port && *env("XDB_PORT"))
            port = uint16_t(number(env("XDB_PORT"), 1, 65535));
        if (wifi && usb)
            throw std::invalid_argument("Choose --usb or --wifi");
        if (args.empty())
            throw std::invalid_argument("Missing command");
        if (args[0] == "server") {
            if (args.size() != 1)
                throw std::invalid_argument("Unexpected server argument");
            if (dir.empty())
                dir = xdb::util::default_data_directory();
            auto *server = xdb_server_start(port, dir.c_str());
            if (!server) {
                std::cerr << xdb_server_last_error() << '\n';
                return 1;
            }
            std::unique_ptr<xdb_server, decltype(&xdb_server_stop)> owned(server, xdb_server_stop);
            std::signal(SIGINT, stop);
            std::signal(SIGTERM, stop);
            std::cout << Json{{"ready", true},
                              {"protocol", xdb::ipc::protocol_version},
                              {"host", "127.0.0.1"},
                              {"port", xdb_server_port(server)}}
                             .dump()
                      << std::endl;
            while (!stopping && xdb_server_is_running(server))
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return 0;
        }
        if (args[0] == "start-server" || args[0] == "kill-server") {
            if (args.size() != 1 || !port)
                throw std::invalid_argument("Server lifecycle commands require a fixed port and no extra arguments");
            auto ok = args[0] == "start-server"
                          ? xdb_client_start_server(port, dir.empty() ? nullptr : dir.c_str())
                          : xdb_client_kill_server(port);
            if (!ok)
                std::cerr << xdb_last_error() << '\n';
            return ok ? 0 : 1;
        }
        if (args[0] != "devices" && args[0] != "pair" && args[0] != "ddi" && args[0] != "screenshot" &&
            args[0] != "rpc" && args[0] != "rpc-shell")
            throw std::invalid_argument("Unknown command; run xdb --help");
        if (args[0] == "ddi") {
            if (args.size() != 2 ||
                (args[1] != "prepare" && args[1] != "status" && args[1] != "remove"))
                throw std::invalid_argument("Expected ddi prepare/status/remove; run xdb ddi --help");
            if (args[1] != "prepare" && (replace || !cache.empty()))
                throw std::invalid_argument("--replace and --cache-dir apply only to ddi prepare");
        }
        if (!dir.empty() && !xdb_client_start_server(port, dir.c_str())) {
            std::cerr << xdb_last_error() << '\n';
            return 1;
        }
        std::unique_ptr<xdb_client, decltype(&xdb_client_destroy)> client(
            xdb_client_connect(port, event, nullptr), xdb_client_destroy);
        if (!client) {
            std::cerr << xdb_last_error() << '\n';
            return 1;
        }
        auto request = [&](const std::string &method, const Json &params, bool display = true) {
            auto text = params.dump();
            xdb_response *raw = nullptr;
            if (!xdb_client_request(client.get(), method.c_str(), text.c_str(), &raw))
                throw std::runtime_error(xdb_last_error());
            std::unique_ptr<xdb_response, decltype(&xdb_response_destroy)> response(raw,
                                                                                    xdb_response_destroy);
            auto value = xdb::ipc::parse_json(xdb_response_json(raw));
            if (!value.value("ok", false)) {
                if (display && json_output)
                    std::cout << value.dump() << '\n';
                else
                    std::cerr << value["error"].value("message", std::string("Request failed")) << '\n';
                return value;
            }
            if (xdb_response_size(raw)) {
                if (output.empty())
                    throw std::invalid_argument("Binary response requires --output FILE");
                std::ofstream file(std::filesystem::u8path(output), std::ios::binary);
                file.write(reinterpret_cast<const char *>(xdb_response_data(raw)), xdb_response_size(raw));
                if (!file)
                    throw std::runtime_error("Failed to write binary output");
            }
            if (display) {
                if (json_output || args[0] == "rpc" || args[0] == "rpc-shell")
                    std::cout << value.dump() << std::endl;
                else if (method == "devices")
                    print_devices(value["result"], long_output);
                else if (method == "pair")
                    std::cout << "Paired: " << value["result"].value("udid", std::string()) << '\n';
                else if (method == "ddi.status") {
                    const auto &state = value["result"];
                    if (state.at("mounted").get<bool>())
                        std::cout << "DDI mounted: " << state.at("image_type").get<std::string>()
                                  << " (" << state.at("udid").get<std::string>() << ")\n";
                    else
                        std::cout << "DDI not mounted: " << state.at("udid").get<std::string>() << '\n';
                    const auto installed = state.value("cryptex_installed", Json(nullptr));
                    if (installed == true)
                        std::cout << "DDI Cryptex installed: " << state.at("cryptex_identifier").get<std::string>()
                                  << " " << state.at("cryptex_version").get<std::string>() << '\n';
                    else
                        std::cout << "DDI Cryptex: " << (installed.is_null() ? "unavailable" : "not installed")
                                  << '\n';
                } else if (method == "ddi.remove")
                    std::cout << (value["result"].at("changed").get<bool>() ? "DDI removed: "
                                                                          : "DDI already absent: ")
                              << value["result"].at("udid").get<std::string>() << '\n';
                else
                    std::cout << "OK\n";
            }
            return value;
        };
        if (args[0] == "rpc-shell") {
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line.empty())
                    continue;
                std::string method;
                Json params;
                try {
                    auto v = xdb::ipc::parse_json(line);
                    method = v.at("method").get<std::string>();
                    params = v.value("params", Json::object());
                    if (method.empty() || !params.is_object())
                        throw std::invalid_argument("Expected a non-empty method string and an object params");
                } catch (const Json::exception &e) {
                    std::cerr << "xdb rpc-shell: " << e.what() << '\n';
                    continue;
                } catch (const std::invalid_argument &e) {
                    std::cerr << "xdb rpc-shell: " << e.what() << '\n';
                    continue;
                }
                request(method, params);
            }
            return 0;
        }
        if (args[0] == "rpc") {
            if (args.size() < 2 || args.size() > 3)
                throw std::invalid_argument("rpc requires METHOD [JSON_PARAMS]");
            return request(args[1], args.size() == 3 ? xdb::ipc::parse_json(args[2]) : Json::object())
                           .value("ok", false)
                       ? 0
                       : 1;
        }
        Json params = Json::object();
        if (!serial.empty())
            params["udid"] = serial;
        if (args[0] == "devices" || args[0] == "pair") {
            if (args.size() != 1)
                throw std::invalid_argument("Unexpected argument");
            params["transport"] = usb ? "usb" : (wifi || args[0] == "pair" ? "wifi" : "auto");
            if (timeout >= 0)
                params["timeout_ms"] = timeout;
            return request(args[0], params).value("ok", false) ? 0 : 1;
        }
        if (args[0] != "screenshot" && args[0] != "ddi")
            throw std::invalid_argument("Unknown command; run xdb --help");
        if (args[0] == "screenshot" && args.size() != 2)
            throw std::invalid_argument("Expected screenshot FILE");
        if (serial.empty()) {
            Json options = {{"transport", usb ? "usb" : wifi ? "wifi" : "auto"}};
            if (timeout >= 0)
                options["timeout_ms"] = timeout;
            auto found = request("devices", options, false);
            if (!found.value("ok", false))
                return 1;
            if (found["result"].value("cancelled", false))
                throw std::runtime_error("Device discovery was cancelled");
            std::set<std::string> identifiers;
            bool unidentified = false;
            for (const auto &device : found["result"]["devices"]) {
                if (wifi && wifi_address != "auto") {
                    bool matches = false;
                    for (const auto &candidate : device["candidates"])
                        matches |= candidate.value("transport", std::string()) == "wifi" &&
                                   candidate.value("address", std::string()) == wifi_address;
                    if (!matches)
                        continue;
                }
                auto id = device.value("udid", std::string());
                if (id.empty())
                    unidentified = true;
                else
                    identifiers.insert(id);
            }
            if (identifiers.empty())
                throw std::invalid_argument(unidentified ? "Discovered device has no known UDID; pair it first"
                                                         : "No device found");
            if (identifiers.size() > 1 || unidentified) {
                std::string message = "Multiple devices; select one with -s UDID:";
                for (const auto &id : identifiers)
                    message += " " + id;
                if (unidentified)
                    message += " (also unidentified devices)";
                throw std::invalid_argument(message);
            }
            serial = *identifiers.begin();
            params["udid"] = serial;
        }
        if (wifi)
            params["wifi"] = wifi_address;
        if (args[0] == "ddi") {
            if (args[1] == "prepare") {
                params["replace"] = replace;
                if (!cache.empty())
                    params["cache_directory"] = cache;
            }
            return request("ddi." + args[1], params).value("ok", false) ? 0 : 1;
        }
        if (args[0] == "screenshot" && args.size() == 2) {
            output = args[1];
            params["transport"] = usb ? "usb" : wifi ? "wifi" : "auto";
            if (!request("connection.open", params, false).value("ok", false))
                return 1;
            return request("screenshot", Json::object()).value("ok", false) ? 0 : 1;
        }
        throw std::invalid_argument("Unknown command; run xdb --help");
    } catch (const std::exception &e) {
        std::cerr << "xdb: " << e.what() << '\n';
        return 2;
    }
}
