// SPDX-License-Identifier: LGPL-3.0-only
#include "Process.h"
#include "transport/IoBudget.h"
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#elif !defined(__ANDROID__)
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

namespace xdb::client {
namespace {
#ifndef __ANDROID__
namespace fs = std::filesystem;
const char module_anchor = 0;
fs::path from_utf8(std::string_view value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}
std::string utf8(const fs::path &path) {
    auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}
fs::path module_directory() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&module_anchor), &module))
        return {};
    std::vector<wchar_t> path(32768);
    auto size = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
    return size && size < path.size() ? fs::path(std::wstring(path.data(), size)).parent_path() : fs::path();
#else
    Dl_info info{};
    return dladdr(&module_anchor, &info) && info.dli_fname
               ? fs::absolute(info.dli_fname).parent_path() : fs::path();
#endif
}
bool executable(const fs::path &path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error))
        return false;
#ifdef _WIN32
    return true;
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}
fs::path find_executable() {
    if (auto *value = std::getenv("XDB_EXECUTABLE"); value && *value) {
        auto path = fs::absolute(from_utf8(value));
        return executable(path) ? path : fs::path();
    }
#ifdef _WIN32
    const auto name = "xdb.exe";
    const char separator = ';';
#else
    const auto name = "xdb";
    const char separator = ':';
#endif
    auto directory = module_directory();
    if (!directory.empty()) {
        for (const auto &path : {directory / name, directory.parent_path() / "bin" / name})
            if (executable(path))
                return path;
    }
    const auto *value = std::getenv("PATH");
    std::string paths = value ? value : "";
    for (size_t start = 0; start < paths.size();) {
        auto end = paths.find(separator, start);
        auto part = paths.substr(start, end - start);
        if (!part.empty()) {
            auto path = fs::absolute(from_utf8(part) / name);
            if (executable(path))
                return path;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return {};
}
#ifdef _WIN32
std::wstring quote(const std::string &argument) {
    auto value = from_utf8(argument).wstring();
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (auto ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (ch == L'"')
            result += L'\\';
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
bool spawn(const std::vector<std::string> &arguments, std::string &error, const transport::IoBudget *budget) {
    if (budget && !budget->check(error, "xdb startup")) return false;
    std::wstring command;
    for (const auto &argument : arguments) {
        if (!command.empty())
            command += L' ';
        command += quote(argument);
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE null = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (null == INVALID_HANDLE_VALUE) {
        error = "Cannot open NUL for xdb server";
        return false;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = null;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    bool initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) != 0;
    bool configured = initialized && UpdateProcThreadAttribute(startup.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &null, sizeof(null), nullptr, nullptr);
    PROCESS_INFORMATION process{};
    bool ok = configured && CreateProcessW(from_utf8(arguments[0]).c_str(), command.data(), nullptr,
        nullptr, TRUE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr, &startup.StartupInfo, &process);
    auto code = GetLastError();
    if (initialized)
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(null);
    if (!ok) {
        error = "Cannot start xdb server (Windows error " + std::to_string(code) + ")";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
#else
bool spawn(const std::vector<std::string> &arguments, std::string &error, const transport::IoBudget *budget) {
    std::vector<char *> argv;
    for (const auto &argument : arguments)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        error = std::strerror(errno);
        return false;
    }
    // Keep the startup pipe above stdio even in a host with closed standard descriptors.
    int input = fcntl(pipe_fds[0], F_DUPFD_CLOEXEC, 3);
    int output = fcntl(pipe_fds[1], F_DUPFD_CLOEXEC, 3);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    if (input < 0 || output < 0) {
        if (input >= 0) close(input);
        if (output >= 0) close(output);
        error = "Cannot create xdb startup pipe";
        return false;
    }
    auto max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0) {
        close(input);
        close(output);
        error = "Cannot determine the descriptor limit for xdb startup";
        return false;
    }
    pid_t child = fork();
    if (child == 0) {
        // The SDK may be used in a multithreaded host: only async-signal-safe work before exec.
        auto fail = [](int fd) {
            int code = errno;
            (void)write(fd, &code, sizeof(code));
            _exit(127);
        };
        close(input);
        if (setsid() < 0) fail(output);
        auto daemon = fork();
        if (daemon < 0) fail(output);
        if (daemon > 0) _exit(0);
        int null = open("/dev/null", O_RDWR);
        if (null < 0) fail(output);
        for (int fd = 0; fd < 3; ++fd)
            if (dup2(null, fd) < 0) fail(output);
        if (null > 2) close(null);
        if (output != 3 && dup2(output, 3) < 0) fail(output);
        if (fcntl(3, F_SETFD, FD_CLOEXEC) < 0) fail(3);
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 4u, ~0u, 0u) != 0)
#endif
            for (long fd = 4; fd < max_fd; ++fd) close(int(fd));
        execv(argv[0], argv.data());
        fail(3);
    }
    int code = errno;
    close(output);
    if (child < 0) {
        close(input);
        error = std::strerror(code);
        return false;
    }
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    pollfd fd{input, POLLIN, 0};
    int ready;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (budget && !budget->check(error, "xdb startup")) {
            close(input);
            return false;
        }
        ready = poll(&fd, 1, 20);
    } while ((ready == 0 || (ready < 0 && errno == EINTR)) &&
             std::chrono::steady_clock::now() < deadline);
    ssize_t size = ready > 0 ? read(input, &code, sizeof(code)) : -1;
    close(input);
    if (size == 0)
        return true;
    error = size == static_cast<ssize_t>(sizeof(code)) ? std::strerror(code) : "Timed out starting xdb server";
    return false;
}
#endif
#endif
} // namespace
bool launch_server(uint16_t port, const char *directory, std::string &error, const transport::IoBudget *budget) {
#ifdef __ANDROID__
    (void)port;
    (void)directory;
    (void)budget;
    error = "Start the embedded xdb Server in the Android app before connecting";
    return false;
#else
    if (budget && !budget->check(error, "xdb startup")) return false;
    auto path = find_executable();
    if (path.empty()) {
        error = "Cannot find xdb executable; install xdb on PATH or set XDB_EXECUTABLE to its path";
        return false;
    }
    std::vector<std::string> arguments{utf8(path), "server", "--port", std::to_string(port)};
    if (directory && *directory) {
        arguments.push_back("--data-dir");
        arguments.emplace_back(directory);
    }
    return spawn(arguments, error, budget);
#endif
}
} // namespace xdb::client
