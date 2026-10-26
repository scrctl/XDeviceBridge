# Validation

## 2026-10-24: candidate API boundary checks

- Invalid screenshot format/type values now fail parameter validation rather than
  falling back to PNG. Unit cases cover defaults, png/bgra and invalid values.
- Initial recording direction now waits for valid display geometry, maps the
  reported orientation explicitly (portrait overrides a conflicting numeric
  fallback with zero), and rejects unrecognized direction instead of guessing.
- input.text has a 30-second default total request budget (configurable 1..300000
  ms). Its per-request HID connection carries cancellation; queued report holds
  wake on stop and delivery stops between reports. Tests check cancellation before
  delivery, interrupting a one-second hold, a real 25 ms budget expiry, preserving
  uncancelled report order/final release, and stopping on write failure.
- Existing HID, service endpoint cancellation, recorder, IPC and documentation
  coverage tests pass alongside the new parameter/sequence checks. This is not
  an old-client/new-server binary compatibility acceptance.
- A USB check against backend commit `232c400` passed after the phone was
  reconnected: `screenshot` with `format: "jpeg"` returned `invalid_params`.
  Video-only MP4 recording with `orientation_from_device: true` and a conflicting
  numeric orientation of 270 used the reported initial `rot0` direction; independent
  FFprobe inspection confirmed zero rotation. Decoded frames were 1136×2464.
  This verifies the portrait override on this device, not every initial direction.
  Earlier device checks below remain scoped to their tested revision. Partial
  text on cancellation is not rolled back; callers may need an explicit keyboard
  release using a valid connection.

## 2026-10-24: current XDB path and upgrade checks

- macOS / iPhone14,4 / iOS 27.0.1, USB, real CLI and C client against an
  isolated server port: existing USB pairing was verified/reused; `ddi.prepare`
  confirmed an existing Personalized mount without downloading or reinstalling.
- New-session screenshots initially returned `geometry_changed` because the first
  display update arrived after capture started. The backend now waits at most two
  seconds for initial valid geometry, checks cancellation, and still rejects a
  genuine geometry change during capture. Fresh-session PNG capture then passed.
- `display.info` returned 1125 x 2436; software `video.frame` returned a decoded
  1136 x 2464 BGRA frame with matching native display geometry. App launch passed.
- Real touch/HID input in Safari: cleared the address field, sent `xdb-input-123`,
  waited for device UI consumption, sent backspace DOWN/UP, and visually verified
  `xdb-input-12` in a returned screenshot. No clipboard or navigation was used.
  An earlier simultaneous modifier+A sequence was not a valid chord setup; an
  immediate screenshot also preceded processing of the queued HID reports.
- With user-started QQ Music, `audio.start` (duplicate=true, buffer_ms=200) returned
  48 kHz / stereo. Three reads each returned 4800 frames / 19200 bytes with nonzero
  sample peaks; `audio.stop` succeeded. The computer did not play these samples.
  This proves PCM delivery, not subjective quality or phone-speaker routing.
- The downloaded Apple-Accepted scrctl/XDB macOS ZIPs and matching source archives
  passed SHA256 checks and extracted executable `--help` startup. The updated Unix
  installer was exercised with the signed native XDB runtime in temporary paths:
  live server stopped, stale library removed, restarted server PID changed and
  the separate data directory was retained. No user installation was replaced.
- 43 native offline tests passed. New installer cases cover managed upgrade,
  stale removal, unmanaged directory/foreign command refusal, invalid new runtime,
  stop failure preservation and activation rollback. IPC/lifecycle regressions
  cover recoverable request errors and refusing to replace a protocol-2 listener.

These checks do not claim fresh Wi-Fi PIN pairing, DDI replacement installation,
subjective audio quality, Windows installer execution, another iPhone/iPad model,
physical disconnection or long-duration stability in this round. Only USB was
usable for discovery during these checks. Earlier device results remain historical
where they used the pre-XDB backend; the protocol and installer policies are in
PROTOCOL.md and INSTALL.md / INSTALL_zh.md. The prior notarized binaries predate
these fixes and must be rebuilt/notarized before releasing the updated revision.

## Historical initial migration checks

The initial implementation was checked locally on macOS:

