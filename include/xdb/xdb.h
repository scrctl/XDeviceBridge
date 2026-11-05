// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "xdb/version.h"
#ifdef _WIN32
#ifdef XDB_CLIENT_EXPORTS
#define XDB_CLIENT_API __declspec(dllexport)
#else
#define XDB_CLIENT_API __declspec(dllimport)
#endif
#ifdef XDB_SERVER_EXPORTS
#define XDB_SERVER_API __declspec(dllexport)
#else
#define XDB_SERVER_API __declspec(dllimport)
#endif
#else
#define XDB_CLIENT_API __attribute__((visibility("default")))
#define XDB_SERVER_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct xdb_server xdb_server;
typedef struct xdb_client xdb_client;
typedef struct xdb_response xdb_response;
typedef int (*xdb_cancel_callback)(void *userdata);
// Events run synchronously within request(). Data is borrowed until return.
// Do not reenter or destroy this client from its event callback.
typedef void (*xdb_event_callback)(const char *json, void *userdata);
// Library version: static storage, no server connection required.
XDB_CLIENT_API const char *xdb_client_version(void);
// Version advertised during hello; borrowed until client destruction. NULL for a NULL client.
// An older compatible server without this optional field yields an empty string.
XDB_CLIENT_API const char *xdb_client_server_version(const xdb_client *client);
// Thread-local diagnostic, borrowed until the next API call on this thread.
XDB_CLIENT_API const char *xdb_last_error(void);
// Loopback only. Port 0 selects a free port. Local clients connect without authentication.
// data_directory must be an absolute path owned by the embedding application.
// Stop joins all workers. Must not be called from an event callback.
XDB_SERVER_API const char *xdb_server_last_error(void);
XDB_SERVER_API xdb_server *xdb_server_start(uint16_t port, const char *data_directory);
XDB_SERVER_API uint16_t xdb_server_port(const xdb_server *server);
// A remote kill request stops listening and interrupts clients. The owner still calls stop().
XDB_SERVER_API int xdb_server_is_running(const xdb_server *server);
XDB_SERVER_API void xdb_server_stop(xdb_server *server);
// Desktop: ensure a background server exists. Optional data_directory must be absolute.
// Existing servers are reused without changing their directory. Android requires an embedded server.
XDB_CLIENT_API int xdb_client_start_server(uint16_t port, const char *data_directory);
// Idempotent; never starts a missing server. Cancels all current server sessions.
XDB_CLIENT_API int xdb_client_kill_server(uint16_t port);
// Desktop connections automatically start a missing server; Android connects to the App's server.
// Port 0 uses XDB_PORT or 17423.
XDB_CLIENT_API xdb_client *xdb_client_connect(uint16_t port, xdb_event_callback event, void *userdata);
// Cancellation covers startup, loopback connection and hello with a total 15-second budget.
// The callback may run on
// a scoped watchdog thread and must be thread-safe. No callbacks run after return.
// Cancellation leaves an already launched shared server running.
XDB_CLIENT_API xdb_client *xdb_client_connect_cancellable(uint16_t port, xdb_event_callback event,
    void *userdata, xdb_cancel_callback cancel, void *cancel_userdata);
// One outstanding request per client. Response owns JSON and any binary payload.
// Returns 1 when a well-formed response was received; inspect its `ok` field.
XDB_CLIENT_API int xdb_client_request(xdb_client *client, const char *method, const char *params_json,
                                      xdb_response **response);
// May interrupt an in-flight request from another thread. Client becomes unusable.
XDB_CLIENT_API void xdb_client_interrupt(xdb_client *client);
// Call after requests/interrupt callers have returned.
XDB_CLIENT_API void xdb_client_destroy(xdb_client *client);
XDB_CLIENT_API const char *xdb_response_json(const xdb_response *response);
XDB_CLIENT_API const uint8_t *xdb_response_data(const xdb_response *response);
XDB_CLIENT_API size_t xdb_response_size(const xdb_response *response);
XDB_CLIENT_API void xdb_response_destroy(xdb_response *response);
#ifdef __cplusplus
}
#endif
