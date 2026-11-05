# XDB C SDK reference — 1.0

English | [简体中文](C_API_zh.md)

The exported header is [xdb.h](../include/xdb/xdb.h). This reference defines the
published C ABI 1 contract for XDB 1.0. RPC parameters
and results are specified in [API.md](API.md), framing/version policy in
[PROTOCOL.md](PROTOCOL.md). `client.hpp` is an optional source-level C++ convenience
adapter; neither nlohmann-json nor C++ objects cross the binary C boundary.

## Libraries and types

| Library | Exported API | Host dependencies |
| --- | --- | --- |
| xdb_client | xdb_client_*, xdb_response_*, xdb_last_error | Local socket/JSON/platform runtime; no device crypto/media backend |
| xdb_server | xdb_server_* | Device protocols, DDI, crypto and media; needed only by embedding hosts |

`xdb_client`, `xdb_response` and `xdb_server` are opaque types. Never allocate,
copy, inspect their layout, or free them with the caller's allocator. Each library
must destroy its own objects, particularly across Windows CRT boundaries. There
is no stable ABI for internal `src/` C++ classes. Build/package artifacts must
retain their dependent shared libraries; do not copy just one executable/DLL.

All strings are NUL-terminated UTF-8. The C API cannot represent embedded NUL in
method/JSON/path arguments. JSON can represent escaped characters but downstream
method-specific validation still applies. Sizes are bytes unless explicitly
called frames; `size_t` follows the target architecture, not a fixed 32-bit value.
Use the header's platform import/export declarations and native C calling ABI.

## Version information

`xdb.h` includes CMake-generated `xdb/version.h`, providing `XDB_VERSION_MAJOR`,
`XDB_VERSION_MINOR`, `XDB_VERSION_PATCH`, `XDB_VERSION_STRING` and `XDB_ABI_VERSION`.
Header versions describe the build-time SDK; query the loaded library at runtime.
Shared-library SOVERSION follows the ABI major version.

### `xdb_client_version`

```c
const char *xdb_client_version(void);
```

Returns the loaded client library version in static storage. No server connection
or caller cleanup is required.

### `xdb_client_server_version`

```c
const char *xdb_client_server_version(const xdb_client *client);
```

Returns the version advertised during hello, borrowed until client destruction.
A NULL client returns NULL. A compatible older server omitting the optional version
field yields an empty string. Package version differences alone do not reject a
protocol-compatible server.

## Lifetime and threading

1. The caller owns a returned client until `xdb_client_destroy` and each returned
   response until `xdb_response_destroy`. Destroy every successful response,
   **including RPC errors with `ok:false`**.
2. Response JSON/binary accessors borrow response-owned memory; copy it if needed
   after destroy. A response is self-contained and can outlive the client.
3. One outstanding request per client. Do not rely on the implementation mutex as
   an ordering contract for concurrent callers. Use separate attached clients for
   concurrent audio, video and control; shared session rules still apply.
4. Event callbacks run synchronously on the request caller's thread, before the
   terminal response. Callback JSON is borrowed only during the callback. Userdata
   is caller-owned. Do not reenter, interrupt or destroy the same client inside
   its callback; dispatch cancellation to another thread. Do not throw exceptions
   across the C callback boundary.
5. `xdb_client_interrupt` may run from another thread while a request blocks. It
   aborts the socket and makes that client unusable. Wait for the request **and**
   interrupt caller to return before destroying/replacing the handle. This also
   applies when using the C++ adapter; its atomic pointer is not a lifetime join.
6. `destroy` is not a concurrent cancellation mechanism. Server stop joins workers;
   do not call it from a callback or while another thread accesses its handle.
7. Last-error strings are thread-local borrowed diagnostics. Read/copy them on
   the thread where failure happened before another API operation; never free
   or retain the pointer as permanent storage. Do not branch on diagnostic text.

## Client API

### `xdb_last_error`

```c
const char *xdb_last_error(void);
```

Returns the calling thread's client diagnostic. Used after NULL/0 failure from
client connect/start/kill/request. A well-formed RPC failure is instead in its
response JSON; this function does not translate `ok:false` into an error string.

### `xdb_client_connect`

```c
xdb_client *xdb_client_connect(uint16_t port, xdb_event_callback event, void *userdata);
```

