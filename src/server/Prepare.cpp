// SPDX-License-Identifier: LGPL-3.0-only
#include "Backend.h"
#include "ddi/Cryptex.h"
#include "ddi/Mounter.h"
#include "ddi/ServiceTransport.h"
#include "ddi/Tss.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>

namespace xdb::server {
namespace {
std::string safe_text(const std::string &text) {
    std::string out;
    for (unsigned char c : text) {
        if (c < 32 || c == 127) {
            char escaped[5];
            std::snprintf(escaped, sizeof(escaped), "\\x%02x", c);
            out += escaped;
        } else
            out += static_cast<char>(c);
    }
    return out;
}
struct DdiTarget {
    ddi::DownloadTarget target;
    std::optional<uint64_t> ecid;
    std::optional<transport::DeviceRecord> record;
    std::optional<transport::Lockdown> lockdown;
    std::optional<remote::Device> device;

    bool connect(const PrepareOptions &o, std::string &error, const Cancel &cancelled,
                 transport::DeviceError *failure) {
        if (failure) *failure = transport::DeviceError::other;
        using namespace std::chrono;
        if (o.wifi.empty()) {
            transport::IoBudget preparation({seconds(30), cancelled});
            std::vector<transport::DeviceRecord> records;
            if (transport::Usbmux::list_devices_for_discovery(records, milliseconds(1000), cancelled, error) !=
                transport::UsbmuxDiscoveryStatus::complete)
                return false;
            record = remote::detail::select_usbmux_device(records, o.serial, error, true);
            if (!record)
                return false;
            lockdown =
                transport::Lockdown::establish_bounded(record->device_id, record->udid, preparation, error, failure);
            if (!lockdown)
                return false;
            auto query = plist::Value::Dict();
            query.set("Request", plist::Value::Str("GetValue"));
            plist::Value reply;
            if (!lockdown->request_bounded(
                    query, reply, preparation.remaining(), [&preparation] { return preparation.stopped(); },
                    error))
                return false;
            if (!preparation.check(error))
                return false;
            const auto *values = reply.find("Value");
            if (!values || !values->is_dict() || reply.find("Error")) {
                error = "DDI target metadata is missing its OS version or product model";
                return false;
            }
            auto text = [&](const char *key) {
                const auto *v = values->find(key);
                return v ? v->as_string_or() : std::string{};
            };
            auto number = [&](const char *key) -> std::optional<uint64_t> {
                const auto *v = values->find(key);
                if (v && (v->is_uint() || (v->is_int() && v->integer >= 0)))
                    return v->as_uint_or();
                return std::nullopt;
            };
            target.system_version = text("ProductVersion");
            target.product_type = text("ProductType");
            target.hardware_model = text("HardwareModel");
            target.board_id = number("BoardId");
            target.chip_id = number("ChipID");
            ecid = number("UniqueChipID");
        } else {
            device = open_device(o.serial, o.wifi, error, o.wifi_port, cancelled, o.data_directory, failure);
            if (!device)
                return false;
            target.system_version = device->property("OSVersion");
            target.product_type = device->property("ProductType");
            target.hardware_model = device->property("HardwareModel");
            const auto *properties = device->rsd().properties();
            auto number = [&](const char *key) -> std::optional<uint64_t> {
                const auto *v = properties ? properties->find(key) : nullptr;
                if (v && v->type == xpc::Type::UInt64)
                    return v->uint64;
                if (v && v->type == xpc::Type::Int64 && v->int64 >= 0)
                    return static_cast<uint64_t>(v->int64);
                return std::nullopt;
            };
            target.board_id = number("BoardId");
            target.chip_id = number("ChipID");
            ecid = number("UniqueChipID");
        }
        return true;
    }
};
} // namespace
bool prepare_ddi(const PrepareOptions &o, std::string &error, const Event &event, const Cancel &cancelled,
                 transport::DeviceError *failure) {
    if (failure) *failure = transport::DeviceError::other;
    using namespace std::chrono;
    auto emit = [&](const char *format, auto... args) {
        char buffer[2048];
        if constexpr (sizeof...(args) == 0) {
            std::snprintf(buffer, sizeof(buffer), "%s", format);
        } else {
            std::snprintf(buffer, sizeof(buffer), format, args...);
        }
        event("progress", {{"message", buffer}});
    };
    auto fail = [&] {
        emit("DDI installation failed: %s\n", safe_text(error).c_str());
        emit("An interrupted installation may already have taken effect; check device state before "
                    "replacing again.\n");
        return false;
    };
    DdiTarget connected;
    if (!connected.connect(o, error, cancelled, failure))
        return fail();
    auto &[target, ecid, record, lockdown, device] = connected;
    if (cancelled && cancelled()) {
        error = "DDI installation cancelled";
        return fail();
    }
    if (target.product_type.empty()) {
        error = "DDI target metadata is missing its OS version or product model";
        return fail();
    }
    auto selected = ddi::select_for_version(target.system_version, error);
    if (!selected)
        return fail();
    if (!o.replace_ddi) {
        if (selected->kind != ddi::Kind::Classic && !device) {
            transport::IoBudget preparation({seconds(30), cancelled});
            device = remote::Device::establish_bounded(record->udid, preparation, error, false, true);
            if (!device)
                return fail();
        }
        if (selected->kind == ddi::Kind::Classic ||
            device->rsd().supports("com.apple.coredevice.screencaptureservice",
                                   "com.apple.coredevice.feature.capturescreenshot")) {
            auto adapter = !o.wifi.empty()
                               ? ddi::ServiceTransport::open_remote(*device, seconds(30), cancelled, error)
                               : ddi::ServiceTransport::open_usb(*lockdown, record->device_id, seconds(30),
                                                                 cancelled, error);
            if (!adapter)
                return fail();
            auto wire = adapter->service();
            ddi::MountOptions options;
            options.timeout = seconds(30);
            options.cancelled = cancelled;
            const auto mounted = ddi::inspect_mounted(wire, target, options, error);
            if (!mounted)
                return fail();
            if (mounted->state != ddi::MountState::NotMounted) {
                if (selected->kind == ddi::Kind::Classic && mounted->signature_matches != true) {
                    error = "Mounted Classic DDI signature does not match the requested image; enable "
                                   "replacement explicitly";
                    return fail();
                }
                emit("DDI already mounted: %s (no new installation)\n", mounted->image_type.c_str());
                return true;
            }
        }
    }
    emit("DDI target: OS %s, model %s, image type %s\n", safe_text(target.system_version).c_str(),
         safe_text(target.product_type).c_str(), std::string(ddi::kind_name(selected->kind)).c_str());
    ddi::DownloadOptions download;
    download.cancelled = cancelled;
    download.timeout = seconds(o.ddi_download_timeout);
    if (!o.ddi_directory.empty()) {
        std::error_code ec;
        download.cache_root = std::filesystem::absolute(
            std::filesystem::path(std::u8string(o.ddi_directory.begin(), o.ddi_directory.end())), ec);
        if (ec) {
            error = ec.message();
            return fail();
        }
    }
    std::string last_file;
    uint64_t last_bytes = 0;
    auto progress = [&](const std::string &file, uint64_t received, uint64_t total) {
        if (last_file != file || received == total || received >= last_bytes + 1024 * 1024) {
            emit("DDI transfer: %s (%llu/%llu bytes)\n", safe_text(file).c_str(),
                 static_cast<unsigned long long>(received), static_cast<unsigned long long>(total));
            last_file = file;
            last_bytes = received;
        }
    };
    download.progress = progress;
    auto image = ddi::download_for_target(download, target, error);
    if (!image)
        return fail();
    // Establishing the CoreDevice tunnel is connection preparation. The
    // installation budget starts once that transport is available.
    if (selected->kind == ddi::Kind::Cryptex && !device) {
        transport::IoBudget preparation({seconds(30), cancelled});
        device = remote::Device::establish_bounded(record->udid, preparation, error, false, true);
        if (!device)
            return fail();
    }
    if (cancelled && cancelled()) {
        error = "DDI installation cancelled";
        return fail();
    }
    const auto deadline = steady_clock::now() + seconds(o.ddi_install_timeout);
    auto remaining = [&] { return duration_cast<milliseconds>(deadline - steady_clock::now()); };
    auto stopped = [&] { return (cancelled && cancelled()) || remaining() <= milliseconds::zero(); };
    auto service = [&](milliseconds timeout) -> std::unique_ptr<ddi::ServiceTransport> {
        timeout = std::min(timeout, remaining());
        if (device && !o.wifi.empty())
            return ddi::ServiceTransport::open_remote(*device, timeout, stopped, error);
        return ddi::ServiceTransport::open_usb(*lockdown, record->device_id, timeout, stopped, error);
    };
    if (selected->kind == ddi::Kind::Cryptex) {
        ddi::CryptexInstallOptions options;
        options.timeout = seconds(o.ddi_install_timeout);
        options.cancelled = stopped;
        options.replace_existing = o.replace_ddi;
        options.progress = progress;
        options.personalized_mounted = [&](milliseconds timeout, std::string &out) -> std::optional<bool> {
            auto adapter = service(timeout);
            if (!adapter) {
                out = error;
                return std::nullopt;
            }
            auto wire = adapter->service();
            ddi::MountOptions inspect;
            inspect.timeout = seconds(o.ddi_install_timeout);
            inspect.cancelled = stopped;
            auto result = ddi::inspect_mounted(wire, ddi::Kind::Personalized, inspect, out);
            if (!result)
                return std::nullopt;
            return result->state != ddi::MountState::NotMounted;
        };
        options.remove_personalized = [&](milliseconds timeout, std::string &out) {
            auto adapter = service(timeout);
            if (!adapter) {
                out = error;
                return false;
            }
            auto wire = adapter->service();
            ddi::MountOptions inspect;
            inspect.timeout = seconds(o.ddi_install_timeout);
            inspect.cancelled = stopped;
            return ddi::unmount_current(wire, ddi::Kind::Personalized, inspect, out);
        };
        auto result = ddi::install_cryptex(*device, *image, target, options, error);
        if (!result)
            return fail();
        auto adapter = service(remaining());
        if (!adapter)
            return fail();
        auto wire = adapter->service();
        ddi::MountOptions verify;
        verify.timeout = seconds(o.ddi_install_timeout);
        verify.cancelled = stopped;
        const auto mounted = ddi::inspect_mounted(wire, ddi::Kind::Cryptex, verify, error);
        if (!mounted)
            return fail();
        if (mounted->state == ddi::MountState::NotMounted) {
            error = "DDI Cryptex is installed but its developer image is not mounted";
            return fail();
        }
        emit(result->already_installed ? "DDI already installed: %s %s (no new installation)\n"
                                       : "DDI installed and verified: %s %s\n",
             safe_text(result->identifier).c_str(), safe_text(result->version).c_str());
    } else {
        if (selected->kind == ddi::Kind::Personalized && !ecid) {
            error = "DDI installation requires the device ECID";
            return fail();
        }
        auto adapter = service(remaining());
        if (!adapter)
            return fail();
        auto wire = adapter->service();
        ddi::MountOptions options;
        options.timeout = seconds(o.ddi_install_timeout);
        options.cancelled = stopped;
        options.replace_existing = o.replace_ddi;
        options.progress = [&](uint64_t current, uint64_t total) {
            progress("DeveloperDiskImage.dmg", current, total);
        };
        options.personalize = [&](const plist::Value &manifest, const plist::Value &identifiers, uint64_t id,
                                  const std::vector<uint8_t> &nonce, milliseconds timeout, std::string &out) {
            auto request = ddi::build_personalized_tss_request(manifest, identifiers, id, nonce, out);
            if (!request)
                return std::optional<std::vector<uint8_t>>{};
            ddi::TssOptions tss;
            tss.timeout = std::min(timeout, remaining());
            tss.cancelled = stopped;
            return ddi::request_tss_ticket(*request, tss, out);
        };
        auto result = ddi::mount_image(wire, *image, target, ecid.value_or(0), options, error);
        if (!result)
            return fail();
        emit(result->state == ddi::MountState::AlreadyMounted
                 ? "DDI already mounted: %s (no new installation)\n"
                 : "DDI mounted and verified: %s\n",
             safe_text(result->image_type).c_str());
    }
    return true;
}
std::optional<Json> manage_ddi(const PrepareOptions &o, bool remove, std::string &error,
                              const Cancel &cancelled, transport::DeviceError *failure) {
    if (failure) *failure = transport::DeviceError::other;
    using namespace std::chrono;
    DdiTarget connected;
    if (!connected.connect(o, error, cancelled, failure))
        return std::nullopt;
    const auto selected = ddi::select_for_version(connected.target.system_version, error);
    if (!selected)
        return std::nullopt;
    // Modern systems can expose a Cryptex DDI independently of the download
    // policy. Inspect the actual installation before choosing its removal path.
    if (selected->kind != ddi::Kind::Classic && !connected.device) {
        transport::IoBudget preparation({seconds(30), cancelled});
        connected.device = remote::Device::establish_bounded(connected.record->udid, preparation,
                                                             error, false, true);
        if (!connected.device)
            return std::nullopt;
    }
    transport::IoBudget budget({seconds(30), cancelled});
    auto stopped = [&] { return budget.stopped(); };
    ddi::CryptexInstallOptions cryptex_options;
    cryptex_options.timeout = seconds(30);
    cryptex_options.cancelled = stopped;
    const bool cryptex_available = connected.device &&
        connected.device->rsd().has_service("com.apple.security.cryptexd.remote");
    if (selected->kind == ddi::Kind::Cryptex && !cryptex_available) {
        error = "Cannot verify DDI installation: device does not advertise cryptexd";
        return std::nullopt;
    }
    std::optional<ddi::CryptexInstallResult> installed;
    auto query_cryptex = [&] {
        return !cryptex_available || ddi::query_installed_cryptex(*connected.device, installed,
                                                                cryptex_options, error);
    };
    if (!query_cryptex())
        return std::nullopt;
    auto open_mounter = [&] {
        return o.wifi.empty()
                   ? ddi::ServiceTransport::open_usb(*connected.lockdown, connected.record->device_id,
                                                     budget.remaining(), stopped, error)
                   : ddi::ServiceTransport::open_remote(*connected.device, budget.remaining(), stopped, error);
    };
    ddi::MountOptions options;
    options.timeout = seconds(30);
    options.cancelled = stopped;
    bool changed = false;
    if (remove && installed) {
        if (!ddi::remove_ddi_cryptex(*connected.device, cryptex_options, changed, error))
            return std::nullopt;
        installed.reset();
    }
    auto adapter = open_mounter();
    if (!adapter)
        return std::nullopt;
    auto wire = adapter->service();
    if (remove && !changed) {
        // An image-mounter DDI can remain even on a Cryptex-capable OS.
        if (!ddi::unmount_current(wire, selected->kind, options, error, &changed))
            return std::nullopt;
    }
    auto mounted = ddi::inspect_mounted(wire, selected->kind, options, error);
    if (!mounted)
        return std::nullopt;
    if (remove) {
        if (!query_cryptex())
            return std::nullopt;
        if (installed || mounted->state != ddi::MountState::NotMounted) {
            error = "DDI remains installed or mounted after removal";
            return std::nullopt;
        }
    }
    if (!budget.check(error, "DDI management"))
        return std::nullopt;
    Json result = {{"udid", o.serial},
                   {"os_version", connected.target.system_version},
                   {"kind", std::string(ddi::kind_name(selected->kind))},
                   {"image_type", mounted->image_type},
                   {"mounted", mounted->state != ddi::MountState::NotMounted},
                   {"verification", mounted->verification},
                   {"cryptex_installed", cryptex_available ? Json(bool(installed)) : Json(nullptr)},
                   {"cryptex_identifier", installed ? installed->identifier : std::string()},
                   {"cryptex_version", installed ? installed->version : std::string()}};
    if (remove)
        result["changed"] = changed;
    return result;
}
} // namespace xdb::server
