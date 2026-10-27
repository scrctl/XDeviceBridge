# XDB API reference

English | [简体中文](API_zh.md)

This document defines the public RPC contract for the current source, checked against `xdb.h`,
`Client.cpp`, `Server.cpp`, `Backend.cpp` and `Discovery.h`. Published releases
follow the protocol 1 compatibility policy.
[Wire framing and compatibility](PROTOCOL.md), [installation](INSTALL.md),
[device validation evidence](VALIDATION.md), [C SDK reference](C_API.md).

The protocol is between local consumers and XDB Server, not between XDB and iOS.
It listens on IPv4 loopback without client authentication. A local client can
control paired devices and request writes to server-host paths. Do not forward
this port to a network or treat loopback as an authorization boundary between
local processes. Phone authentication is separate. Credentials stay in Server.

## Common contract

- Requests: `{"id":1,"method":"...","params":{...}}`; one request at a time per
  socket. The SDK assigns IDs and performs `hello` automatically. No request binary.
- Success: `{"id":1,"ok":true,"result":...}`. Except `app.list` (an array), documented
  results are objects. Mutation/stop calls return `{}` unless stated otherwise.
- Failure: `{"id":1,"ok":false,"error":{"code":"...","message":"..."}}`.
  `message` is diagnostic, not a stable string to match. See error codes below.
- Events precede the terminal response and share its ID. Ignore event types/fields
  not understood. `progress.data` is either `{message:string}` or
  `{file:string,bytes:uint,total:uint}`; total may be unknown (zero). A `pin` event
  carries `{pin:string}` for the user to enter on the phone. Never persist/log PINs.
- String fields are UTF-8, booleans are JSON booleans, integer ranges below are
  inclusive. Required fields are marked **required**. Do not depend on incidental
  acceptance of values outside documented supported domains.
- All paths refer to the **server host**, not the client process. Preserve UTF-8.
  Recording paths must be absolute; cache paths should also be absolute so they
  do not depend on Server's working directory. No paths are shell commands.
- Successful input RPCs confirm report delivery, not that an app accepted the input.
  Query/screenshot the device separately when actual UI state matters.
- There is no universal request deadline in `xdb_client_request`. Use a worker and
  `xdb_client_interrupt` to abort; the handle then cannot be reused. Disconnection
  requests cancellation but cannot undo an already completed device/file mutation.

## Session states and ordering

| State | Methods available |
| --- | --- |
| Connected SDK / completed hello, no device session | server.info, server.stop, devices, pair, ddi.download, ddi.prepare, connection.open, connection.attach, connection.close |
| Device session owned or attached | All above plus device/display/input/app/clipboard/media methods; another open/attach on this connection fails |
| Detached by connection.close | Management methods available again; open/attach can create a new association |
| Interrupted, transport failed, destroyed | Discard client; reconnect with a new handle |

One UDID has one owning session at a time. Other sockets may share it via its
private `attachment` string. Opening the same UDID again returns `device_busy`.
An attached client has the same control rights; it is not a read-only subscriber.
The session survives until the **last** attached connection closes. Then media
is finalized, held input releases are attempted and the lease is freed. Cleanup
cannot promise successful releases if the phone/transport has already failed.

Media configuration is shared by attachments. Use one controller to change media
state, a dedicated attached audio reader, and a separate video poller as needed.
Do not issue competing reads from multiple audio clients: they consume the same
PCM queue. Shared-session locks prevent producer teardown during a read, but are
not a transaction mechanism for multiple control clients.

## Error codes and recovery

