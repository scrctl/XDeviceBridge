# Changelog

## Unreleased

- Add `xdb ddi status` and `xdb ddi unmount`, with USB/Wi-Fi selection and JSON
  output. Query mount evidence without downloads; verify explicit unmounts and
  treat already absent images as success. Preserve host caches and installed
  Cryptex assets.

- Use system Bonjour for macOS device discovery and temporary Wi-Fi pairing
  advertisements, with direct mDNS as a startup fallback. Keep existing pairing
  records, advertisement identity, discovery budgets and SDK contracts.
- Add an optional Linux Avahi backend with runtime library loading and direct
  mDNS fallback. Preserve pairing records and avoid adding Avahi dependencies to
  lightweight clients.
- Use optional Windows system DNS-SD for device discovery, preserving
  interface-scoped addresses and asynchronous cancellation with direct mDNS as a
  startup fallback. Keep direct mDNS for complete pairing-host advertisements.

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
