# Changelog

## Unreleased

- Add capability-negotiated `access: "shared"` control groups. Independent mirrors
  share one video producer, retain separate attachment/input lifetimes and reject
  conflicting media options. Legacy control access remains exclusive. Audio has
  one consuming group; recording remains exclusive.

- Add `connection.open` with `access: "read"` for screenshots and read-only
  queries alongside an existing controller. Scope attachments to their access,
  preserve media when readers close, and stop media/release held inputs when the
  last control attachment closes. Readers can retain the device for screenshots;
  a later controller can acquire it with a fresh capability.
- The screenshot CLI negotiates read access when Server advertises
  `capabilities.read_only_sessions`; older Servers retain their existing behavior.

## 1.2.0 — 2026-10-28

- Add `xdb connect HOST[:PORT]` and `xdb disconnect [HOST[:PORT]]`. Authenticate
  an explicit Wi-Fi endpoint without discovery; retain its base connection in
  Server across client/media-session exits. List connected endpoints alongside
  discovered devices. Disconnect/loss/shutdown closes them without automatic
  reconnect; preserve pairing records and DDI. Independent control sessions remain
  exclusive per device.

- Add optional `pair.display_brand` and `xdb pair --display-brand NAME` for
  application-specific host names/models. Keep `xdb` as the default and preserve
  display serials, pairing identifiers, keys and stored records. Advertise support
  through `server.info.capabilities.pairing_display_brand` so clients can detect
  older servers before requesting a custom prefix.
- Simplify text device listings by omitting the redundant `connection:` label.
  Keep the JSON field for API compatibility, prefer a sole manual connection for
  automatic screenshot selection, and report unsupported older Servers clearly.

## 1.1.0 — 2026-10-27

- Keep desktop native discovery subscriptions for the Server lifetime. Warm
  device listings and automatic Wi-Fi connection discovery reuse an updated
  snapshot instead of waiting a fresh scan window. Follow native removals, reclaim
  retired queries, and clear failed providers before request-driven recovery.
- Finish idle image-mounter sessions with a bounded, cancellable `Hangup` before
  closing or reconnecting. This prevents repeated Wi-Fi DDI queries from waiting
  about 30 seconds on affected devices; incomplete uploads and failed transports
  close directly without sending plist commands into raw image data.
- Add `xdb ddi status` and `xdb ddi remove`, with USB/Wi-Fi selection and JSON
  output. Query mount and Cryptex installation state independently; reverse device
  preparation by uninstalling the reported DDI Cryptex or unmounting the image.
  Verify absence and preserve pairing records, host caches and unrelated Cryptexes.
- Use system Bonjour for macOS device discovery and temporary Wi-Fi pairing
  advertisements, with direct mDNS as a startup fallback. Keep existing pairing
  records, advertisement identity, discovery budgets and SDK contracts.
- Add an optional Linux Avahi backend with runtime library loading and direct
  mDNS fallback. Preserve pairing records and avoid adding Avahi dependencies to
  lightweight clients.
- Use optional Windows system DNS-SD for device discovery, preserving
  interface-scoped addresses and asynchronous cancellation with direct mDNS as a
  startup fallback. Keep direct mDNS for complete pairing-host advertisements.
- Add an Android API 34+ `NsdManager` adapter for App-owned discovery subscriptions,
  resolved service updates and loss notifications. Multiple embedded runtimes can
  share a subscription through lifecycle leases; older Android versions retain
  direct mDNS discovery. The host App still owns permissions and lifecycle.
- Synchronize the English and Chinese DDI RPC references and check both languages
  for complete method coverage.

C ABI 1, local RPC protocol 1 and existing pairing records remain compatible.
Restart an already running Server to use the new discovery and DDI behavior;
updating only a client library does not upgrade the Server. Older servers do not
provide the new `ddi.status` and `ddi.remove` methods. See the
[validation record](docs/VALIDATION.md) for tested devices and remaining boundaries.

## 1.0.1 — 2026-10-26

### Fixes

- Save new Wi-Fi pairing records with atomic no-replace rename on Linux and
  Android. Android application sandboxes can reject the hard links previously
  used to publish records; existing records and symlinks remain protected from
  replacement, and filesystem errors now retain their actual cause.
- Fix MSVC builds of the server core, including warning options and missing
  declarations.
- Collect release attachments recursively so the publishing workflow accepts
  nested artifact directories.

### Command line and documentation

- Show a concise command summary and provide command-specific help without
  contacting or starting a server.
- Simplify device listings, omit empty fields and repeated pairing labels, and
  align candidate addresses in detailed output. JSON output is unchanged.
- Document the official Homebrew tap and migration from manual installations.
- Record Windows USB/Wi-Fi and MaaFramework C ABI device validation.

C ABI 1 and local RPC protocol 1 are unchanged. Pairing records remain compatible
with 1.0.0. Native system discovery backends are planned after this release.

## 1.0.0 — 2026-10-25

First stable integration baseline: C ABI 1 and local RPC protocol 1.

### Features

- Lightweight C client SDK for macOS, Windows, Linux and Android; desktop `xdb`
  CLI, automatic background-server startup and an embeddable server library.
- USB and paired Wi-Fi discovery, pairing and connection management. The server
  owns device credentials; clients receive device identifiers and status.
- Pinned developer-image downloads, reusable caches, personalization and mounting
  for supported Classic, Personalized and Cryptex image categories.
- PNG screenshots, BGRA conversion, HEVC video capture, touch/keyboard input,
  explicit clipboard access, orientation requests and application control.
- AAC-ELD audio, PCM delivery and server-side raw HEVC / MP4 / MKV recording.
  Recording skips damaged video until a checked IDR, continues audio, and retains
  source timestamps across packet loss.
- Desktop runtime and matching source archives, installation tools and a macOS
  Developer ID signing and notarization workflow.

### Reliability

- Bounded framing, parsing and media buffers; bounded/cancellable device RPCs,
  idle-client reclamation and independent audio/video stop ownership.
- Recording refuses to overwrite existing files. Decoder input limits, packet
  padding and output draining protect capture and recovery paths.
- Client/server version queries, ABI-major library names and documented handle,
  response-buffer, callback and concurrency ownership.

### Compatibility and scope

- Stable contracts are specified in the [RPC reference](docs/API.md),
  [C SDK reference](docs/C_API.md) and [compatibility policy](docs/PROTOCOL.md).
  The C ABI major becomes 1; the wire protocol remains 1. Consumers bundling the
  development SDK should rebuild against 1.0 and retain its versioned libraries.
- Local RPC listens only on loopback and does not authenticate local processes
  or operating-system users. It must not be exposed to a network.
- Device services and DDI availability vary by model and OS. Real-device evidence
  remains scoped to the revisions and devices in the [validation record](docs/VALIDATION.md);
  this release does not claim every iPhone/iPad or OS version has been tested.
- Text input uses keyboard ASCII; arbitrary Unicode injection, multitouch,
  device/application resolution changes and universal hardware decoding are not
  part of this baseline. See the [current limitations](README.md#initial-scope-and-limitations).