| Code | Meaning / typical response |
| --- | --- |
| handshake_rejected | Wrong initial method/protocol; server closes socket. Use a compatible client/server. |
| invalid_params | Missing/wrong JSON field type, bounded numeric value or explicit parameter validation failed. Fix request; connection can remain usable. |
| cancelled | Cancellation detected before backend dispatch; disconnect-time cancellation may instead give EOF/no response. |
| operation_failed | Device, discovery/connection, pairing, DDI, codec, recording or other backend failure. Message explains this occurrence; no stable subcodes yet. |
| device_busy | Device lease held by another session or pairing operation. Close its session/attachments first. |
| invalid_state | Method not valid for current connection/media state; fix ordering. |
| invalid_session | Attachment no longer identifies a live session. Open a new one. |
| not_connected | No device session for a device-dependent request. Open/attach first. |
| ambiguous_device | Empty-UDID automatic selection found multiple identified devices. Select an explicit UDID. |
| device_not_found | Empty-UDID automatic selection found no identified device. Refresh discovery or select a device. |
| geometry_changed | Display state changed while screenshot was captured. Retry after geometry settles; never map input using stale geometry. |
| unsupported | No requested decoder support, or input.text contains unsupported characters. Use a supported capability/path. |
| no_frame | No newer video frame in the requested wait. Poll again or inspect media.status. |
| unknown_method | Dispatch reached an unimplemented method. Negotiate/use a supported method. |

Error precedence follows dispatch: e.g. an unknown device method without a session
can return `not_connected` before `unknown_method`. Unknown `input.*` methods are
rejected before HID service initialization. Do not use unknown calls as a no-side-effect
capability probe. Discovery exposes video decoding availability explicitly.
Malformed framing, oversized messages and invalid request envelopes close the
socket without guaranteeing an RPC error. Successful RPC error receipt does not
mean transport failure. The C API return value distinguishes response receipt
from an RPC's `ok`; see C_API.md.

## Management

### `hello`

Wire clients only; first request within five seconds. Params: `protocol` **required**,
integer `1`. Result: `{protocol:1,version:string}` (package version). Wrong protocol
or method closes the socket after a best-effort `handshake_rejected` response.
The framing version must also be 1. SDK users must not send a second hello.

### `server.info`

No params/session required. Result: `{protocol:1,version:string,pid:integer,
device_transport:"usb-or-wifi"}`. `version` is the CMake package version; it does
not replace wire protocol negotiation or prove a device is connected.

`capabilities.pairing_display_brand:true` advertises support for `pair.display_brand`.
Treat an absent capability as unsupported; older servers may ignore unknown fields.

### `server.stop`

No params/session required. Result `{}` is sent before shutdown is requested.
All clients/sessions are cancelled; no device is unpaired and user files remain.
A response is not proof that the process has exited. `xdb_client_kill_server`
additionally waits for the port to close. Embedded owners must still call
`xdb_server_stop` to release their server handle.

### `devices`

| Param | Type / default |
| --- | --- |
| transport | string: auto (default), usb, wifi |
| timeout_ms | integer 0..60000; default 3000; discovery scan budget, not a universal total RPC deadline |

Result: `{devices:Device[],warnings:string[],cancelled:bool,
capabilities:{video_decode:bool}}`. Partial discovery can succeed with warnings or
`cancelled:true`; inspect these instead of treating an empty list as definitive.
Does not authenticate every listed device or actively pair/connect it.
Desktop servers read their shared native discovery snapshot when it is ready;
the timeout bounds waiting for cold/empty discovery, not a mandatory scan delay.
See the [protocol documentation](PROTOCOL.md) for cache ownership.

`Device`: `udid:string`, `name:string` (may be empty), `candidates:Candidate[]`,
`state:discovered|paired|connected`, `pairing:record_present|unknown`,
`connection:connected|unverified`. UDID may be empty for unidentified advertisements;
retain candidate details but do not invent an identity. `connected` describes an
active XDB session, not proof all phone services work.

`Candidate`: `transport:usb|wifi`, `address:string`, `port:integer`,
`interface:string`, `connection_type:string`, `paired:bool`, `pairing:string`.
USB address/interface may be empty and port zero. Wi-Fi IPv6 may include scope.
Retain all candidates; one UDID can have multiple interfaces/addresses.
`paired` means a complete matching local record. Candidate pairing reasons are
`record_present`, `incomplete_record`, `identifier_hint`, `ambiguous`,
`invalid_advertisement`, `crypto_error`, `unknown`; tolerate future reason values.

### `pair`

| Param | Type / default |
| --- | --- |
| transport | string wifi (default) or usb |
| udid | string; default empty = pairing workflow selection |
| timeout_ms | integer 1..300000; default 120000 |
| repair | bool; default false; USB-only permission to attempt repair |
| display_brand | string; default xdb; 1..64 UTF-8 bytes, no control characters or leading/trailing spaces |

