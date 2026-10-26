# xdb local protocol (version 1)

English | [简体中文](PROTOCOL_zh.md)

This is the **client-to-host-server protocol**, not Apple's device protocol.
The server listens only on IPv4 loopback and accepts local clients without
authentication. Loopback limits direct network access; it does not authenticate
client processes or operating-system users. Any local process able to connect can
invoke device control, clipboard access and server shutdown. Phone pairing and
device authentication are handled separately by the server and do not authorize
individual local clients.

## Version and upgrade policy

The package version (1.0.1), C ABI (1) and wire protocol (1) are separate contracts.
The protocol number in the frame header and `hello.params.protocol` must both be 1.
A package version mismatch alone is not a reason to kill or replace a server.

XDB 1.0 establishes the documented C ABI 1 and wire protocol 1 baseline.
The compatibility commitments apply to published releases, not unreleased development snapshots.

Within protocol 1:

- New optional request fields must preserve the old behavior when omitted.
- Clients must ignore unknown result fields; servers may add result fields/events.
  Event consumers must tolerate names they do not use. Required fields and existing
  error codes keep their documented meanings; error messages are diagnostic text.
- New methods may be added. A client must handle `unknown_method`/operation failure
  instead of assuming the method exists because a package version looks new enough.
- Removing a method/field, changing its type/units/default meaning or media body layout
  requires a new protocol number. This includes ownership/lifetime changes.
- Rejected handshakes close the connection and never trigger automatic replacement
  of a listener. Use the executable that owns the old service to stop it, then start
  the desired version. The SDK never kills an occupied/incompatible port automatically.

The C SDK owns opaque handles and response buffers as documented in `xdb.h`.
Existing exported entry points keep their signatures within the published ABI;
new entry points are additive and consumers must check optional symbols. Protocol
compatibility does not imply an Apple device supports a requested feature.

Incompatible C ABI changes require a new ABI major and shared-library SOVERSION;
incompatible wire changes require a new protocol number and migration notes. Users
should ship a tested client/server combination, and keep older packages available
for rollback. Pairing data is retained during installation; any future incompatible
on-disk migration must preserve the prior records or offer an explicit export path.

## Framing

Each message is a 16-byte header followed by UTF-8 JSON and optional raw binary:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `XDB1` |
| 4 | 4 | Version `1`, unsigned big endian |
| 8 | 4 | JSON bytes, unsigned big endian, 1..1 MiB |
| 12 | 4 | Binary bytes, unsigned big endian, 0..64 MiB |

Reads must tolerate TCP fragmentation and coalescing. JSON nesting is limited to
64 levels. Bad framing closes the connection before allocating oversized payloads.
Client requests currently carry no binary body. Never search for newlines to delimit messages.

## Handshake and calls

The first request must be:

```json
{"id":1,"method":"hello","params":{"protocol":1}}
```

Protocol version or hello method failure closes the connection. The library performs this handshake.
Then send one request at a time, with a unique unsigned integer `id` and object `params`:

```json
{"id":2,"method":"server.info","params":{}}
```

Responses use `{"id":2,"ok":true,"result":{...}}` or
`{"id":2,"ok":false,"error":{"code":"...","message":"..."}}`.
During a request, zero or more messages of the form
`{"id":2,"event":"progress","data":{...}}` may precede its response.
Pairing PIN events use `event: "pin"`, `data: {"pin":"123456"}` and must not be logged.
The server does not send credentials in responses, events or discovery results.

Disconnecting cancels pairing/DDI where supported and destroys the connection's
session. Cancelled or failed operations may have already changed device state.
The client can interrupt its socket from another thread; wait for request completion
before destroying that client. Event callbacks cannot reenter or destroy their client.

## Methods

Complete field/default/result/error/state references are in [API.md](API.md);
C symbol ownership/threading/reference is in [C_API.md](C_API.md). The table below
is a quick summary rather than the full method contract.

