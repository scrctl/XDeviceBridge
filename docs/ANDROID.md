# Android host integration

English | [简体中文](ANDROID_zh.md)

## Recommended initial model

```text
Android app (UI / lifecycle / permissions)
  ├─ loads libxdb_server.so
  │    └─ xdb_server_start(0, app-private-files-dir)
  └─ MaaFramework / scrctl client
       └─ libxdb_client.so → same loopback protocol
```

An Android Service may host the native server if the embedding application's
lifecycle needs it. XDeviceBridge does not install a system daemon, start a Java
service or ask for permissions itself. Each application has a separate server and
private credential store. MaaFramework never receives device pairing keys.
The application's only storage responsibility is supplying an absolute private
persistent directory, e.g. a subdirectory of `Context.getFilesDir()`.

Use `xdb_server_port()` to configure clients. Local connections need no access
token; the server still owns the iPhone pairing record. When tearing down,
interrupt and join client requests, destroy clients, then stop the server. Server
stop joins workers; do not block the Android UI thread on device service deadlines.
The server library must remain loaded until all server workers have stopped.
Desktop automatic process startup is disabled on Android. If a client requests
shutdown, `xdb_server_is_running()` becomes false; the App still owns the handle
and must call `xdb_server_stop()` to join workers and free it.

A UI pairs by invoking the same `pair` protocol as `xdb pair --wifi`, displaying its
PIN event. The server handles the cryptography, reconnect verification and storage.
Standalone CLI use from adb shell is useful for diagnostics but does not prove
that an ordinary Android application can launch a native executable the same way.

## Why not just execute xdb from app storage?

Android 10+ restricts execution from an application's writable home directory for
apps targeting API 29+. Also, the current MaaFramework `ChildPipeIOStream` fallback
reports `Boost.Process is not available in this build` on Android.
The embedded-server C ABI avoids depending on that process path.

- [Android 10 execution restrictions](https://developer.android.com/about/versions/10/behavior-changes-10#execute-permission)
- [Android local-network permission](https://developer.android.com/privacy-and-security/local-network-permission)

The host application must provide Internet/LAN permissions appropriate to its
target SDK, request runtime local-network permission where required, handle
background execution, and arrange mDNS/multicast access for its device/network.
`ACCESS_LOCAL_NETWORK` applies to apps targeting Android 17/API 37+; native socket
code does not bypass Android permission policy. Wi-Fi is the initial transport;
Android USB Host access would require an explicit Android adapter.

## NDK build

Client only (no device dependencies):

```sh
cmake -S . -B build-android-client -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DXDB_BUILD_SERVER=OFF -DXDB_BUILD_CLI=OFF
cmake --build build-android-client
```

The arm64 dependency recipe is `tools/android-deps.sh <prefix>`; it builds pinned
OpenSSL/curl and HEVC/PNG FFmpeg libraries and prints server configure flags.
Audio/container support is disabled in that recipe. Supply target-built
OpenSSL/curl, pugixml and optional FFmpeg through
the NDK search roots/CMake package variables. Host pkg-config metadata is not used
when cross-compiling. HEVC needs avcodec/avutil/swscale; audio decoding separately
needs swresample. Using target libraries from a different architecture is invalid.
Android API 24 is the initial tested compile target (`getifaddrs` is needed by discovery).

## Performance and validation boundary

Decoded BGRA transfer is simple for MaaFramework, but a 1080×2400 frame is about
10.4 MB; 30 frames/s moves roughly 311 MB/s of pixel payload through loopback,
before additional copies. This needs device profiling. Latest-frame polling bounds
queues; encoded HEVC transport or shared-memory frames can be added later with
explicit negotiation rather than changing the meaning of BGRA responses.

Local cross-builds validate target compilation/linkage only. The NSD adapter and
embedded server have also been tested in scrctl-android on a Xiaomi 13 Ultra/API
36 with an existing paired iPhone; see [validation](VALIDATION.md). Other Android
versions and physical network switching are not covered by that result.
Existing MaaFramework Android Native integration supports
loading an application-supplied native library, which is a useful lifecycle precedent;
its `GetLockedPixels`/`DispatchInputMessage` ABI is not reused or modified here.

## Android NSD discovery (API 34+)

Compile `android/src/main/java/org/xdevicebridge/NsdDiscovery.java` into the host
application and include `android/consumer-rules.pro` in its R8 rules. After loading
`libxdb_server`, acquire `NsdDiscovery.acquire(applicationContext)` before starting
an embedded server. Close that lease after stopping its workers/clients. Multiple
servers in one application share one subscription; closing the last lease stops
it and clears the snapshot. The adapter retains the application Context only.
No Java runtime or Android permission is introduced into the client-only SDK.

Only `_remotepairing._tcp.` is browsed, on Wi-Fi/Ethernet networks. Android 14/API
34+ service-info callbacks update TXT bytes, port and all host addresses, and
remove lost services. Network loss removes its entries; interface metadata scopes
link-local IPv6 addresses. Warm discovery snapshots return immediately; a cold or
empty cache may wait up to the requested discovery timeout. The adapter has no
periodic scan/refresh timer; Android owns the subscription. Discovery is advisory,
not proof of pairing or connectivity. `NsdDiscovery.status()` exposes the current
advertisements and availability for diagnostics, never pairing credentials.

API 24–33, a missing adapter, missing permissions or failed NSD callbacks retain
the direct multicast backend. Keep the host's existing MulticastLock policy for
that fallback and pairing advertisements. App background/network lifecycle still
belongs to the host; xdb does not start a foreground Service or persist the cache.

Pairing advertisements continue to use xdb's direct backend. Public Android APIs
cannot set the SRV hostname to the session identifier required by this pairing
implementation; native registration success alone would not establish iPhone
compatibility. The service type and pairing identity are unchanged.

See [Android service-info subscriptions](https://developer.android.com/reference/android/net/nsd/NsdManager#registerServiceInfoCallback(android.net.nsd.NsdServiceInfo,%20java.util.concurrent.Executor,%20android.net.nsd.NsdManager.ServiceInfoCallback)).