- Full native CLI, server and client build.
- Existing derived JSON/plist/HID/DDI-selection offline tests.
- A real local TCP server exercised by the C client, CLI and fragmented raw messages.
- Protocol version rejection, oversized message rejection, invalid requests,
  no persistent state after invalid imports, and shutdown with an idle client.
- A separate client-only build.
- Android arm64 client, server and CLI compilation/linkage with API 24. The server
  includes target-built HEVC libraries; the client ELF has no device/media dependencies.

Android arm64 client/server builds are cross-compilation checks, not application
or physical-device acceptance. They use Android API 24 and target-built dependencies.
These initial checks do not imply successful physical-device acceptance.

Before a stable release: complete physical-device acceptance for fresh pairing
and Android application packaging/lifecycle. DDI replacement below covers one
iOS 27 device; older image categories still need their own device acceptance.
Existing-DDI reuse, USB input, video-only recording, desktop distribution and
consumer capture evidence are recorded below.

## Consumer migration checks

- 38 native backend/IPC tests pass, covering transport cancellation, HTTP/2/XPC,
  pairing identity, DDI selection/download/TSS/mount fixtures, RTP/frame geometry,
  audio decoding/regulation and recording finalization.
- MaaFramework's real C ABI passes deterministic server tests for discovery,
  BGRA/PNG capture and fallback, crop/rotation, touch/key/text and application RPCs.
  Python's optional-symbol and management boundaries also pass offline tests.
- scrctl's actual executable passes deterministic server tests for background
  capture, explicit clipboard, ASCII, attached PCM delivery and encoded recording.
- MaaFramework's Android arm64 control unit and Toolkit cross-build without device
  cryptography/media dependencies. This is not Android runtime acceptance.
- An existing authorized scrctl credential was imported into a disposable server
  directory. Wi-Fi discovery found the iPhone, but pair-verify rejected the record
  because the device Identifier did not match. PNG/video/input acceptance through
  the new server remains unverified; no device pairing or DDI was changed.

An existing PiCli record was converted in a temporary private copy and successfully
verified against iPhone14,4 / iOS 27.0.1 over Wi-Fi. The server returned a PNG and
decoded a 1136×2464 HEVC frame. The real scrctl executable then consumed two frames
headlessly, and the real MaaFramework C ABI captured through both Stream and
ScreenshotService (cropped native display 1125×2436). This did not test input,
audio listening, fresh pairing, DDI installation or Android device runtime.

Pairing label changes additionally pass the host metadata/serial, pairing workflow
and IPC tests; Android arm64 server recompilation succeeds. These do not assert
that an already paired iPhone updates its cached display labels.

Desktop lifecycle tests exercise real background processes on macOS: CLI and SDK
automatic startup, persistence after client exit, idempotent start/kill, concurrent
first starts, immediate restart, and refusal to replace a non-xdb listener. Initial
Windows launcher checks were cross-compilation; native desktop package checks
are recorded below. The Android client is rebuilt with process startup disabled;
this remains a cross-build check, not Android runtime acceptance.


## Desktop distribution checks