| Method | Parameters | Result / binary |
| --- | --- | --- |
| `server.info` | none | Protocol, server version and process ID |
| `server.stop` | none | Requests shutdown after replying; all sessions are cancelled |
| `devices` | `transport`: auto/usb/wifi; `timeout_ms`: 0..60000 | Devices, candidates, warnings |
| `pair` | `transport`: wifi/usb; optional `udid`; `timeout_ms`: 1..300000 | UDID and reused status; progress/PIN events |
| `ddi.download` | `os_version`, optional `product_type`, `cache_directory`, `timeout_seconds` | Build/cache metadata; progress events |
| `ddi.prepare` | `udid`, optional `wifi`, `port`, `cache_directory`, `replace`, download/install timeout seconds | Prepared status; progress events |
| `ddi.status` | `udid`, optional `wifi`, `port` | Mount state and evidence; independent installed DDI Cryptex metadata |
| `ddi.remove` | `udid`, optional `wifi`, `port` | Verified absent state and whether this request removed the DDI |
| `connection.open` | `udid`, optional `wifi`: auto/address, `port`: device port | Acquires a connection-owned session |
| `connection.close` | none | Releases services, inputs and device lease |
| `device.info` | none | Device UDID, OS version, model |
| `screenshot` | optional `format`: png/bgra (default png) | PNG body or decoded BGRA pixels, with display geometry |
| `video.start` | optional `hardware_decode` | Starts background latest-frame capture |
| `video.frame` | `since`: frame serial, `timeout_ms`: 0..1000 | Format BGRA, serial, width, height, row pitch, optional display geometry; raw pixels |
| `video.stop` | none | Stops the video pump |
| `input.tap` | `x`, `y`: 0..1; `hold_ms`: 0..2000 | Report delivery status |
| `input.touch` | `x`, `y`: 0..1; `down`: bool | Contact/release report delivery status |
| `input.keyboard` | `usages`: USB HID keyboard usages, each <240 | Replaces the held keyboard usage set; `[]` releases |
| `input.button` | `page` (default 12), `usage`, `down` | Hardware consumer-key down/up |
| `input.text` | `text`: US-keyboard ASCII/tab/newline, <=4096 bytes; `timeout_ms`: 1..300000 (default 30000) | Unsupported text is rejected in full |
| `app.list` | none | Bundle identifiers and names |
| `app.launch` | `bundle_id`, optional `terminate_existing` | Request status |
| `app.stop` | `bundle_id` | Request status |

Except management methods, operations require a device session. A disconnected socket cannot resume itself; a new socket can attach only while
another attachment keeps the session alive. Use one long-lived library client
or `rpc-shell` for a multi-operation session.

## Geometry and media ownership

`devices` adds per-device `state` (`discovered`, `paired`, `connected`), `pairing`
(`record_present` or `unknown`) and `connection` (`connected` or `unverified`).
Connected means an active authenticated server session, not a fresh liveness
probe or a promise that every device service works. Candidate `pairing` preserves
the discovery reason (`record_present`, `incomplete_record`, `identifier_hint`,
`ambiguous`, `invalid_advertisement`, `crypto_error`, `unknown`). The existing
candidate `paired` boolean still only describes a complete matching local record.
No active connection or pairing is performed by this listing request.

CLI-only selection allows omitting `-s` for `screenshot` / `ddi prepare` /
`ddi status` / `ddi remove` when exactly one identified device is found. SDK DDI
methods require an explicit nonempty UDID.

`ddi.status` queries mount evidence and the DDI Cryptex installation independently,
without downloading or installing. `cryptex_installed:null` means that service is
not advertised; queried installation failures return errors. `ddi.remove` reverses
preparation: uninstall the reported DDI Cryptex at its live version, or otherwise
unmount the Classic/Personalized image. Verifies absence; already absent is
success. Preserves host caches, pairing records and unrelated Cryptexes; does not
request a nonce roll.
Both methods need an exclusive lease; close active sessions first. Empty `wifi`
uses USB, `wifi:"auto"` uses paired Wi-Fi discovery. A reported OS category is not
the installed image's identity. Errors or cancellation can follow a device-side
change; query status before retrying an uncertain removal.

Idle image-mounter connections send a best-effort `Hangup` before teardown or
reconnect. Cleanup waits at most one second, honors cancellation and does not
replace the completed operation's result. Failed transports and incomplete raw
uploads close directly without an additional protocol command.