`port`: 1..65535 chooses the loopback port. Zero uses nonempty `XDB_PORT` (decimal
1..65535), otherwise 17423; invalid environment text fails. `event`: optional NULL
callback. `userdata`: passed through without ownership transfer. Returns a
handshaken owned client or NULL (inspect xdb_last_error).

Desktop: connects to an existing server; only a genuinely missing listener triggers
automatic background startup. `XDB_EXECUTABLE` is tried first (an invalid override
fails), followed by the client module's directory, adjacent `bin`, then PATH.
Startup retries are bounded at five seconds; handshake reads have a five-second
timeout. These are distinct stages, not one universal end-to-end deadline.
An occupied non-XDB or incompatible listener is not killed/replaced. Use its old
executable's kill-server explicitly when upgrading.

Android: requires an application-owned embedded server; no automatic process
launch. Destroying a desktop client does not stop its detached background server.

### `xdb_client_connect_cancellable`

```c
typedef int (*xdb_cancel_callback)(void *userdata);
xdb_client *xdb_client_connect_cancellable(
    uint16_t port, xdb_event_callback event, void *userdata,
    xdb_cancel_callback cancel, void *cancel_userdata);
```

Uses the same endpoint and ownership rules as `xdb_client_connect`. A non-NULL
`cancel` enables a shared 15-second budget covering startup-lock acquisition,
server startup/retries, loopback connection and the hello handshake. Returning
nonzero requests cancellation. NULL keeps the ordinary connect behavior.

The cancel callback may run on a scoped watchdog thread. It must be thread-safe,
must not throw, and must not reenter this connection operation. The caller owns
`cancel_userdata` until the call returns. All watchdogs are joined before return,
so neither the callback nor its userdata is accessed afterward.

Failure returns NULL with a calling-thread `xdb_last_error` diagnostic. Cancellation
closes the pending client socket. A shared server launched during this attempt
can remain running. A cancellation racing with successful completion can return a
valid client, which the caller owns and must destroy if no longer needed.

### `xdb_client_start_server`

```c
int xdb_client_start_server(uint16_t port, const char *data_directory);
```

Ensures a compatible server is reachable; returns 1 on success, 0 on failure.
Port semantics match connect. `data_directory`: NULL/empty uses CLI's platform
user-data default on a newly started desktop server; a nonempty path must be
absolute. Reuses an existing server **without changing its directory**; does not
restart it merely because a different package/path is installed. On Android,
only an already reachable embedded server can be used.

### `xdb_client_kill_server`

```c
int xdb_client_kill_server(uint16_t port);
```

Port semantics match connect. Returns 1 when absent or stopped; 0 on error. Does
not start an absent service. Requests server.stop, then waits at most five seconds
for the listener to close. Never force-kills an unrelated process or changes phone
pairing data. Existing sessions and clients are cancelled; callers must coordinate
with other consumers before invoking it. A port becoming free is the checked
condition, not proof every external device operation rolled back.

### `xdb_client_request`

```c
int xdb_client_request(xdb_client *client, const char *method,
                       const char *params_json, xdb_response **response);
```

`client`, `method` and `response` must be non-NULL. `params_json`: NULL means `{}`;
otherwise must parse as a JSON object. Strings need only remain valid for the
synchronous call. `*response` is set NULL first, then filled with an owned response
when a valid terminal envelope was received. Do not pass a still-owned response
without destroying it first.

**Return 1 means a response was received, not that the RPC succeeded.** Parse the
JSON `ok` boolean. On `ok:false`, inspect `error.code`/`message`, then destroy the
response normally. Return 0 means local validation, transport, framing, response
or exception failure; inspect xdb_last_error and do not assume a mutation failed
to reach the phone. Reconnect after transport/protocol failure. A local bad-params
request can fail before sending; the API does not provide a separate typed
transport-status enum yet.

No generic per-call timeout parameter. Method-specific discovery/DDI/pairing/frame/text
budgets do not cover every device service operation. Run requests off the UI/audio
thread and use interrupt from another thread if the application deadline expires.

### `xdb_client_interrupt`

```c
void xdb_client_interrupt(xdb_client *client);
```

NULL is a no-op. Interrupts the socket, not a transactional undo; client becomes
unusable. No response or cancellation callback is guaranteed. Pairing/install/file
changes already accepted by the device/server may persist. Join before destroy.

### `xdb_client_destroy`

```c
void xdb_client_destroy(xdb_client *client);
```

