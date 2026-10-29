// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include <string>
#include <string_view>

namespace xdb::transport {
// Classify device responses at the protocol boundary, never by matching a rendered diagnostic.
enum class DeviceError { other, not_paired, unauthorized, locked };

inline DeviceError lockdown_device_error(std::string_view value) {
    if (value == "PasswordProtected" || value == "DeviceLocked") return DeviceError::locked;
    if (value == "InvalidHostID" || value == "InvalidPairRecord" || value == "UserDeniedPairing" ||
        value == "UserDenied" || value == "PairingDialogResponsePending") return DeviceError::unauthorized;
    return DeviceError::other;
}
inline const char *device_error_code(DeviceError value) {
    switch (value) {
    case DeviceError::not_paired: return "not_paired";
    case DeviceError::unauthorized: return "unauthorized";
    case DeviceError::locked: return "device_locked";
    default: return "operation_failed";
    }
}
inline std::string device_error_hint(DeviceError value, bool wifi) {
    if (value == DeviceError::locked)
        return " (device is locked; unlock it and keep the screen awake before retrying)";
    if (value == DeviceError::unauthorized && !wifi)
        return " (device unauthorized; unlock it and confirm Trust on the device. Once USB trust is established, run xdb -d pair)";
    if (value == DeviceError::unauthorized)
        return " (device unauthorized; pairing may have been removed. Run xdb -e pair to pair again)";
    if (value == DeviceError::not_paired)
        return wifi ? " (run xdb -e pair to create a Wi-Fi pairing record)"
                    : " (unlock the device and confirm Trust, then run xdb -d pair)";
    return {};
}
inline std::string lockdown_rejection(std::string operation, std::string_view response,
                                      DeviceError *failure = nullptr) {
    const auto kind = lockdown_device_error(response);
    if (failure) *failure = kind;
    return operation + " rejected: " + std::string(response) + device_error_hint(kind, false);
}
} // namespace xdb::transport