The [2026-10-24 package workflow](https://github.com/scrctl/XDeviceBridge/actions/runs/39997769147)
passed at commit `06019f9` on Ubuntu 24.04 x86-64, macOS 15 arm64 and
Windows 2025 x86-64. Each native job:

- Builds pinned server dependencies and the CLI/client/server libraries.
- Produces separate runtime and corresponding-source archives, with license texts
  in the runtime and build caches/provenance confined to the source archive.
- Relocates the runtime to a directory containing spaces and exercises real SDK
  startup through both sibling-bin lookup and a separately shipped client using PATH.
- Exchanges `server.info` through the C API and stops the background server;
  Windows additionally waits for process exit before temporary-directory cleanup.
- Checks that bilingual installation instructions are present and that the runtime
  contains no static archives or CMake build cache.

Local macOS checks also covered user-directory installation, command lookup and
runtime archive SHA256 verification. The separate
[Developer ID workflow](https://github.com/scrctl/XDeviceBridge/actions/runs/40001700376)
passed signing and Apple notarization at commit `5c77f69` (Accepted). This does not
verify execution on the minimum deployment OS. These packaging checks use isolated
data directories and do not exercise physical-device services.

## USB device acceptance (2026-10-24)

A disposable local server used the existing USB trust for iPhone14,4 / iOS 27.0.1:

- Screenshot and HEVC frame decoding succeeded (native panel 1125×2436;
  decoded frame 1136×2464).
- Home Screen Search displayed `qabc123` after ASCII input and `qabc12` after a
  HID backspace. The remaining query was cleared without submitting it.
- A 3.15-second video-only MP4 finalized successfully; an independent FFprobe
  counted 176 HEVC frames and FFmpeg decoded the entire file without errors.
- DDI preparation emitted `DDI already mounted: Personalized (no new installation)`
  and returned success. No pairing record or mounted image was replaced.

This exposed a missing `hevc_metadata` bitstream filter in the minimal desktop
FFmpeg recipe. The recipe now enables it and relocated-package smoke checks require
it. The existing recording-parameter fixture passes against these dependencies.
The recorder fixture's readback checks require demuxers omitted from the minimal
runtime, so they are not counted as passing release-dependency tests; the independent
file decoding above verifies the actual device recording instead. Audio listening,
fresh pairing and DDI removal/reinstallation were not exercised by this check.


## Audio and A/V acceptance (2026-10-24)

While the same USB-connected iPhone played music, a disposable server captured
with `duplicate: true`, leaving phone playback enabled. PCM negotiation reported
48 kHz, stereo and a 200 ms preroll. Reads returned 62,400 interleaved frames with
nonzero samples; frame counts matched binary payload lengths.

A video-and-audio MP4 finalized successfully. Independent FFprobe inspection found
314 HEVC frames over 5.419 seconds and 502 AAC-ELD packets over 5.035 seconds.
Audio starts at 0.295 seconds because its session starts after video; timestamps
retain that offset. Independent FFmpeg decoding of both complete tracks returned
no errors. This checks capture/transport/container decoding, not perceptual lip sync.
The user also confirmed prior listening quality was satisfactory; it was not
repeated as a new listening test during this automated check.

The updated three-platform package run at `6a34bab` completed successfully:
[packages](https://github.com/scrctl/XDeviceBridge/actions/runs/40003804533).
The Developer ID package at `3cfb502` also passed Apple notarization:
[signed package](https://github.com/scrctl/XDeviceBridge/actions/runs/40005397783).


With the client pinned to `3cfb502`, MaaFramework's actual C ABI connected over USB
and captured successfully on the first request using both Stream and
ScreenshotService (default scaled output 720×1559); no retry loop or display-setup
sleep was inserted. Its two iOS offline/integration tests and scrctl's existing
xdb integration test also passed.

The actual scrctl executable additionally recorded a five-second A/V MP4 through
the disposable server with `--no-window --no-playback --audio-dup`. It finalized
successfully, and independent FFmpeg decoding of both tracks returned no errors.
This exercises consumer-to-server recording without repeating a listening test.


## Notarized local upgrade (2026-10-24)

The Developer ID runtime from run `40005397783` was downloaded, checked against
its published SHA256, and verified with strict code-signature checks for the CLI
and all bundled dylibs. An online `codesign --check-notarization` check with the
explicit `=notarized` requirement succeeded for the CLI.

The extracted installer upgraded the existing user-local installation. There were
no established client connections before the upgrade. The existing device-store
file was byte-for-byte unchanged, and the installed CLI started the new service
and returned `server.info` with protocol 1 and version 0.1.0. This is local upgrade
and CLI ticket verification, not an App-bundle Gatekeeper assessment or a minimum-OS test.

MaaFramework's dependency update `630770a6` subsequently passed both upstream PR
[build](https://github.com/MaaXYZ/MaaFramework/actions/runs/40006341916) and
[test](https://github.com/MaaXYZ/MaaFramework/actions/runs/40006342086) workflows.


## DDI replacement acceptance (2026-10-24)

The installed xdb runtime used existing USB trust with iPhone14,4 / iOS 27.0.1:

1. Baseline screenshot succeeded, and ordinary preparation reported
   `DDI already mounted: Personalized (no new installation)`.
2. `ddi.download` fetched the pinned Cryptex build `27A5228h` into the platform
   cache (`cache_hit: false`) and validated all five assets before replacement.
3. `ddi.prepare` with `replace: true` completed the explicit uninstall/reinstall
   path, transferred the image and Apple ticket, and reported
   `DDI installed and verified: com.apple.MobileAsset.DDI 27.1.5228.8`.
   The replacement phase took 5.57 seconds; this was a fresh installation result,
   not the already-installed shortcut.
4. A new USB session captured a valid 1125×2436 PNG after replacement. Another
   ordinary preparation then reported the mounted-image reuse result again.

The cache remains at `~/Library/Caches/xdb/ddi/cryptex-27A5228h/`. The test used
a disposable server data directory and did not replace the saved pairing records.
This verifies Cryptex replacement on the stated device/OS; Classic and older
Personalized installations, other models and Android-host installation remain
outside this acceptance result.

## Windows client/server and MaaFramework C ABI acceptance (2026-10-25)

### Windows AMDP USB and Wi-Fi (Parallels VM)

The official `xdb-windows-amd64.zip` release artifact of v1.0.0 ran on
Windows 11 (build 26100) inside a Parallels VM with `AppleMobileDeviceProcess`
as the usbmux provider, against iPhone14,4 / iOS 27.0.1 attached by USB
passthrough:

1. `devices` listed the device through USB; the CLI auto-started the
   background server. `pair --usb` completed handshake, SRP mutual
   verification, host-key registration and record re-verification.
2. A USB session returned `device.info`, a 1125×2436 PNG screenshot, a
   1136×2464 BGRA video frame (byte count matches `row_pitch × height`
   exactly), touch input and the power key.
3. `pair --wifi` completed the advertised PIN flow (host shown as
   `xdb - YJBEETLE42B4`), and its post-pairing reconnect verified the new
   Wi-Fi record. A `transport: "wifi"` session then succeeded through
   auto-discovery without an explicit address: `device.info`, PNG screenshot
   and video frame all returned over Wi-Fi.

This validates the released Windows client, server, AMDP transport and both
device transports inside a VM. Bare-metal Windows, other Windows builds and
Intel Windows remain outside this result.

### MaaFramework real C ABI (macOS host)

MaaFramework built at `bf23044a` (PR #1574, xdb pinned to v1.0.0 `fea3f0f`)
drove the same device through the real `libMaaFramework` /
`libMaaIOSControlUnit` path: `MaaIOSControllerCreate`,
`PostConnection`, `GetUuid` (exact UDID), `PostScreencap` with both the
default short-side-720 scaling (720×1559) and
`MaaCtrlOption_ScreenshotUseRawSize` (1125×2436, matching the stream display
geometry), `PostClick`, `PostKeyDown/Up` and `PostStartApp/StopApp` all
returned success. This closes the "simulated server only" gap for the C ABI
path on macOS; Windows MaaFramework runs of the C ABI remain build-verified
only.

### Stream silence observation (macOS)

With a USB video session running, locking the screen and holding it locked
for 45 seconds did not silence the stream: frame serials kept advancing
(30 → 2173 → 2320) with `video_unusable` false and no restart. True receive
silence could not be induced non-destructively on this device/OS, so the
silence-recovery change (`81d89123` on the MaaFramework side, server-owned
recovery) remains covered by simulated-server tests only. Lock-screen frames
stay capturable on this device.

The host Mac itself could not complete Wi-Fi sessions on this LAN
(unicast to the device returned host-unreachable while ICMP passed); Wi-Fi
transport evidence above was collected from the Windows VM instead.

## macOS Wi-Fi pairing from the native DNS-SD checkout (2026-10-26)

The operator built `xdb` on macOS from `codex/avahi-mdns`, based on the Bonjour
implementation and link fix at `c476333`, and ran `./xdb pair` without an address
or USB option. The phone discovered the `xdb - <hostname>` pairing host, accepted
the one-use PIN, and xdb reconnected to verify the new Wi-Fi pairing record before
reporting `Paired` with the expected device UDID.

This is operator-provided real-device evidence for the macOS pairing, reconnect
and record-persistence path with the native backend enabled. Backend selection
was not traced during the run. The earlier host-unreachable observation above
therefore no longer describes this later pairing attempt; media and input
sessions remain covered by the separate results above.