Result `{udid:string,reused:bool}`. Can emit progress and Wi-Fi PIN events. Keep
request/socket alive while the user completes the phone interaction. Device trust,
unlock and developer settings may be required. A device in use gives `device_busy`;
empty-UDID pairing reserves the pairing operation against active device leases.
An existing usable record can be verified and reused. Wi-Fi re-pairing can replace
a stale record only after verification of the new pairing; normal clients never
provide/import arbitrary pairing-file text.

`display_brand` changes only the host name/model prefix for new pairing: for
example `scrctl - <hostname>` or, on Android, `scrctl - <device model>`.
It is used consistently in Wi-Fi advertising and pairing information, and in USB
pairing information. Display serials remain `XDB-...`; keys, host identifiers and
existing records are unchanged. Reusing a record does not rename the phone's saved
computer. Check `server.info.capabilities.pairing_display_brand` before supplying
this option to a separately installed server. The CLI exposes it as
`xdb pair --display-brand scrctl` and rejects unsupported servers before pairing.

### `ddi.download`

| Param | Type / default |
| --- | --- |
| os_version | string **required**; target iOS/iPadOS version |
| product_type | string; default empty; optional hardware product identifier |
| cache_directory | string; default empty = platform cache selection |
| timeout_seconds | integer 1..3600; default 300 |

No device session needed. Result `{build:string,cache_hit:bool}`; progress events
contain file/bytes/total. Downloads/verifies assets, **does not install/mount DDI**.
Manifest selection is not a claim that every phone with that version is supported.

### `ddi.prepare`

| Param | Type / default |
| --- | --- |
| udid | nonempty string **required** |
| wifi | string; default empty = USB; auto = paired Wi-Fi discovery; explicit address = Wi-Fi endpoint |
| port | integer 1..65535; default 49152 (explicit Wi-Fi address) |
| cache_directory | string; default empty = built-in cache selection |
| replace | bool; default false |
| download_timeout_seconds | integer 1..3600; default 300 |
| install_timeout_seconds | integer 1..3600; default 300 |

Result `{prepared:true}`. Progress may contain message or file/bytes/total. Requires
an exclusive device lease: close device sessions first. Reuses a suitable mounted
DDI; otherwise may download, request Apple personalization and install. Explicit
replacement can alter the phone's mounted developer image. Cancellation/timeout
is not transactional rollback. A prepared image is separate from availability of
video/audio/input services; see DDI/device validation documentation.

### `ddi.status`

| Param | Type / default |
| --- | --- |
| udid | nonempty string **required** |
| wifi | string; default empty = USB; auto = paired Wi-Fi discovery; explicit address = Wi-Fi endpoint |
| port | integer 1..65535; default 49152 (explicit Wi-Fi address) |

No device session needed; requires an exclusive device lease, so close existing
sessions first. Connects and queries the image mounter plus installed DDI Cryptex
metadata when that service is advertised. Does not download, sign, install,
remove or compare against a download catalog. Result:
`{udid:string,os_version:string,kind:string,image_type:string,mounted:bool,
verification:string,cryptex_installed:bool|null,cryptex_identifier:string,
cryptex_version:string}`.

`kind` is the OS-selected download category (`classic`, `personalized`, `cryptex`),
not the mounted image's build or identity. `image_type` is the queried service
image type (`Developer` or `Personalized`; Cryptex also appears as `Personalized`).
`verification` reports `LookupImage signature` or `CopyDevices entry` when mounted,
otherwise an empty string. This observes mount evidence, not compatibility with
any particular downloaded image.

`cryptex_installed` is independently queried through `cryptexd`; an installed DDI
can exist without a current mount. Its identifier and version are included only
when installed (otherwise empty strings). If the service is not advertised,
`cryptex_installed` is null; it is not a negative query result. A Cryptex OS target
requires that query to be available and otherwise fails. Modern USB connections
also establish RSD to inspect the actual installation, independent of OS download
policy. Connection stages and the shared query/removal phase have bounded
30-second budgets and support cancellation. Unverifiable replies or failed
queries return `operation_failed`, never fabricated absence. An occupied device
returns `device_busy`.

