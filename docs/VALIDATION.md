# Validation

## 2026-10-27: released 1.0 / 1.1 client-server compatibility

- macOS ARM64, using the real client and server libraries from the official
  `v1.0.0` and `v1.1.0` notarized release archives. The older archive's SHA256
  was checked against its published GitHub asset digest
  `5b14a08be3cde63dd118e818f9aca1614ee68f92e3b638f8758706100ce14678`;
  the 1.1.0 archive retained its independently verified release hash
  `a63142541ed0065250aa9ab7efc6a826c548c1f6c419a038e1465e4411f45b9c`.
- All four combinations passed: client/server 1.0.0/1.0.0, 1.0.0/1.1.0,
  1.1.0/1.0.0 and 1.1.0/1.1.0. Each version ran in its own process to avoid
  shared-library loader aliasing, with a free loopback port and temporary empty
  data directory. Existing servers and pairing records were not changed.
- Verified the published C API's version queries, protocol-1 handshake,
  `server.info`, ignoring an extra optional request field, `connection.close`,
  and `unknown_method` for an unsupported input method. Subsequent requests
  succeeded after each rejected operation.
- The old server has no `ddi.status` endpoint: it returns `not_connected` /
  `Open a device session first`, even though opening a session would not add the
  method. The 1.1 server instead recognizes the method and rejects missing UDID
  as `invalid_params`. This diagnostic limitation in the published 1.0 server
  remains; clients must handle operation failures and upgrade the server for
  new methods, rather than infer support from a successful handshake.
- No device session, pairing, DDI operation or media capture was performed.
  This acceptance covers the macOS binary/API boundary and these requests;
  it does not establish cross-version live-media or Windows/Linux acceptance.

## 2026-10-27: image-mounter session cleanup

- iPhone14,4 / iOS 27.0.1, paired Wi-Fi, phone configured not to auto-lock.
  Before the fix, repeated CLI DDI queries on both macOS and Windows ARM64 could
  wait about 30 seconds in image-mounter responses. A standalone control probe
  reproduced this without `Hangup`; adding it completed repeated full queries in
  about 1.0–1.2 seconds on both hosts.
- The rebuilt real macOS CLI passed successive explicit-IP status, automatic
  status, explicit-IP status, existing-DDI prepare and automatic status requests:
  1.142, 4.257, 1.242, 0.838 and 4.189 seconds. The rebuilt Windows ARM64 CLI
  passed alternating explicit/automatic status requests in 1.398, 4.399, 1.280
  and 4.481 seconds. Timings are measured inside each host, excluding VM command
  output delivery. Automatic discovery retains its existing discovery budget.
- Status consistently reported the Personalized mount and installed DDI Cryptex
  `com.apple.MobileAsset.DDI` version `27.1.5228.8`. Prepare reused this mount;
  this round did not remove or newly install a DDI. Windows used a private
  temporary copy of a verified record, removed afterwards without replacing its
  existing default credentials.
- All 52 macOS offline/IPC tests passed. The image-mounter transport test also
  built and passed natively on Windows ARM64, and now runs in Windows CI.
  Cases cover exactly-once Hangup, reconnect, empty/rejected/malformed replies,
  complete/partial/failed raw upload, missing upload completion, failed framing,
  a silent peer, and cancellation before/during cleanup. These checks do not
  establish real USB cleanup or compatibility with older image-mounter versions.

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

## Linux Avahi discovery and advertisement (2026-10-26)

Debian 13.7 ARM64 in a Parallels VM, with a bridged LAN interface and Avahi 0.8,
built source at `6d8ad99` plus the owner-name normalization fix `7ad229f`.
The CLI and server ran as an ordinary desktop user with a separate test data
directory; existing pairing records and system network configuration were
preserved.

- `xdb_avahi`, `xdb_dns_sd` and `xdb_discovery` passed inside the VM.
- The first real discovery run exposed Avahi's owner names without a trailing
  root dot. After normalizing them at the native adapter boundary, `devices
  --wifi -l` listed the phone at IPv4 `10.24.24.7:49152` and its scoped IPv6
  link-local endpoint on `enp0s5`. With an empty test pairing store it correctly
  reported `discovered`, without inventing a UDID or claiming connectivity.
- Runtime tracing confirmed loading `libavahi-client.so.3`, connection to the
  system bus, and native `RecordBrowserNew` requests. The server's ELF `NEEDED`
  list contains neither Avahi nor D-Bus libraries.
