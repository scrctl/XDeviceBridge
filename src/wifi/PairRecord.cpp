#include "util/Hex.h"
#include "util/DataDirectory.h"
#include "wifi/PairRecord.h"

#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#include "wifi/PairRecordWindows.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <set>
#include <system_error>

#include "wifi/Tlv.h"

namespace xdb::wifi {
namespace {

constexpr char kHeader[] = "xdb-pair-record 1";
// 最多读取上限加一个检测字节，避免超大文件在大小校验前占用大量内存。
constexpr size_t kMaxRecordText = 16384;



std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/// 将外部提供的 UDID 转为文件名片段，仅保留 ASCII 字母和数字，其余替换为下划线。
/// 净化防止 UDID 引入目录跳转或路径分隔符，但不保证不同原始标识之间没有重名。
std::string sanitize(const std::string &udid) {
    std::string out;
    out.reserve(udid.size());
    for (const char c : udid) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9');
        out += ok ? c : '_';
    }
    return out;
}

/// 随机后缀临时文件与目标位于同目录，成功关闭后通过 rename 替换。
/// POSIX 路径不调用 fsync，成功返回不能作为断电后持久性的保证。
bool write_file(const std::string &path, std::string_view text, std::string &err, bool replace) {
    const auto nonce = random_bytes(8, err);
    if (!nonce) return false;
    const std::string tmp = path + ".tmp." + util::hex_encode(*nonce);
#ifdef _WIN32
    return write_private_record(tmp, path, text, err, replace);
#else
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    std::FILE *f = fd < 0 ? nullptr : ::fdopen(fd, "wb");
    if (f == nullptr) {
        if (fd >= 0) { ::close(fd); ::remove(tmp.c_str()); }
        err = "Cannot open temporary file " + tmp;
        return false;
    }
    // 在写入含私钥的记录前将临时文件权限设为 0600；设置失败即关闭并移除文件。
    if (::fchmod(::fileno(f), 0600) != 0) {
        err = "Failed to set record file permissions";
        std::fclose(f);
        ::remove(tmp.c_str());
        return false;
    }
    const size_t written = std::fwrite(text.data(), 1, text.size(), f);
    const int closed = std::fclose(f);
    if (written != text.size() || closed != 0) {
        err = "Incomplete record file write";
        ::remove(tmp.c_str());
        return false;
    }
    // Publish exclusively without exposing an incomplete record. Android's
    // app SELinux policy forbids hard links even inside its private data dir.
    // renameat2 preserves no-replace semantics, including existing symlinks;
    // use the syscall directly so this works below Android API 30 as well.
#ifdef __linux__
    const auto published = replace ? std::rename(tmp.c_str(), path.c_str())
        : ::syscall(SYS_renameat2, AT_FDCWD, tmp.c_str(), AT_FDCWD, path.c_str(), RENAME_NOREPLACE);
#else
    const auto published = replace ? std::rename(tmp.c_str(), path.c_str())
        : ::link(tmp.c_str(), path.c_str());
#endif
    if (published != 0) {
        const int code = errno;
        err = !replace && code == EEXIST
            ? "Pairing record already exists; Wi-Fi pairing will not replace it"
            : "Cannot publish pairing record: " + std::error_code(code, std::generic_category()).message();
        ::remove(tmp.c_str());
        return false;
    }
    if (!replace) ::remove(tmp.c_str());
    return true;
#endif
}

std::optional<std::string> read_file(const std::string &path, std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "Cannot open " + path;
        return std::nullopt;
    }
    std::string text(kMaxRecordText + 1, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto count = static_cast<size_t>(in.gcount());
    if (in.bad() || (in.fail() && !in.eof())) {
        err = "Failed to read pairing record file " + path;
        return std::nullopt;
    }
    if (count > kMaxRecordText) {
        err = "Pairing record file too large";
        return std::nullopt;
    }
    text.resize(count);
    return text;
}

}  // namespace

bool valid_peer_identifier(std::string_view identifier) {
    return !identifier.empty() && identifier.size() <= kMaxPeerIdentifier && trim(identifier) == identifier &&
           std::none_of(identifier.begin(), identifier.end(), [](unsigned char c) {
               return c < 0x20 || c == 0x7f;
           });
}

std::string format_record(const PairRecord &record) {
    std::ostringstream out;
    out << kHeader << '\n';
    out << "udid=" << record.udid << '\n';
    out << "host_identifier=" << record.host_identifier << '\n';
    out << "host_private_key=" << util::hex_encode(record.host_private_key) << '\n';
    out << "host_public_key=" << util::hex_encode(record.host_public_key) << '\n';
    if (!record.advertised_identifier.empty()) {
        out << "advertised_identifier=" << record.advertised_identifier << '\n';
    }
    if (!record.peer_identifier.empty()) {
        out << "peer_identifier=" << record.peer_identifier << '\n';
    }
    if (!record.peer_public_key.empty()) {
        out << "peer_public_key=" << util::hex_encode(record.peer_public_key) << '\n';
    }
    if (!record.peer_alt_irk.empty()) {
        out << "peer_alt_irk=" << util::hex_encode(record.peer_alt_irk) << '\n';
    }
    return out.str();
}

