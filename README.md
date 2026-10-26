# XDeviceBridge (`xdb`)

English | [简体中文](README_zh.md)

A cross-platform library and command-line bridge for iPhone/iPad device discovery,
pairing, developer-image preparation, capture and input. No jailbreak, WebDriverAgent
or on-device automation application is required for the implemented services.

Version 1.0 establishes the public C SDK (ABI 1) and local RPC protocol 1 as the
stable integration baseline. scrctl and MaaFramework use the lightweight client;
device services run in xdb Server. Device and OS support remains service-dependent;
see the [validation boundaries](docs/VALIDATION.md) and [changelog](CHANGELOG.md).

## Architecture

```text
xdb CLI / scrctl / MaaFramework
              |
     libxdb_client (C API)
              |
    loopback protocol
              |
           xdb Server
              |
     USB / paired Wi-Fi device
```

The client links only sockets, JSON and platform runtime libraries. OpenSSL,
DDI, media decoding and device protocol implementations stay inside the server.
The server runs as a standalone process on desktop or as `libxdb_server` hosted by
an application. Android applications can start the same server on a native thread;
the client still uses the same protocol.

Pairing displays the host as `xdb - <hostname>` (Android uses its device model),
the model as `xdb - <hardware model>`, and a stable display serial `XDB-<16 hex digits>`.
These labels do not replace the cryptographic pairing identity.

The server owns device pairing records. Its API returns a UDID, never the private
pairing material. Each Android application supplies its own private data directory;
there is no shared system daemon or cross-application credential store.

## Install

See [installation and desktop packages](docs/INSTALL.md) for user-local installation,
PATH setup, runtime/source archives and downstream SDK lookup.

## Build

C++20 (including `std::jthread`/`std::stop_token`), CMake >= 3.20, OpenSSL and curl
are required for the server. nlohmann-json, pugixml, lwIP and mDNS use pinned source
versions when not supplied locally. FFmpeg video decoding is optional; Apple also
has VideoToolbox. FFmpeg audio decoding additionally needs libswresample.
There is no SDL, MaaUtils or OpenCV dependency.

On macOS, device discovery and temporary Wi-Fi pairing advertisements use the
system Bonjour service. If it cannot be opened, xdb falls back to direct mDNS.
The discovery timeout still bounds a fresh snapshot; this does not add a
persistent server-side device cache or verify device connectivity.

Linux builds with Avahi development headers (`libavahi-client-dev` on Debian/
Ubuntu) also use the system Avahi service for discovery and pairing advertisements.
The server loads the Avahi client library only when needed; a missing library,
daemon or system-bus connection falls back to direct mDNS. Client-only builds do
not need Avahi. xdb does not change system mDNS configuration or require root.

After rebuilding the server, restart an already running daemon with
`./build/xdb kill-server` and `./build/xdb start-server` before testing. Replacing
the binary does not update the code in an existing background process.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

A downstream application can build only the lightweight client:

```sh
cmake -S . -B build-client -DXDB_BUILD_SERVER=OFF -DXDB_BUILD_CLI=OFF
cmake --build build-client
```

See [Android integration](docs/ANDROID.md) for the NDK and embedded-server model.
The C ABI is in [xdb.h](include/xdb/xdb.h); the wire protocol is documented in
[PROTOCOL.md](docs/PROTOCOL.md).