Touch coordinates refer to the device's panel axes, not a rotated/cropped client
window. BGRA frame dimensions can include codec padding. `display` metadata comes
from the device display subscription and contains width, height, orientation and
geometry serial. Clients must account for crop/rotation when mapping visible pixels
to panel coordinates; absent metadata must not be invented.

A response owns its pixels until `xdb_response_destroy`. There is no FFmpeg,
OpenCV, C++ object or GPU handle in the C ABI. Future encoded media negotiation
must be explicit; the current implementation does not send HEVC/AAC payloads.

## Consumer sessions and additional methods

`connection.open` accepts `transport` (`auto`, `usb`, `wifi`) and returns a random,
connection-private `attachment` capability. A client may call
`connection.attach` with it on another socket, allowing audio/video polling and
control to proceed independently. Keep this capability private. The session and
device lease live until its last attached connection closes. `connection.close`
only detaches the requesting client; it does not invalidate another attachment.

- `display.info`: subscribes to display updates and returns panel geometry.
- `display.rotate`: queries the current cardinal orientation, requests the opposite
  portrait/landscape orientation once, and verifies the returned state. It does not
  unlock the device's orientation policy or retry an uncertain mutation.
- `clipboard.set` / `clipboard.get`: explicit clipboard access (`text`, <=256 KiB
  on write). This is not automatic text-input fallback.
- `recording.configure`: before media starts, provide an absolute server-host `path`,
  `format` (`mp4`/`mkv`), `video`, `audio`, `orientation`, and optionally
  `orientation_from_device`. The file is written by the server. Container media
  remains encoded; it is not reconstructed from the IPC image/audio bodies.
- `video.start`: also accepts `decode` (default true), `record_path` (raw HEVC,
  absolute server path) and `restart_on_silence` (default true). Encoded-only capture
  must become ready with a checked recording IDR before this call succeeds.
- `video.frame`: `allow_cached` permits reuse of a healthy latest frame after its
  wait expires. Geometry is captured with the published frame, not read later.
- `screenshot`: `format: "bgra"` requests server-side PNG decoding; the default
  remains PNG. Both include geometry; a changing display fails the capture.
- `audio.start`: `decode`, `duplicate`, `buffer_ms` (0..1000); returns sample rate,
  channels and preroll frames. `audio.read` requests 1..48000 interleaved frames,
  returning raw **signed 16-bit little-endian** PCM and its actual frame count.
  Poll on a dedicated attached socket; never make a network call in a sound callback.
- `audio.stop`: stops audio only. `media.finish`: joins producers, finalizes
  recordings, then stops device audio. Errors propagate to the caller.
- `media.status`: terminal `error` and `video_unusable` (for screenshot fallback).
- `network.debug`: `enabled` controls server-side tunnel diagnostics.

`devices` includes `capabilities.video_decode`; clients should derive Stream
availability from the server, not their local libraries. In the C API, port 0
uses `XDB_PORT` or 17423. Desktop connections start a missing server automatically;
Android connections require an App-owned embedded server.
`xdb_client_start_server` explicitly ensures a server is running, while
`xdb_client_kill_server` stops an existing server without starting one.
`include/xdb/client.hpp` is an optional header-only C++ adapter; the
binary boundary remains the C API. GUI clients can use that API directly for
`pair` / `ddi.prepare`; device credentials never leave the server.

`media.status.recording_error` reports a recording failure separately from the
transport/media `error`, so decoded mirroring may continue. `media.finish` still
returns the recording failure. The server holds the requested audio startup
preroll; clients should keep only a small output queue instead of buffering that
duration again.

## Idle connection limits

A hello must complete within five seconds. After hello, a connection without a
device session or in-flight operation expires after 60 idle seconds; when the
16-worker pool is full, such idle clients can be reclaimed immediately. A partial
request does not reserve a slot. Connections owning/attached to a device session
and requests already executing are not evicted by this policy. Stalled frame
chunks have a five-second receive timeout. Clients whose idle socket was closed
must reconnect before another request.