### `ddi.remove`

Parameters and exclusive lease requirements are the same as `ddi.status`.
Reverses device preparation without deleting pairing records or host caches:

- If `cryptexd` reports `com.apple.MobileAsset.DDI`, sends `uninstall` scoped to
  its reported installed version and verifies no DDI installation remains.
  Does not remove unrelated Cryptexes or guess a version from the download catalog.
- Otherwise, unmounts the Classic/Personalized image through the image mounter
  (`/Developer` or `/System/Developer`) and verifies its absence. This also handles
  an image-mounter DDI on a Cryptex-capable system.

An already absent DDI succeeds without a removal mutation. After removal, verifies
mount absence and rechecks the Cryptex installation where available. A remaining
mount after Cryptex removal is an error, not permission to try a second removal
mechanism. Returns the status fields above with `mounted:false`, no installed
Cryptex reported, empty verification/identifier/version, plus `changed:bool`.
`changed:true` means this request performed removal and verified the result.

Does not roll a personalization nonce, download assets or uninstall applications.
Use `ddi.prepare` to prepare the device again. This reverses DDI preparation's
result; it does not restore an image that an earlier `prepare --replace` replaced.
Cancellation or a lost reply can follow a device-side change: query `ddi.status`
before retrying. No automatic retry or rollback is performed.

## Connections and display

### `connection.open`

| Param | Type / default |
| --- | --- |
| udid | string **required**; empty requests automatic selection of one identified device |
| transport | string auto (default), usb, wifi |
| wifi | string; default empty; auto or explicit RemotePairing address |
| port | integer 1..65535; default 49152 (explicit Wi-Fi address) |

No current session allowed. Result `{udid:string,attachment:string}`. In auto mode,
empty wifi tries USB then paired Wi-Fi; wifi auto/address chooses the wireless
path. `transport:usb` forces USB. `transport:wifi` with empty wifi uses discovery.
Explicit UDID failures normally return `operation_failed`; not_found/ambiguous
selection errors refer to empty-UDID discovery. Preparation shares a bounded
30-second connection budget, but callers should still manage cancellation.
Keep attachment private; do not log it. It is a live session capability, not a
persistent device identifier and not a credential export.

### `connection.attach`

Param `attachment`: string **required**, from a live connection.open. No current
session allowed. Result `{}`. Shares the same device/session and producer state.
Failure `invalid_session` means it expired or is unknown; no resume after all
owners detach. Multiple sockets do not allow multiple owners of one device.

### `connection.close`

No params. Result `{}`; idempotent. Detaches **this socket**. Other attachments may
keep the session, media and held input alive. To finish a recording with errors
reported, call media.finish before closing; destructor cleanup discards errors.

### `device.info`

Session required. No params. Result `{udid:string,os_version:string,
product_type:string}`. These are returned device properties, not a capability list.

### `display.info`

Session required. No params. Result `Display`: `{width:integer,height:integer,
orientation:string,serial:uint}`. Starts/reuses display subscription and waits up
to two seconds for initial positive size and orientation after subscription setup.
Failure is `operation_failed` if initial geometry is unavailable/cancelled.
Width/height are native panel/display-mode dimensions; orientation is typically
rot0/rot90/rot180/rot270. The serial changes when watcher geometry changes and is
independent of the video frame serial.

### `display.rotate`

Session required. No params. Queries effective cardinal orientation and requests
the opposite portrait/landscape orientation once. Result
`{target:"portrait"|"landscapeLeft"}`. Success requires reported state to match.
Does not change the phone's orientation lock; applications/policy can reject it.
Unknown/mismatched response is `operation_failed`; do not blindly repeat an
uncertain request, because rotation is a mutation rather than an idempotent set.

### `network.debug`

Session required. `enabled`: bool, default false. Result `{}`. Enables/disables
server-side tunnel diagnostics if the device has a tunnel stack; otherwise a no-op.
Diagnostic output is not a structured API or a stable parsing target.

## Clipboard and input

### `clipboard.set`

Session required. `text`: string **required**, at most 262144 UTF-8 bytes. Result
`{}`. Explicitly writes phone clipboard; does not automatically paste or focus UI.
Do not use this to simulate ordinary keyboard input silently.