The 1.0 contract is documented in [RPC API](docs/API.md) and [C SDK](docs/C_API.md),
including defaults, errors, states, lifetimes and examples. Published releases
follow the [compatibility policy](docs/PROTOCOL.md#version-and-upgrade-policy).

When built via `add_subdirectory`, dependency downloading defaults to OFF. Supply
`nlohmann_json::nlohmann_json` (>= 3.12.0), installed dependency packages or local
FetchContent source overrides, or explicitly opt in with `XDB_FETCH_DEPENDENCIES=ON`.
Standalone builds retain automatic pinned downloads; `XDB_FETCH_DEPENDENCIES=OFF`
also enforces offline dependency resolution there.

## Command-line use

Run `xdb` or `xdb --help` for a command overview. Each command has detailed help
and examples, such as `xdb pair --help` and `xdb ddi prepare --help`.
Help does not start or connect to a server.

Desktop CLI and SDK clients automatically start a background server when needed.
The server remains running after the client exits and is reused by other clients.
It listens only on loopback (default port 17423); local clients need no access token.
It does not authenticate local processes or operating-system users. Any local
process able to connect can control devices, access the clipboard and stop the server.
Phone pairing does not distinguish those clients.

```sh
# Starts the background server automatically.
xdb devices -l
xdb pair --wifi
# Select the advertised computer in the phone's Developer Mode pairing page.
# Enter the one-time code printed by xdb on the phone.
xdb pair --usb
xdb -s '<UDID>' ddi prepare --wifi
xdb -s '<UDID>' screenshot screen.png --wifi
```

`devices` prints one line per discovered device; `devices -l` includes its name,
addresses, network interfaces and pairing/connection details. `--json` retains the
complete structured response. Empty names and endpoint fields are omitted from
text output. Candidate pairing reasons are shown only when they differ from the
device's pairing summary. States have distinct meanings:

| State | Meaning |
| --- | --- |
| `discovered` | Found through USB/mDNS; no verified local Wi-Fi pairing record was matched |
| `paired` | A complete local record matched the Wi-Fi advertisement; connection remains unverified |
| `connected` | The server currently holds an authenticated device session; individual services may still fail |

Listing does not actively connect to devices. `pairing:record_present` never
means the phone has accepted a new connection. USB trust is not inferred from
USB enumeration. Unknown device identities appear as `(unidentified)`.

For `screenshot` and `ddi prepare`, omit `-s` when discovery finds exactly one
identified device. `--usb` / `--wifi` constrain that discovery, and an explicit
Wi-Fi address further selects its matching candidate. Several addresses for the
same UDID count as one device. Multiple devices, including unidentified ones,
require explicit selection; an unidentified device must be paired first.

Use `xdb start-server` to start or reuse the background server explicitly and
`xdb kill-server` to stop it, cancelling its sessions. Killing an absent server
succeeds without starting one. `xdb server` remains available for foreground
diagnostics. Default pairing data is stored in `~/Library/Application Support/XDeviceBridge`
on macOS, `%LOCALAPPDATA%/XDeviceBridge` on Windows, and
`${XDG_DATA_HOME:-~/.local/share}/XDeviceBridge` on Linux. The CLI and low-level
pairing defaults share this rule. `--data-dir /absolute/path` selects a store when starting a new
server; it never changes a running server's store. The default is the platform
user data directory. `start-server` / `kill-server` require a fixed port;
`server --port 0` can select an ephemeral port for diagnostics.

Desktop SDK startup locates `xdb` beside the client library, in the sibling
`bin` directory, or on `PATH`. `XDB_EXECUTABLE` can override this with an explicit
executable path (useful for separate development build trees). No server backend
is loaded into the client process. Android Apps continue to start an embedded
server themselves.

`--wifi` without an address discovers paired devices; `--wifi ADDRESS` selects a
RemotePairing address explicitly. Device sessions prefer USB and fall back to
paired Wi-Fi unless a transport is explicitly selected.
USB requires a working system usbmux/Apple device service and device trust.
Android initially supports Wi-Fi; Android USB Host integration is not implemented.

For protocol development, `xdb rpc METHOD JSON_PARAMS` and `xdb rpc-shell` expose
raw requests. `rpc-shell` maintains a client session; it is not an iOS command shell.

## Initial scope and limitations

- Discovery, USB/Wi-Fi pairing and DDI download/preparation.
- One device session per client connection; a known UDID has one owning connection.
- PNG screenshots; decoded latest-frame BGRA video with display geometry metadata.
- Single-contact panel-normalized touch, keyboard usages, ASCII text, consumer keys and app control.
- Binary media is framed separately from JSON, never Base64-encoded. Only one outstanding
  request per connection; frame polling returns the latest/newer frame rather than queuing all frames.
- Some operations can already have changed the phone when a transport failure is reported.
  Do not automatically retry input, pairing or image replacement after an unknown outcome.
- Cancellation uses client disconnection or server shutdown. Existing device service calls
  may finish at their own deadlines; immediate cancellation of every service is not promised.
- PCM audio transport and server-side raw HEVC / MP4 / MKV recording are available
  through RPC. Encoded HEVC/AAC transport to clients remains follow-up work.
- Cross-build success does not prove Android application packaging, lifecycle, permission
  handling or physical-device behavior. See [validation](docs/VALIDATION.md).

## License

Project-owned code is licensed under **LGPL-3.0-only**. See [LICENSE](LICENSE) and [COPYING.GPLv3](COPYING.GPLv3).
Third-party components retain their own licenses; the license of a distributed binary
also depends on its exact dependency build, especially FFmpeg.
See [third-party components](THIRD_PARTY_NOTICES.md).