NULL is a no-op. Closes local connection and frees the handle; do not use it again.
Server cleanup is asynchronous relative to that local return. The device lease
is retained while another socket is attached. Explicit media.finish reports
recording finalization errors; disconnect/destructor cleanup does not return them.

## Response API

### `xdb_response_json`

```c
const char *xdb_response_json(const xdb_response *response);
```

Borrowed NUL-terminated UTF-8 envelope, valid until response destruction. NULL
response yields an empty string, which is not a JSON response. The envelope may
be successful or failed. Do not store its pointer after destroying response.

### `xdb_response_data`

```c
const uint8_t *xdb_response_data(const xdb_response *response);
```

Borrowed binary body, valid until destruction. NULL for no body or NULL response.
Never interpret as NUL-terminated text or free it. Layout depends on RPC result
(e.g. PNG bytes, padded BGRA rows, interleaved little-endian PCM).

### `xdb_response_size`

```c
size_t xdb_response_size(const xdb_response *response);
```

Number of binary bytes; zero for empty/NULL response. Validate against advertised
pixel dimensions/row pitch or frame/channel count before reading/copying. Use
checked arithmetic in language bindings; do not truncate size_t to int blindly.

### `xdb_response_destroy`

```c
void xdb_response_destroy(xdb_response *response);
```

NULL is a no-op. Invalidates all borrowed JSON/binary pointers. Exactly once per
owned response. Different responses are independent of later client requests.

## Embedded Server API

### `xdb_server_last_error`

```c
const char *xdb_server_last_error(void);
```

Calling thread's server diagnostic, borrowed. Use after xdb_server_start returns
NULL; independent of xdb_last_error in the client library.

### `xdb_server_start`

```c
xdb_server *xdb_server_start(uint16_t port, const char *data_directory);
```

Starts an owned threaded server bound only to IPv4 loopback; does not require an
already connected phone. Port zero selects a free port (unlike client port zero).
`data_directory` must be a nonempty absolute UTF-8 path controlled by the embedding
application; NULL/relative is rejected. Parent/data creation may occur later when
an operation needs persistent state. The server does not take ownership of the
caller's string. Returns owned handle or NULL; use server_last_error for failure.
A maximum of 16 client workers is accepted by the current implementation; do not
open unbounded attachment sockets. Bind failure never evicts another listener.

### `xdb_server_port`

```c
uint16_t xdb_server_port(const xdb_server *server);
```

Actual bound port, useful after start(0,...). Returns zero for NULL. Only valid
while the handle exists; does not itself prove listener health.

### `xdb_server_is_running`

```c
int xdb_server_is_running(const xdb_server *server);
```

Nonzero while the handle has not entered stopping state; zero for NULL/stopping.
This is a lifecycle flag, not a phone/transport health check. Remote server.stop
can change it while the owner still holds an allocated server handle.

### `xdb_server_stop`

```c
void xdb_server_stop(xdb_server *server);
```

NULL is a no-op. Stops listener, interrupts clients, joins workers and frees the
handle. Must be called once even after a remote stop. Can wait for device cleanup;
not safe from callbacks/concurrent access and not an instant hard-kill promise.
Existing phone pairing records and DDI cache are not removed.

## Minimal C example

A compilable version is [examples/client_info.c](../examples/client_info.c).
This shows ownership/response receipt; use a real JSON parser to inspect business
`ok` for device operations. Do not substring-match JSON or diagnostic messages.

```c
xdb_client *client = xdb_client_connect(0, NULL, NULL);
if (!client) { /* copy xdb_last_error() on this thread */ }
xdb_response *response = NULL;
if (client && xdb_client_request(client, "server.info", "{}", &response)) {
    /* Parse xdb_response_json(response); inspect `ok` and `result`. */
    /* Consume/copy any binary body BEFORE destroying response. */
    xdb_response_destroy(response);
}
xdb_client_destroy(client);
```

## 1.0 freeze checklist

- Freeze the exported symbol/signature inventory and define an explicit ABI
  revision/compatibility mechanism before promising binary mixing across releases.
- Review threading/cancellation/error semantics above and bounded device operations.
- Verify old baseline client/new server and bindings' size/lifetime handling.
- Keep C-only example compilation and RPC/symbol documentation coverage checks in CI;
  these check documentation drift, not hardware acceptance or semantic compatibility.