### `clipboard.get`

Returned text is limited to 262144 bytes, matching `clipboard.set`. Oversized
device text returns `operation_failed` before copying it into the RPC result.

Session required. No params. Result `{text:string}`; contains clipboard contents.
Treat this response as user-sensitive. Phone policy may reject clipboard access.

### `input.tap`

Session required. `x`, `y`: finite numbers **required**, each 0..1 in device panel
axes; `hold_ms`: integer 0..2000, default 30. Result `{}` after DOWN/UP report delivery.
No UI hit-test confirmation. Coordinates are not client-window pixels.

### `input.touch`

Session required. `x`, `y`: finite numbers **required**, 0..1; `down`: bool
**required**. Result `{}`. Repeated down reports move the single contact; false
releases it. Callers must release when focus/gesture ends. Final session cleanup
attempts release; keeping another attachment alive keeps the shared session alive.

### `input.keyboard`

Session required. `usages`: integer array **required**, each usage 0..239.
Result `{}`. Replaces the full USB HID keyboard usage set; `[]` releases all keys.
This is physical-key input using phone layout/IME, not Unicode insertion. Compose
chords by sending modifier-only state, then modifier+key state, then release;
simultaneous modifier+key alone is not reliable on the tested phone. Delivery
completion does not mean the phone's UI has consumed the report.

### `input.text`

Session required. `text`: string **required**, at most 4096 bytes; printable
US-keyboard ASCII 32..126, tab and newline only. Entire request is rejected with
`unsupported` before typing if any character is outside this set.
`hold_ms`: integer 0..1000, default 0; delay after each generated HID report,
including modifier/release reports. `timeout_ms`: integer 1..300000, default 30000;
one total budget across HID connection, report delivery and holds. Result `{}`. Does not use clipboard, choose
an input field or support host IME/Unicode. Focus and suitable phone keyboard
layout are caller preconditions; UI processing may lag behind delivery. Longer
text/hold may exhaust the budget and return operation_failed after partial input.
Cancellation interrupts only this request's HID endpoint and wakes holds; it does
not roll back typed characters or guarantee a final release reached the phone.
After a timeout, send input.keyboard with [] if a release is needed; after socket
interruption, reconnect/attach before sending it. No further text reports are
intentionally sent after cancellation is observed.

### `input.button`

Session required. `usage`: integer **required**, 0..65535; `down`: bool **required**;
`page`: integer 0..65535, default 12 (consumer page). Result `{}`. DOWN and release
are separate calls. Availability is device/service dependent; do not assume all
Android key actions have an iOS equivalent. Held buttons are released best-effort
when the last session attachment goes away.

## Screenshot and video

### `screenshot`

Session required. `format`: string, default png; supported caller values png/bgra.
Result for PNG: `{format:"png",display:Display}`, raw PNG binary. For BGRA:
`{format:"bgra",display:Display,width:integer,height:integer,row_pitch:integer}`,
raw BGRA binary. BGRA dimensions come from decoded PNG; respect row_pitch.
PNG-to-BGRA decoding limits both encoded input and packed BGRA output to 64 MiB;
the pixel limit is applied before the decoder allocates the image.
Waits for initial display geometry, captures, then checks geometry equality. A
change gives `geometry_changed` and discards that capture. No audio/video pump is
required. Other format strings return invalid_params before initiating capture.

### `video.start`

| Param | Type / default |
| --- | --- |
| hardware_decode | bool; default false; prefer platform decoder where supported |
| decode | bool; default true |
| record_path | string; default empty; nonempty must be an absolute raw HEVC output path |
| restart_on_silence | bool; default true; enables a 3000 ms silence restart threshold |

Session required. Result `{}`. Starts a latest-frame producer; decoded start does
not wait for the first frame. Use video.frame to wait for data. Encoded-only start
waits at most five seconds for checked recording readiness, resetting the video
pump on failure. Builds without a decoder reject decode=true with `unsupported`.
A repeated call while running reuses the current pump: it does not apply new
options. Stop and start to change options; serialize control from attachments.

### `video.frame`

