// SPDX-License-Identifier: LGPL-3.0-only
#include "cli/Help.h"
#include "xdb/version.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace xdb::cli {
namespace {
std::filesystem::path executable_path() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    auto size = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    return size && size < buffer.size()
               ? std::filesystem::path(std::wstring(buffer.data(), size)) : std::filesystem::path();
#elif defined(__APPLE__)
    uint32_t size = 1024;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    }
    return std::filesystem::path(buffer.data());
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error);
#endif
}
}

void print_version() {
    std::cout << "XDeviceBridge " XDB_VERSION_STRING "\n";
    auto path = executable_path();
    if (!path.empty()) {
        std::error_code error;
        auto resolved = std::filesystem::weakly_canonical(path, error);
        auto text = (error ? path : resolved).u8string();
        std::cout << "Installed as " << std::string(text.begin(), text.end()) << '\n';
    }
#ifdef _WIN32
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    const char *arch = info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64"
                       : info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x86_64"
                       : info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? "x86" : "unknown";
    std::cout << "Running on Windows (" << arch << ")\n";
#else
    utsname info{};
    if (uname(&info) == 0)
        std::cout << "Running on " << info.sysname << ' ' << info.release << " (" << info.machine << ")\n";
#endif
}
}