- Pairing used native `EntryGroupNew`, five `AddRecord` calls and `Commit`.
  `avahi-browse` resolved the UUID instance and matching UUID hostname with the
  real listening port and the expected pairing TXT fields. Capture on the guest
  interface observed IPv4/IPv6 multicast announcements and service queries from
  the iPhone. During the advertisement, xdb owned no UDP discovery socket.

- The operator selected `xdb - YJBeetle-M2-Debian` on the iPhone and entered the
  displayed one-use PIN. The CLI reconnected to verify the new Wi-Fi record and
  returned `Paired` with the expected device UDID. A subsequent Wi-Fi discovery
  returned `paired` with both address families. The data directory had mode
  `0700` and the saved record mode `0600`; the temporary pairing advertisement
  disappeared after the operation.
- The old default-port server was still running an earlier build, so an ordinary
  CLI initially showed stale behavior despite rebuilding the executable. After
  restarting that server, the verified test record was copied atomically to the
  default user directory with no-replace semantics. Ordinary `devices --wifi -l`
  then returned the paired device. The test-only server and monitor were stopped.

This verifies Avahi discovery, pairing advertisement, the phone-initiated PIN
handshake, post-pairing verification and record persistence on the stated setup.
USB is outside this acceptance record.

## Linux USB passthrough (2026-10-26)

The same Debian VM and build above were tested with iPhone14,4 / iOS 27.0.1
attached by Parallels USB passthrough. Linux initially exposed the PTP interface,
but the already-running usbmuxd had logged `LIBUSB_ERROR_NO_DEVICE` during device
initialization and returned no USB devices. Restarting the existing Debian
usbmuxd service restored its device connection; no xdb code, USB configuration
or pairing records were changed to recover enumeration.

- As the ordinary desktop user, `devices --usb -l` listed the expected UDID and
  `pair --usb` successfully verified the existing USB trust record. This was
  record reuse, not a new trust-prompt acceptance test.
- `screenshot --usb` returned a valid 1125×2436 PNG.
- A C ABI client explicitly opened `transport: "usb"`, read the device model
  and OS version, and started video with software decoding. Three BGRA responses
  had advancing serials (1, 33, 44), dimensions 1136×2464, and exactly
  `4544 × 2464 = 11196416` payload bytes each. `video.stop` and
  `connection.close` succeeded afterward.
- usbmuxd reported no new transfer errors during these checks.

This validates USB discovery, existing-record verification, device information,
screenshot and decoded video on this VM setup. It does not establish long-term
USB stability or identify the cause of the initial passthrough initialization
failure. DDI mounting and input injection were not exercised in this run.

## Windows native DNS-SD discovery (2026-10-26)

Windows ARM64 build 26100.9711 in a Parallels VM built the Windows DNS-SD adapter
with MSYS2 CLANGARM64 (Clang 22.1.8). The iPhone was iPhone14,4 / iOS 27.0.1.

- The Windows DNS-SD and CLI device-list tests passed inside the VM. Coverage
  includes PTR-only browse replies followed by resolution, separate IPv4/IPv6
  interface indices, TTL-zero removal, unrelated service types, missing APIs,
  and callbacks that complete after cancellation or the discovery deadline.
- A probe called `detail::windows_browse` directly, bypassing the raw fallback.
  It reported the native provider available and returned the phone's IPv4 address
  and IPv6 link-local address with the correct scope. Real API calls showed that
  queries with a trailing root dot were accepted but returned no results on this
  Windows build; undotted API queries worked. Cache names remain absolute.
- An experimental `DnsServiceRegister` advertisement returned success and was
  visible from macOS with the expected UUID service instance, UUID SRV target,
  listener port and all seven TXT properties. The UUID target did not resolve to
  an A/AAAA address from macOS during the test, and selecting the host on the
  phone did not establish a pairing TCP connection. This is not accepted as a
  working native advertisement. The experimental advertiser was removed;
  Windows pairing publication retains the existing complete direct-mDNS records.
- With direct publication restored, the operator completed PIN pairing. xdb
  reconnected to verify the new record in a separate test data directory; native
  discovery then matched it as `paired` and retained both address families.
  Explicit Wi-Fi connection, `device.info` and a 1125×2436 PNG screenshot worked.
- A first video start after the screenshot encountered a display-service HTTP/2
  handshake disconnect. A fresh session using an explicit Wi-Fi address started
  software decoding and returned three advancing BGRA frames (1136×2464,
  `4544 × 2464 = 11196416` bytes each), then stopped and closed normally.
  A separate fresh session using native auto-discovery also started video and
  returned advancing frames with the same checked geometry and byte counts.