Session and video pump required. `since`: uint, default 0; `timeout_ms`: integer
0..1000, default 50; `allow_cached`: bool, default false. Result
`{format:"bgra",serial:uint,width:integer,height:integer,row_pitch:integer}` plus
optional `display:Display`, raw BGRA pixels. Decoded frame must be newer than since,
except allow_cached may return a healthy cached frame after wait expiry. Compare
serials rather than counting repeated cached frames as new. No frame -> `no_frame`.
Encoded-only video has no BGRA producer; do not poll it for decoded frames.

Pixels are byte-ordered B,G,R,A. Rows include row_pitch bytes, potentially padding;
validate dimensions/length and do not assume width*4 is the row pitch. Codec frame
size may exceed visible panel size. Display metadata is captured with the frame;
never combine old pixels with freshly queried orientation to map input.

### `video.stop`

Session required. No params; result `{}`, idempotent. Stops the shared video pump,
not audio. Does not report finalization errors of the full recording session;
media.finish is the explicit finalization/error boundary.

## Audio and recording

Audio and video share device-stop ownership regardless of startup order. Stopping
one pump while the other remains active stops only its local reception/RTCP
renewal. The stopped device stream can remain until its lease expires; no
`stopAll` is sent during this partial stop or a sibling's video recovery. The
last pump's cleanup can stop device media globally.

### `audio.start`

Session required. `decode`: bool default true; `duplicate`: bool default false;
`buffer_ms`: integer 0..1000, default 50. Result `{sample_rate:uint,channels:uint,
preroll:uint}`; preroll is **frames per channel**, not milliseconds or bytes.
Default requests routing to the computer; duplicate requests phone+computer feed.
No fallback to duplicate if default routing fails. A player can pause on route
change; XDB does not automatically resume it or adjust device volume. Caller
must actually consume/output PCM to get computer sound. Success does not prove
there is currently non-silent music. A repeated start reuses the running pump
without applying new options. Stop/start to change them.

### `audio.read`

Session and audio decode=true required. `frames`: integer 1..48000, default 960.
Result `{buffered:uint,frames:uint}` and interleaved signed 16-bit **little-endian**
PCM. Size is actual frames * channels * 2. The returned frame count may be less
than requested, including zero. Before startup preroll is filled, returns zero
frames without consuming. Buffered is a diagnostic snapshot, not a reservation.
Read on a worker, never synchronously from a real-time output callback. Only one
consumer should drain a shared session queue. Audio with decode=false can feed
recording but cannot be read through this method.

### `audio.stop`

Session required. No params; result `{}`, idempotent. Stops audio, resets its pump;
video can remain active. This is not the container finalization boundary. Phone
playback restoration may require the user to resume the player after a route change.

### `recording.configure`

| Param | Type / default |
| --- | --- |
| path | absolute server-host string **required** |
| format | string mkv (default) or mp4 |
| video | bool; default true |
| audio | bool; default false |
| orientation | integer; default 0; supported 0/90/180/270 |
| orientation_from_device | bool; default false |

Session required, before video/audio/recorder exist. Result `{}`. At least one
track must be selected. Audio-only requires zero orientation. Unsupported angles,
container-specific rotation, missing muxer or invalid recorder configuration may
return `operation_failed`; explicit format/path validation gives `invalid_params`.
With orientation_from_device, the backend waits for valid initial display geometry;
a recognized device rotation overrides the numeric orientation, including resetting
it to zero for portrait. Unknown/unavailable initial orientation fails instead of
silently using the numeric fallback. No automatic rotation/reencoding/letterboxing.

Configuration does not start the media tracks. Start selected video/audio producers
next, then call media.finish to flush/finalize. Container tracks remain original
encoded HEVC/AAC-ELD; IPC BGRA/PCM is not reencoded. Output files are created exclusively
when the recorder opens output; do not silently retry a failed mutation on the same
path assuming atomic rollback. Decoding and transport can continue after a
recording failure; inspect recording_error and finish explicitly.

