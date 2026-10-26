# Android 宿主接入

[English](ANDROID.md) | 简体中文

## 推荐的初期模型

```text
Android app (UI / lifecycle / permissions)
  ├─ loads libxdb_server.so
  │    └─ xdb_server_start(0, app-private-files-dir)
  └─ MaaFramework / scrctl client
       └─ libxdb_client.so → same loopback protocol
```

如应用生命周期需要，可由 Android Service 承载原生 Server。
XDeviceBridge 本身不安装系统守护进程、不启动 Java 服务，也不自行申请权限。
每个应用使用独立 Server 和私有凭据存储；MaaFramework 不接收设备配对密钥。
应用只需提供绝对、私有、持久的数据目录，例如 `Context.getFilesDir()` 下的子目录。

通过 `xdb_server_port()` 为客户端配置端口。本机连接不需要访问令牌，
iPhone 配对记录仍由 Server 持有。结束时，先中断并等待客户端请求，
销毁客户端，再停止 Server。停止服务会等待工作线程；
不要让 Android UI 线程等待设备服务的截止时间。
在全部服务端工作线程停止前，服务库必须保持加载状态。

Android 禁用桌面式自动进程启动。若客户端请求关闭服务，
`xdb_server_is_running()` 会变为 false；App 仍持有句柄，
须调用 `xdb_server_stop()` 等待工作线程并释放它。

UI 通过调用与 `xdb pair --wifi` 相同的 `pair` 协议配对，并显示 PIN 事件。
Server 负责加密、重连验证和存储。
从 adb shell 运行独立 CLI 可用于诊断，但不能据此证明普通 Android 应用
也能以相同方式启动原生可执行文件。

## 为什么不直接执行应用目录中的 xdb？

对目标 API 29+ 的应用，Android 10+ 限制执行应用可写主目录中的文件。
当前 MaaFramework 的 `ChildPipeIOStream` 回退在 Android 上也会报告
`Boost.Process is not available in this build`。
内嵌 Server C ABI 不依赖这条进程启动路径。

- [Android 10 执行限制](https://developer.android.com/about/versions/10/behavior-changes-10#execute-permission)
- [Android 本地网络权限](https://developer.android.com/privacy-and-security/local-network-permission)

宿主应用须按目标 SDK 提供 Internet／局域网权限，在需要时申请运行时本地网络权限，
处理后台执行，并为设备和网络安排 mDNS／多播访问。
`ACCESS_LOCAL_NETWORK` 适用于目标 Android 17/API 37+ 的应用；
原生套接字代码不能绕过 Android 权限策略。
初期传输方式是 Wi-Fi；Android USB Host 访问需要显式实现 Android 适配层。

## NDK 构建

仅客户端，无设备后端依赖：

```sh
cmake -S . -B build-android-client -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DXDB_BUILD_SERVER=OFF -DXDB_BUILD_CLI=OFF
cmake --build build-android-client
```

arm64 依赖配方为 `tools/android-deps.sh <prefix>`，构建固定版本的
OpenSSL/curl 和 HEVC/PNG FFmpeg 库，并输出 Server 配置参数。
这份配方禁用音频和容器支持。
通过 NDK 搜索根和 CMake 包变量提供目标平台的 OpenSSL/curl、pugixml 及可选 FFmpeg。
交叉编译不使用宿主 pkg-config 元数据。
HEVC 需要 avcodec/avutil/swscale，音频解码还需 swresample。
不能使用其他架构的目标库。初期已验证的编译目标为 Android API 24
（设备发现需要 `getifaddrs`）。

## 性能与验证范围

解码后传输 BGRA 便于 MaaFramework 使用，但一张 1080×2400 帧约为 10.4 MB；
30 帧／秒会让约 311 MB/s 的像素负载经过回环，还不包括额外复制。
需要在实际设备上测量。最新帧轮询限制队列长度；
未来若增加编码 HEVC 或共享内存帧，应显式协商，不能改变已有 BGRA 响应的含义。

本地交叉构建只验证目标编译和链接。
NSD 适配和内嵌 Server 已通过 scrctl-android，在 Xiaomi 13 Ultra／API 36 上使用
既有配对的 iPhone 验证，详见 [验证记录](VALIDATION.md)。该结果不覆盖其他 Android
版本或实际网络切换。
MaaFramework 已有的 Android Native 接入支持加载应用提供的原生库，
可作为生命周期组织的参考；这里不复用或修改其 `GetLockedPixels`／`DispatchInputMessage` ABI。

## Android NSD 发现（API 34+）

宿主应用编译 `android/src/main/java/org/xdevicebridge/NsdDiscovery.java`，并在 R8
配置中包含 `android/consumer-rules.pro`。加载 `libxdb_server` 后，在启动内嵌服务前调用
`NsdDiscovery.acquire(applicationContext)`；客户端和 Server 工作线程停止后关闭返回的
租约。同一应用的多个 Server 共用一个订阅，最后一个租约关闭时停止订阅并清空快照，
仅持有 Application Context。纯客户端 SDK 不增加 Java 或 Android 权限依赖。

只订阅 Wi-Fi／以太网上的 `_remotepairing._tcp.`。Android 14／API 34 起使用
service-info 回调更新 TXT 原始字节、端口和所有主机地址，移除已消失的服务；网络消失时
清除该网络条目，用本机接口元数据为 IPv6 链路本地地址补上作用域。已有快照立即返回，
首次发现或空缓存最多等待调用方指定的期限。适配层没有定时扫描／刷新任务，由系统维护
订阅。发现结果不是配对或连接成功的证明。`NsdDiscovery.status()` 可查看广播快照和
可用状态，不含配对凭据。

API 24–33、未加载适配层、权限不足或 NSD 回调失败时保留直接组播后端。宿主仍应保留
现有 MulticastLock 策略，供回退和配对广播使用。后台和网络生命周期仍由 App 管理；
xdb 不启动前台 Service，不持久化发现缓存。

配对广播继续使用 xdb 的直接后端：Android 公开 API 无法将 SRV 主机名设为配对会话
标识，单凭系统注册成功不足以证明 iPhone 配对兼容。服务类型和配对身份保持不变。

参见 [Android service-info 订阅接口](https://developer.android.com/reference/android/net/nsd/NsdManager#registerServiceInfoCallback(android.net.nsd.NsdServiceInfo,%20java.util.concurrent.Executor,%20android.net.nsd.NsdManager.ServiceInfoCallback))。