The Apple USB composite device initially reported Code 10; restarting that device
did not clear it. After the operator physically unplugged and reconnected the
iPhone to the Windows VM, its USB device status became `OK`. The same build then
passed USB enumeration, verification of the existing USB trust record, a
1125×2436 PNG screenshot, and an explicitly USB C ABI session returning device
information and three advancing decoded video frames (serials 1, 25, 50).
Each BGRA response had the same checked geometry and 11196416-byte payload as
the Wi-Fi tests. Video stopped and the session closed normally.

The test-only server was stopped afterward. No system firewall policy or
existing pairing records were changed. Native Windows advertisement
compatibility, long-term stability, and other Windows versions remain outside
this result.

## Windows DDI lifecycle acceptance (2026-10-26)

Built the `ba71123` runtime sources in `C:\Workspace\XDeviceBridge\build` on
Windows ARM64, build 26100.9711, using MSYS2 clangarm64 / Clang 22.1.8. The
executable was verified as COFF-ARM64. With the test-only fixes `86d4aa0` and
`837536f`, all 16 Windows CTest cases passed under the machine's GBK locale.
The fixes explicitly decode UTF-8 documentation and consume the complete fake
hello request before closing a rejection socket; they do not change the runtime.

The real device was iPhone14,4 / iOS 27.0.1. USB drivers reported OK. Test servers
used separate loopback ports, retaining existing pairing records. The Windows
user's default Wi-Fi record was rejected as revoked; Wi-Fi acceptance instead
used the still-valid record retained from the earlier Windows pairing test in
`C:\Workspace\xdb-windows-dnssd\runtime\data`. No new pairing was performed.

USB acceptance:

1. `ddi status --usb --json` initially reported `mounted:false` and
   `cryptex_installed:false`.
2. `ddi prepare --usb --cache-dir ...` downloaded and verified all five pinned
   Cryptex assets into `build\validation\ddi-cache`, requested an Apple ticket,
   uploaded the assets and verified `com.apple.MobileAsset.DDI 27.1.5228.8`.
3. `ddi remove --usb --json` reported `changed:true`; a separate status query
   confirmed both the mount and Cryptex installation were absent.
4. A second removal reported `changed:false` without reinstalling anything.
5. Preparing again reused the downloaded assets, completed in 15.33 seconds and
   restored the installed/mounted state. A new screenshot returned a visible
   1125 x 2436 PNG (193197 bytes).

Wi-Fi acceptance with the verified test record:

1. Status identified the installed DDI and mounted image through paired discovery.
2. Removal reported `changed:true`; a separate status query confirmed absence.
3. A second removal reported `changed:false`.
4. Preparing again reused the Windows cache, requested a ticket and uploaded the
   image over Wi-Fi. It completed in 39.28 seconds and restored installation and
   mount state. A new visible PNG was 1125 x 2436 (191856 bytes).

The phone locked during the initial USB preparation: upload finished but the
installation reply waited until the user unlocked it. That preparation's 229.25
seconds include download and the lock wait, and are not a throughput baseline.
Earlier macOS read-only diagnosis likewise found `copy-installed` queued at a
cryptexd barrier while locked; unlocking restored a successful status reply in
0.54 seconds. Keep the device unlocked during these DDI tests. This evidence does
not establish identical lock behavior for every iOS version.

An additional Windows latency observation remains open: several automatic Wi-Fi
DDI operations took about 31 seconds, although standalone discovery took 3.11
seconds. The snapshot's IPv4 endpoint preceded its scoped IPv6 endpoint. On the
same installed image, an explicit IPv4 status query took 1.24 seconds versus
31.63 seconds with `--wifi auto`; both returned the same state. Specifying
`--wifi ADDRESS` avoids the observed delay. These timings do not yet identify
which automatic-connection stage accounts for the difference.

The test servers were stopped. The device was left with the DDI installed and
mounted. The older default server on port 17423 was not replaced; it still points
to the desktop's previously installed runtime. These results validate the stated
build/device/USB/Wi-Fi paths, not a portable Windows package or other iOS models.

## Linux DDI lifecycle acceptance (2026-10-26)

Built `ca17de7` in `/home/YJBeetle/Workspace/XDeviceBridge/build` on Debian 13
ARM64 (kernel 6.12.113+deb13-arm64), using GCC 14 and CMake 3.31.6. All 51 CTest
cases passed. Avahi 0.8 and usbmuxd 1.1.1 were active; the iPhone was passed
through to the Parallels VM by USB. Test servers used separate loopback ports
and the user's existing Wi-Fi pairing record. No new pairing was performed.

The device was iPhone14,4 / iOS 27.0.1, initially with
`com.apple.MobileAsset.DDI 27.1.5228.8` installed and its image mounted.