Within the same session, video packet loss, malformed or oversized access units
are discarded. Recording skips dependent pictures until a valid HEVC IDR with
supported parameters arrives; audio continues meanwhile. Audio packet loss skips
the missing packets. Original timestamps are retained, so gaps are not compressed
or filled with synthetic media; players may hold the last video picture.
Recoverable drops do not set recording_error. Session/source changes, unsupported
codec configurations, invalid clocks, shared queue exhaustion and file I/O errors
still end the recording. If no usable initial media/clock arrives within the
startup budget, recording startup still fails.

### `media.status`

Session required. No params. Result `{error:string,recording_error:string,
video_unusable:bool}`. Empty strings mean no currently reported terminal failure,
not proof of recent activity. video_unusable allows screenshot fallback. Recording
error is separate so a consumer can continue mirroring when saving fails.

### `media.finish`

Stopping a media pump cancels its recovery/status RPC waits before joining.
Device stop RPC cleanup has a two-second total budget, including connection
setup; timeout may leave the device stream until its RTCP lease expires.
This does not impose a total deadline on disk finalization or decoding.

Session required. No params; result `{}` or `operation_failed`. Stops receiving,
joins video recording, finalizes the recorder, then stops audio and resets the
media objects. Shared attachments see the stopped state. Prefer this before
connection.close when output integrity/failure reporting matters. Repeating after
successful finish is safe; it does not close the device session.

## Applications

### `app.list`

Session required. No params. Result array `[{bundle_id:string,name:string},...]`;
this is not an object wrapper. Availability/list contents depend on phone services.
No guarantee that listing an app makes every launch request permissible.

### `app.launch`

Session required. Nonempty `bundle_id`: string **required**;
`terminate_existing`: bool default false. Result `{}`; device launch request was
accepted, not proof UI is rendered/focused. This can change the user's foreground app.

### `app.stop`

Session required. Nonempty `bundle_id`: string **required**. Result `{}` after
requesting app termination; failures are operation_failed. No launch/stop rollback.

## Example sequences

Each line below is one request on the **same** already-handshaken socket. CLI
`xdb rpc` invocations are separate connections; use rpc-shell or a persistent SDK
client for a sequence. Replace placeholders, never log real attachment/PIN values.

Screenshot and control:

```json
{"id":2,"method":"connection.open","params":{"udid":"DEVICE_UDID","transport":"usb"}}
{"id":3,"method":"display.info","params":{}}
{"id":4,"method":"screenshot","params":{"format":"png"}}
{"id":5,"method":"input.keyboard","params":{"usages":[4]}}
{"id":6,"method":"input.keyboard","params":{"usages":[]}}
{"id":7,"method":"connection.close","params":{}}
```

Video + audio use an owning connection to open/configure/start, and a second
client to attach with the returned attachment. Poll video.frame from one socket,
audio.read from another; schedule both outside sound callbacks. A container flow:

```json
{"id":2,"method":"connection.open","params":{"udid":"DEVICE_UDID"}}
{"id":3,"method":"display.info","params":{}}
{"id":4,"method":"recording.configure","params":{"path":"/absolute/session.mkv","video":true,"audio":true,"orientation_from_device":true}}
{"id":5,"method":"audio.start","params":{"duplicate":true,"decode":true,"buffer_ms":200}}
{"id":6,"method":"video.start","params":{"decode":true}}
{"id":7,"method":"media.status","params":{}}
{"id":8,"method":"media.finish","params":{}}
{"id":9,"method":"connection.close","params":{}}
```

## Compatibility and validation

XDB 1.0 established C ABI 1 and wire protocol 1. Package versions are separate
from these contracts; see the [version and upgrade policy](PROTOCOL.md).

- All four released 1.0.0/1.1.0 client/server combinations passed the selected
  C API and RPC checks on macOS ARM64. This does not establish cross-version
  live-media or Windows/Linux acceptance; see the [validation records](VALIDATION.md).
- A successful hello confirms protocol compatibility, not device capabilities
  or the availability of new methods. Clients must handle operation failures
  and upgrade older servers when new methods are required.
- Device, media, cancellation and deadline validation remains limited to the
  platforms, devices and operations documented in the validation records.

## Recording output paths

Recording outputs (`recording.configure.path` and `video.start.record_path`) must be new absolute local files. Existing files and symlinks are refused atomically; remove or rename an old recording explicitly before reusing its path. Failed recordings may leave a partial new file.