std::optional<PairRecord> parse_record(std::string_view text, std::string &err) {
    PairRecord record;
    bool first = true;
    bool seen_private = false;
    std::set<std::string_view> seen_fields;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            nl = text.size();
        }
        const std::string_view line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty()) {
            continue;
        }
        if (first) {
            first = false;
            if (line != kHeader && line != "scrctl-pair-record 1") {
                err = "Invalid xdb pairing record header";
                return std::nullopt;
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            err = "Record line is not key=value";
            return std::nullopt;
        }
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        const bool known = key == "udid" || key == "host_identifier" ||
                           key == "host_private_key" || key == "host_public_key" ||
                           key == "advertised_identifier" || key == "peer_identifier" ||
                           key == "peer_public_key" || key == "peer_alt_irk";
        if (known && !seen_fields.insert(key).second) {
            err = "Duplicate pairing record field: " + std::string(key);
            return std::nullopt;
        }
        auto set_hex = [&](Bytes &field, size_t want) -> bool {
            const std::optional<Bytes> bytes = util::hex_decode(value, err);
            if (!bytes) {
                return false;
            }
            if (bytes->size() != want) {
                err = "Incorrect field length: " + std::string(key) + " requires " + std::to_string(want) +
                      " bytes; got " + std::to_string(bytes->size());
                return false;
            }
            field = *bytes;
            return true;
        };
        if (key == "udid") {
            record.udid = value;
        } else if (key == "host_identifier") {
            record.host_identifier = value;
        } else if (key == "host_private_key") {
            if (!set_hex(record.host_private_key, 32)) {
                return std::nullopt;
            }
            seen_private = true;
        } else if (key == "host_public_key") {
            if (!set_hex(record.host_public_key, 32)) {
                return std::nullopt;
            }
        } else if (key == "advertised_identifier") {
            record.advertised_identifier = value;
        } else if (key == "peer_identifier") {
            if (!valid_peer_identifier(value)) {
                err = "Pairing record peer_identifier must be nonempty single-line text";
                return std::nullopt;
            }
            record.peer_identifier = value;
        } else if (key == "peer_public_key") {
            if (!set_hex(record.peer_public_key, 32)) {
                return std::nullopt;
            }
        } else if (key == "peer_alt_irk") {
            if (!set_hex(record.peer_alt_irk, 16)) {
                return std::nullopt;
            }
        }
        // 忽略未知键，允许新增可选字段后的记录继续由旧版本解析。
    }
    if (!seen_private) {
        err = "Record missing host_private_key";
        return std::nullopt;
    }
    return record;
}

namespace {
bool save_record_impl(const std::string &path, const PairRecord &record, std::string &err, bool replace) {
    err.clear();
    const auto parent = std::filesystem::path(path).parent_path();
    const std::string dir = parent.string();
    if (!dir.empty()) {
        // 创建缺失的父目录；任何目录创建错误都在写入记录前返回。
        std::error_code ec;
        const bool existed = std::filesystem::is_directory(dir, ec);
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            err = "Failed to create directory " + dir + ": " + ec.message();
            return false;
        }
        if (!existed) {
            // 只限制新建的叶子目录。共享父目录及已有目录的权限由用户管理。
#ifdef _WIN32
            if (!protect_record_directory(dir, err)) return false;
#else
            std::filesystem::permissions(dir, std::filesystem::perms::owner_all, ec);
            if (ec) {
                err = std::string("Failed to set record file permissions") + ": " + ec.message();
                return false;
            }
#endif
        }
    }
    return write_file(path, format_record(record), err, replace);
}
} // namespace

bool save_record(const std::string &path, const PairRecord &record, std::string &err) {
    return save_record_impl(path, record, err, true);
}

bool save_record_new(const std::string &path, const PairRecord &record, std::string &err) {
    return save_record_impl(path, record, err, false);
}

std::optional<PairRecord> load_record(const std::string &path, std::string &err) {
    const std::optional<std::string> text = read_file(path, err);
    if (!text) {
        return std::nullopt;
    }
    return parse_record(*text, err);
}

std::string default_record_dir() {
    return util::default_data_directory();
}

std::string record_path(const std::string &dir, const std::string &udid) {
    return dir + "/remote-" + sanitize(udid) + ".pair";
}

std::vector<std::string> list_record_udids(const std::string &dir, std::string &err) {
    std::vector<std::string> out;
    static constexpr std::string_view kPrefix = "remote-";
    static constexpr std::string_view kSuffix = ".pair";
    std::error_code ec;
    const std::filesystem::path root(dir);
    if (!std::filesystem::is_directory(root, ec)) {
        // 未识别为目录时统一返回空列表，包括不存在和 is_directory 查询失败。
        // 当前接口在此清除 err，调用方无法用该结果区分不存在与查询失败。
        err.clear();
        return out;
    }
    for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) {
            err = "Reading directory " + dir + " failed: " + ec.message();
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.size() <= kPrefix.size() + kSuffix.size() ||
            name.compare(0, kPrefix.size(), kPrefix) != 0 ||
            name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
            continue;
        }
        out.push_back(
            name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace xdb::wifi