USB acceptance:

1. Status identified the installed Cryptex and mounted Personalized image.
2. Removal completed in 1.63 seconds with `changed:true`; an independent status
   query confirmed `mounted:false` and `cryptex_installed:false`.
3. Repeated removal completed in 0.89 seconds with `changed:false`.
4. Preparation downloaded and verified the five pinned assets into
   `build/validation/ddi-cache`, requested a ticket, uploaded the assets and
   verified installation. Its 159.61 seconds include downloading and waiting for
   the phone to be unlocked, so this is not a throughput baseline.
5. A separate status query confirmed installation and mount state; a new visible
   screenshot was 1125 x 2436 (1386119 bytes).

Wi-Fi acceptance through paired automatic discovery:

1. Initial status identified the installed/mounted DDI in 4.46 seconds.
2. Removal reported `changed:true`; a separate status query confirmed absence.
3. Repeated removal reported `changed:false`.
4. Preparation reused the Linux cache, requested a ticket and uploaded the assets
   over Wi-Fi. It completed in 39.01 seconds and restored installation and mount
   state, confirmed by a separate query.
5. The first screenshot was a valid PNG with a black image. After the operator
   unlocked the phone again, a fresh Wi-Fi screenshot showed the home screen:
   1125 x 2436, 3926098 bytes, captured in 4.40 seconds. The black image alone was
   not counted as visible screenshot acceptance.

Linux reproduced the automatic Wi-Fi latency observed on Windows: removal,
post-removal status, repeated removal and restored status took 31.82, 31.58,
32.14 and 31.69 seconds respectively. On the restored image, a later explicit
IPv4 status query took 1.24 seconds and returned the same installed/mounted
state. The delayed automatic-connection stage remains unidentified.

Both test-only servers were stopped; the existing default server was retained.
The device was left with its DDI installed and mounted. These results cover this
Debian ARM64 VM, existing trust records and the stated device, not other Linux
distributions or iOS models.

## Android NSD subscription acceptance (2026-10-27)

The Android adapter introduced in `a7f1826` was built with NDK 29/API 26 and
compiled into the scrctl-android debug App (compile/target SDK 36). The real host
was a Xiaomi 13 Ultra/API 36, using its existing App-private pairing record for
iPhone14,4 / iOS 27.0.1. The App, not a shell executable, loaded the embedded
server. No credentials were imported, replaced or printed; no DDI mutation was
performed in this test.

Five on-device instrumentation tests passed, covering independent server ports,
interruption/close races, repeated controller lifecycle/network-state transitions,
the native NSD adapter, and an opt-in iPhone stream test. The NSD test registered
an ephemeral `_remotepairing._tcp.` service, observed its resolved callback in the
adapter and its endpoint in the native `devices` reply. Warm snapshot discovery
returned in under two seconds instead of waiting the requested three seconds.
Unregistering the test service removed it; multiple leases shared the subscription
and the last close cleared it. A new lease did not resurrect the lost test peer.

The iPhone test identified the existing paired phone through the NSD snapshot,
opened a Wi-Fi session and received three advancing, decoded BGRA frames, then
stopped video and closed the session. The App UI separately discovered the phone
and displayed its visible lock-screen image in the fullscreen mirror. Android's
service-discovery diagnostics showed only the intended remote-pairing browse and
service-info subscriptions for the App UID on its Wi-Fi network. Backgrounding
the App released its active NSD requests; foregrounding recreated them.

This validates native discovery, the existing-pairing connection and video on
this device. Pairing advertisements retain direct mDNS and were not re-paired.
API 24–33 fallback, physical network switching, simultaneous Ethernet/Wi-Fi and
other Android vendors remain outside this real-device result. The controller
network-state test simulates lifecycle transitions, not a physical Wi-Fi switch.

## Desktop discovery subscriptions (2026-10-27)

Validated the Server-lifetime discovery subscription on Apple Silicon macOS,
Debian ARM64 in Parallels, and Windows 11 ARM64 in Parallels. Each used its native
provider (Bonjour, Avahi, and Windows DNS-SD respectively). The existing iPhone
was iPhone14,4 / iOS 27.0.1; pairing records were retained without replacement.

Offline/IPC regression suites passed: 53 tests on macOS, 53 on Linux and 18 on
Windows. Platform tests cover persistent record resolution and withdrawal;
shared-cache tests cover concurrent readers, waiting for endpoint/TXT resolution,
failure clearing, request-driven recovery and prompt shutdown. Cache tests cover
daemon-owned expiry, late TXT removal and bounded storage across service churn.
The final readiness changes also passed the three targeted discovery tests on
each desktop platform. Android Server cross-compilation succeeded with NDK 29
and the existing scrctl-android dependency binaries; this is compile evidence,
not a new Android runtime or minimum-API acceptance result.

