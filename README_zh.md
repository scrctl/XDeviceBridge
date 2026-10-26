# XDeviceBridge（`xdb`）

[English](README.md) | 简体中文

面向 iPhone/iPad 的跨平台库和命令行工具，提供设备发现、配对、开发者镜像准备、
截图、视频取帧和输入控制。已实现的服务不需要越狱、WebDriverAgent 或在手机上
安装自动化应用。

1.0 将公开 C SDK（ABI 1）和本机 RPC 协议 1 确立为稳定接入基线。
scrctl 和 MaaFramework 使用轻量客户端，设备服务运行在 xdb Server 中。
机型与系统支持仍取决于具体服务；测试范围见[验证说明](docs/VALIDATION.md)，
版本变化见[变更记录](CHANGELOG.md)。

## 架构

```text
xdb CLI / scrctl / MaaFramework
              |
     libxdb_client（C API）
              |
          本机回环协议
              |
           xdb Server
              |
      USB / 已配对的 Wi-Fi 设备
```

客户端只链接套接字、JSON 和平台运行库。OpenSSL、DDI、媒体解码和设备协议实现
均位于服务端。桌面端可以运行独立的 Server 进程，也可以由应用加载 `libxdb_server`。
Android 应用可在原生线程中启动同一服务端，客户端仍使用相同协议。

配对时，手机上显示的主机名称为 `xdb - <主机名>`，Android 使用设备型号代替主机名；
型号为 `xdb - <硬件型号>`，显示序列号为稳定的 `XDB-<16 位十六进制字符>`。
这些显示信息不替代实际的加密配对身份。

设备配对凭据由 Server 保存，API 返回 UDID，不返回私密配对材料。
每个 Android 应用自行指定私有数据目录，不使用跨应用共享的系统服务或凭据存储。

## 安装

参见[安装说明](docs/INSTALL_zh.md)，包括：

- macOS/Linux 用户目录安装及 PATH 设置。
- Windows 解压安装及用户 PATH 设置。
- 运行包与对应源码包的区别。
- scrctl、MaaFramework 的 SDK 如何查找已安装的 xdb。
- 更新、卸载及从源码构建安装包。

安装后可直接运行：

```sh
xdb devices -l
xdb pair --wifi
```

## 构建

服务端需要 C++20（包括 `std::jthread` 和 `std::stop_token`）、CMake >= 3.20、
OpenSSL 和 curl。未提供本地依赖时，nlohmann-json、pugixml、lwIP 和 mDNS
使用固定版本的源码。FFmpeg 视频解码为可选项，Apple 平台还支持 VideoToolbox；
FFmpeg 音频解码另外需要 libswresample。项目不依赖 SDL、MaaUtils 或 OpenCV。

macOS 的设备发现和临时 Wi-Fi 配对广播优先使用系统 Bonjour，无法打开时回退到
直接 mDNS。发现超时仍用于收集一次新快照；这一步尚未增加 Server 持久设备缓存，
也不代表已经验证设备可连接。

Linux 构建时若有 Avahi 开发头文件（Debian/Ubuntu 的 `libavahi-client-dev`），
也会优先通过系统 Avahi 服务发现设备和广播配对主机。服务端按需加载 Avahi 客户端库；
库、后台服务或系统总线不可用时回退到直接 mDNS。仅构建客户端无需 Avahi，
xdb 不修改系统 mDNS 配置，也不要求 root 权限。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

下游应用也可以只构建轻量客户端：

```sh
cmake -S . -B build-client -DXDB_BUILD_SERVER=OFF -DXDB_BUILD_CLI=OFF
cmake --build build-client
```

NDK 和内嵌服务端的接入方式见 [Android 集成说明](docs/ANDROID_zh.md)。
C ABI 定义位于 [xdb.h](include/xdb/xdb.h)，通信协议见[通信协议说明](docs/PROTOCOL_zh.md)。