Real command timings were measured inside each host, excluding VM command
transport overhead. With the phone already discovered, repeated Wi-Fi `devices`
queries took approximately 0.07 seconds on macOS, 0.15–0.16 seconds on Linux, and
0.08–0.11 seconds on Windows. Ephemeral `_remotepairing._tcp.` test services
appeared and disappeared through the same running subscriptions on all three
hosts. Removal was checked by disappearance of the whole candidate, not merely
its port. Native notifications can take time to arrive; these measurements are
not a promise of immediate network propagation. macOS automatic-discovery DDI
status completed in 0.83 seconds without changing the device's DDI state.

The native loops block on provider events and explicit stop notifications, with
no periodic rescan timer. Cold or empty discovery can still wait up to the
requested budget. This round did not exercise physical Wi-Fi/network switching,
daemon restart against a real provider, USB handoff, new phone pairing, or power
consumption. Failure/retry behavior was covered with controlled providers.

## Shared mirror control acceptance (2026-10-28)

On Apple Silicon macOS with the existing iPhone14,4 / iOS 27.0.1 Wi-Fi pairing,
two independent CoreDevice video negotiations did not sustain two live streams:
the first froze after the second started, both over separate tunnels and over one
shared tunnel. The phone was unlocked; saved Settings/Home frames confirmed real
screen changes. In the separate-tunnel run the first serial stopped at 214 while
the second advanced to 341; in the shared-tunnel run they were 194 and 328.
The first did not resume within 2.5 seconds after closing the second. Automatic
video restarts were disabled and closing the second skipped device-wide stopAll.
These observations cover this device and negotiation path, not all iOS devices.

The new capability-negotiated shared control groups instead reused one FramePump.
Two independent opens and attached polling sockets received video; one group
launched Settings and the other returned Home, with saved screenshots confirming
the visible effects. Closing the first root socket retained its group while its
auxiliary attachment remained; closing that attachment left the second group's
video and input working. Conflicting video options, simultaneous single-touch
ownership, shared global stop/record requests and exclusive/shared mixing were
rejected. Read-only screenshot access remained available after the last control
group left, while video stopped; legacy exclusive media.finish remained usable.

Audio checks confirmed one consuming group: another group could neither consume
nor stop it. Closing the non-owner preserved audio/video; closing the audio owner
preserved the other group's video, and that group could explicitly start audio.
Two real desktop scrctl windows then rendered 67 and 134 frames in staggered
four/eight-second runs. Both renderer readbacks were saved, and a read-only poll
confirmed advancing video after the first window exited, then stopped video after
the last exited. These counts are functional evidence, not a performance benchmark.
Original pairing records were unchanged; isolated test Servers were stopped.

## Single-window RPC latency (2026-10-28)

The disconnect monitor's per-request socket poll could delay replies by about
50 ms until its thread joined. An isolated, device-free run of 96 server.info
requests averaged 23.67 ms, with 44 requests taking at least 40 ms. Waking an
interruptible condition-variable wait at request completion reduced the same
run to 0.20 ms average and 0.27 ms p95, with none reaching 40 ms. These are local
measurements, not CI timing thresholds.

On the existing iPhone14,4 / iOS 27.0.1 over Wi-Fi, the same scrctl executable,
Metal renderer and 1136x2464 source delivered 105 rendered frames in a six-second
single-window run before the fix and 253 after it (approximately 17 versus 42 fps).
Separate three-second C-client probes of the old Server received 18.15 fps with
exclusive access and 18.00 fps with shared access; the fixed Server's shared path
received 41.45 fps. This isolates the measured RPC delay from shared-control
bookkeeping; it does not establish a universal frame rate or prove a regression
against every previously released frontend. No audio was routed in these runs,
and no pairing or DDI state was changed.

A software operation-response probe also issued app.launch for Settings from a
settled Home screen while another attached socket received decoded frames. It
classified a sampled interior-right image region (excluding the status bar and
dock), rather than treating any new serial as a changed image. With five trials
per build, median command-start to significant image change was 294.6 ms before
and 255.9 ms after; recognition of the Settings scene was 539.8 and 483.4 ms.
Samples varied with iOS launch/transition scheduling. These figures include that
animation and end when the C client receives the frame; compositor and physical
display scanout are excluded. They are not glass-to-glass video latency. Accurate
screen-to-screen measurement still requires both displays in the same camera view.