1.0 的完整接口约定见 [RPC API](docs/API_zh.md) 与 [C SDK](docs/C_API_zh.md)，
包括字段、默认值、错误码、状态前提、内存所有权、并发取消规则和示例。
已发布版本遵守[兼容性策略](docs/PROTOCOL_zh.md#版本与升级策略)。

通过 `add_subdirectory` 嵌入时默认不下载依赖。调用方可提供
`nlohmann_json::nlohmann_json`（>= 3.12.0）、已安装依赖或本地 FetchContent 源码覆盖；
需要自动下载时须显式开启 `XDB_FETCH_DEPENDENCIES=ON`。独立构建仍默认下载固定版本依赖，
也可用 `XDB_FETCH_DEPENDENCIES=OFF` 强制离线解析。

## 命令行使用

运行 `xdb` 或 `xdb --help` 查看命令速查。各子命令提供参数说明和示例，
例如 `xdb pair --help`、`xdb ddi prepare --help`。查看帮助不会启动或连接 Server。

桌面端 CLI 和 SDK 会在需要时自动启动后台 Server。客户端退出后，Server 继续运行，
供其他客户端复用。它只监听本机回环地址，默认端口为 `17423`；本机客户端不需要访问令牌。
服务不按本机进程或操作系统用户鉴权。能够连接该端口的本机进程可控制设备、访问剪贴板
或停止服务；手机配对不会区分这些客户端。

```sh
# 需要时自动启动后台 Server。
xdb devices -l
xdb pair --wifi
# 在手机的开发者模式配对页面选择广播出的电脑，
# 然后在手机输入 xdb 显示的一次性配对码。
xdb pair --usb
xdb -s '<UDID>' ddi prepare --wifi
xdb -s '<UDID>' screenshot screen.png --wifi
```

`devices` 为每个发现的设备输出一行；`devices -l` 额外显示名称、地址、网络接口以及
配对和连接详情。文本输出省略空名称和空端点字段，候选端点的配对原因仅在与设备
汇总状态不同时显示。`--json` 保留完整的结构化响应。设备状态的含义如下：

| 状态 | 含义 |
| --- | --- |
| `discovered` | 已通过 USB/mDNS 发现，但未匹配到已验证的本地 Wi-Fi 配对记录 |
| `paired` | 完整的本地凭据匹配到了 Wi-Fi 广播，但尚未验证能否连接 |
| `connected` | Server 当前持有已认证的设备会话，具体服务仍可能失败 |

列举设备不会主动连接。`pairing:record_present` 不代表手机接受了新的连接，
USB 枚举成功也不代表已获得信任。无法识别身份的设备显示为 `(unidentified)`。

对于 `screenshot` 和 `ddi prepare`，发现结果中只有一台已识别设备时可省略 `-s`。
`--usb` / `--wifi` 限制发现所用的传输方式，指定 Wi-Fi 地址还会进一步筛选候选项。
同一 UDID 的多个地址只算一台设备。存在多台设备（包括身份未知的设备）时需要明确选择；
身份未知的设备应先完成配对。

使用 `xdb start-server` 显式启动或复用后台 Server，使用 `xdb kill-server`
停止 Server 并取消其会话。Server 不存在时，`kill-server` 直接成功，不会将其启动。
`xdb server` 可用于前台诊断。

默认配对数据目录：macOS 为 `~/Library/Application Support/XDeviceBridge`，
Windows 为 `%LOCALAPPDATA%/XDeviceBridge`，Linux 为
`${XDG_DATA_HOME:-~/.local/share}/XDeviceBridge`。CLI 与底层配对的默认规则一致。
`--data-dir /absolute/path` 在启动新 Server 时指定数据目录，不会改变已有 Server
使用的目录。默认采用平台用户数据目录。`start-server` / `kill-server` 需要固定端口；
诊断时可用 `server --port 0` 自动分配临时端口。

桌面 SDK 会在客户端库所在目录、相邻的 `bin` 目录或 `PATH` 中查找 `xdb`。
也可通过 `XDB_EXECUTABLE` 指定可执行文件路径，适用于分开的开发构建目录。
客户端进程不加载服务端后端。Android 应用仍由自身启动内嵌 Server。

不带地址的 `--wifi` 会发现已配对设备；`--wifi ADDRESS` 则明确指定 RemotePairing 地址。
未限定传输方式时，设备会话优先尝试 USB，再回退到已配对的 Wi-Fi。
USB 需要系统 usbmux/Apple 移动设备服务正常运行，并在手机上授予信任。
Android 初期支持 Wi-Fi，尚未实现 Android USB Host 接入。

协议开发可使用 `xdb rpc METHOD JSON_PARAMS` 和 `xdb rpc-shell` 发送原始请求。
`rpc-shell` 会保持客户端会话，但它不是 iOS 系统命令行。

## 当前能力与限制

- 设备发现、USB/Wi-Fi 配对，以及 DDI 下载和准备。
- 每个客户端连接持有一个设备会话；同一已知 UDID 同时只有一个连接拥有其会话。
- PNG 截图，以及解码后的最新 BGRA 视频帧和显示几何信息。
- 按面板归一化坐标执行单点触控，支持键盘用法码、ASCII 文本、媒体按键和应用控制。
- 二进制媒体与 JSON 分帧传输，不使用 Base64；每条连接一次只处理一个待响应请求。
  视频取帧返回最新帧或更新的帧，不逐帧排队。
- 报告传输失败时，某些操作可能已经改变了手机状态。结果不明确时，不应自动重试输入、
  配对或镜像替换操作。
- 通过断开客户端或停止 Server 取消操作。已有设备服务调用可能仍要等自身截止时间，
  不保证所有服务都能立即取消。
- 已通过 RPC 提供 PCM 音频传输，以及服务端原始 HEVC / MP4 / MKV 录制；
  面向客户端的编码 HEVC/AAC 传输仍待后续接入。
- 交叉编译成功不代表 Android 应用打包、生命周期、权限或真机行为已经验证。
  详见[验证说明](docs/VALIDATION.md)。

## 许可证

项目自有代码采用 **LGPL-3.0-only**。
参见 [LICENSE](LICENSE) 和 [COPYING.GPLv3](COPYING.GPLv3)。
第三方组件保留原许可证；分发二进制的许可条件还取决于实际依赖构建，尤其是 FFmpeg。
详见[第三方组件说明](THIRD_PARTY_NOTICES.md)。
