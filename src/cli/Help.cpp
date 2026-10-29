// SPDX-License-Identifier: LGPL-3.0-only
#include "cli/Help.h"
#include "xdb/version.h"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace xdb::cli {
namespace {
struct CommandHelp {
    std::string_view name;
    std::string_view usage;
    std::string_view details;
};

constexpr CommandHelp commands[] = {
    {"help", "xdb help [COMMAND...]",
     "Show the command overview, or detailed help for a command.\n\n"
     "Examples:\n  xdb help\n  xdb help devices\n  xdb help ddi prepare\n"},
    {"version", "xdb version",
     "Show the local executable version, installation path and running platform.\n"
     "This does not start or connect to a server. Use xdb rpc server.info\n"
     "to query the running server version.\n"},
    {"get-state", "xdb [-d|-e] [-s UDID] get-state [--wifi ADDRESS] [--timeout MS] [--json]",
     "Print discovered, paired or connected for the selected device.\n\n"
     "Uses the same discovery snapshot as devices, without opening a device session.\n"
     "paired means a matching local record; connected means an authenticated\n"
     "server connection/session. This is not an active liveness probe.\n"
     "A sole manual connection is preferred over unrelated discovery.\n"
     "Missing or ambiguous devices fail without printing a state.\n\n"
     "Example:\n  xdb -s UDID get-state\n"},
    {"get-serialno", "xdb [-d|-e] [-s UDID] get-serialno [--wifi ADDRESS] [--timeout MS] [--json]",
     "Print the UDID of a selected discovered or manually connected device.\n\n"
     "Use -s UDID or XDB_SERIAL when several devices are available.\n"
     "A sole manual connection is preferred; otherwise select a sole device.\n"
     "No device session is opened.\n"
     "Missing or ambiguous devices fail without printing a UDID.\n\n"
     "Examples:\n  xdb get-serialno\n  xdb -d get-serialno\n  xdb -e get-serialno\n"},
    {"wait-for-device", "xdb [-d|-e] [-s UDID] wait-for-device [--wifi ADDRESS] [--timeout MS] [--json]",
     "Wait until the selected device accepts a read-only development connection.\n\n"
     "Opens and closes a temporary read-only session; starts no media, sends no input\n"
     "and does not pair, prepare DDI or keep a manual connection. Prepare DDI first.\n"
     "Uses -s or XDB_SERIAL, or waits for a sole identified device. Ambiguity fails.\n"
     "Connection failures are retried without changing pairing records.\n"
     "Default/--timeout 0 waits indefinitely; 1..300000 bounds the entire wait\n"
     "after connecting to the local Server, including pending device RPCs.\n"
     "Success is silent (or JSON); timeout exits 124, Ctrl+C exits 130.\n\n"
     "Example:\n  xdb -e wait-for-device --timeout 30000\n"},
    {"mdns", "xdb mdns <check|services> [--timeout MS] [--json]",
     "Diagnose the Server's iOS service discovery without connecting to devices.\n\n"
     "  check          Report the attempted backend, availability and warnings\n"
     "  services       Also list discovered _remotepairing._tcp.local. endpoints\n"
     "  --timeout MS   Maximum discovery wait (default: 3000; range: 1..60000)\n\n"
     "Uses the Server subscription cache when available, or a bounded query.\n"
     "Availability means discovery can run, not that an iPhone must be found.\n"
     "An empty service list is success when the backend is available. No pairing\n"
     "records are read, no authentication is attempted and no media is started.\n\n"
     "Examples:\n  xdb mdns check\n  xdb mdns services\n"},
    {"devices", "xdb devices [-l] [-d|-e] [--timeout MS] [--json]",
     "List discovered and manually connected devices without opening new sessions.\n\n"
     "  -l             Show names, addresses, interfaces and pairing details\n"
     "  -d, --usb      Discover USB devices only\n"
     "  -e, --wifi     Discover network devices only\n"
     "  --timeout MS   Discovery timeout (default: 3000; range: 0..60000)\n"
     "  --json         Print the complete JSON response\n\n"
     "States:\n"
     "  discovered     Discovered without a verified local Wi-Fi record\n"
     "  paired         A local record matches discovery; connectivity is unverified\n"
     "  connected      The server holds an authenticated connection or device session\n\n"
     "Pairing records do not prove connectivity. Use -l for candidate details.\n\n"
     "Examples:\n  xdb devices\n  xdb devices -l --wifi\n"},
    {"connect", "xdb [-s UDID] connect HOST[:PORT] [--timeout MS] [--json]",
     "Authenticate a paired Wi-Fi device and keep its connection in the server.\n\n"
     "  HOST[:PORT]    Device RemotePairing endpoint (default port: 49152)\n"
     "  --timeout MS   Connection timeout (default: 30000; range: 1..300000)\n\n"
     "No multicast discovery is needed for an explicit IP. A sole usable local\n"
     "pairing record is selected without -s; multiple records require -s UDID.\n"
     "The connection survives CLI exit and can be reused by device sessions.\n"
     "It starts no media and does not pair or prepare DDI. Repeat connect is idempotent.\n"
     "Disconnect, transport loss or server exit closes it. There is no auto-reconnect.\n\n"
     "Examples:\n  xdb connect 192.168.1.10\n  xdb -s UDID connect '[fe80::1%en0]:49152'\n"},
    {"disconnect", "xdb [-s UDID] disconnect [HOST[:PORT]] [--json]",
     "Close manually connected Wi-Fi devices and their active device sessions.\n\n"
     "With no endpoint or -s, disconnect all manually connected devices.\n"
     "Pairing records and DDI are preserved. Already disconnected is success.\n"
     "An endpoint and -s together must both match.\n\n"
     "Examples:\n  xdb disconnect 192.168.1.10\n  xdb -s UDID disconnect\n  xdb disconnect\n"},
    {"pair", "xdb [-d|-e] [-s UDID] pair [--display-brand NAME] [--timeout MS] [--json]",
     "Pair with an iOS device. Wi-Fi pairing is the default.\n\n"
     "  -e, --wifi     Advertise this computer for Wi-Fi pairing\n"
     "  -d, --usb      Pair through the system USB device service\n"
     "  --display-brand NAME  Display prefix for new pairing (default: xdb)\n"
     "  --timeout MS   Pairing timeout (default: 120000; range: 1..300000)\n\n"
     "For Wi-Fi, open Settings > Privacy & Security > Developer Mode on the device,\n"
     "select this computer and enter the one-use code printed by xdb.\n"
     "USB pairing requires device trust. The server saves pairing credentials.\n\n"
     "Examples:\n  xdb pair --wifi\n  xdb -s UDID pair --usb\n"},
    {"ddi", "xdb [-s UDID] ddi <prepare|status|remove> [--usb|--wifi [ADDRESS]] [--json]",
     "Manage the developer disk image on a paired device.\n\n"
     "  prepare           Download and mount; reuse an applicable mounted image\n"
     "  status            Query DDI installation and mount state\n"
     "  remove            Remove the device DDI; already absent is success\n\n"
     "  --usb             Use USB (the default for DDI operations)\n"
     "  --wifi [ADDRESS]  Use paired Wi-Fi discovery, or an explicit device address\n"
     "  --cache-dir DIR   prepare: absolute cache directory on the server host\n"
     "  --replace         prepare: allow replacement of an already mounted image\n\n"
     "Without --cache-dir, downloads use temporary storage.\n"
     "remove unmounts Classic/Personalized images or uninstalls the DDI Cryptex.\n"
     "It verifies removal and preserves pairing records and host caches.\n"
     "Close active sessions and disconnect manual connections before managing DDI.\n"
     "A sole identified device is selected when -s is omitted.\n\n"
     "Examples:\n  xdb ddi status\n  xdb ddi remove --wifi\n"
     "  xdb -s UDID ddi prepare --wifi --cache-dir /absolute/ddi-cache\n"},
    {"screenshot", "xdb [-s UDID] screenshot FILE [--usb|--wifi [ADDRESS]]",
     "Save a lossless PNG screenshot to FILE on the client host.\n\n"
     "  --usb             Connect through USB only\n"
     "  --wifi [ADDRESS]  Use paired Wi-Fi discovery, or an explicit device address\n\n"
     "With a supporting Server, capture can run alongside an existing mirror.\n"
     "Read access never controls input or changes media. Older Servers remain exclusive.\n\n"
     "Prepare pairing and DDI before capture. Without a transport option,\n"
     "the device session reuses a manual connection, or prefers USB then paired Wi-Fi.\n"
     "A sole identified device is selected when -s is omitted.\n\n"
     "Examples:\n  xdb screenshot screen.png\n  xdb -s UDID screenshot screen.png --wifi\n"},
    {"start-server", "xdb start-server [--data-dir DIR] [--port PORT]",
     "Start or reuse the background server. Clients also start it automatically.\n\n"
     "  --data-dir DIR  Pairing data directory for a new server\n"
     "  --port PORT     Fixed loopback port (default: XDB_PORT or 17423)\n\n"
     "--data-dir does not change an existing server's data directory.\n\n"
     "Example:\n  xdb start-server\n"},
    {"kill-server", "xdb kill-server [--port PORT]",
     "Stop the server and cancel its active sessions.\n\n"
     "  --port PORT  Fixed loopback port (default: XDB_PORT or 17423)\n\n"
     "Succeeds without starting a server when none is running.\n\n"
     "Example:\n  xdb kill-server\n"},
    {"server", "xdb server [--data-dir DIR] [--port PORT]",
     "Run the server in the foreground for diagnostics. Stop with Ctrl+C.\n\n"
     "  --data-dir DIR  Pairing data directory (default: platform user data)\n"
     "  --port PORT     Loopback port (default: XDB_PORT or 17423)\n"
     "                  Use 0 to select a free port; the ready response reports it\n\n"
     "The server listens on IPv4 loopback only. Local clients are not authenticated\n"
     "by operating-system user or process.\n\n"
     "Examples:\n  xdb server\n  xdb server --port 0\n"},
    {"rpc", "xdb rpc METHOD [JSON_PARAMS] [--output FILE]",
     "Send a raw RPC request and print its JSON response.\n\n"
     "  JSON_PARAMS    JSON parameters (default: {})\n"
     "  --output FILE  Save a binary response to a client-side file\n\n"
     "A binary response requires --output. A single invocation owns one client\n"
     "connection; use rpc-shell for multiple requests in the same session.\n\n"
     "Example:\n  xdb rpc server.info\n"},
    {"rpc-shell", "xdb rpc-shell [--output FILE]",
     "Open the JSON debugging console, keeping one client session until EOF.\n"
     "This is not an iOS command shell. Enter one JSON request per line.\n\n"
     "  --output FILE  Save binary responses to this client-side file\n\n"
     "Malformed input is reported without ending the console.\n\n"
     "Example input:\n  {\"method\":\"server.info\",\"params\":{}}\n"},
};
}

void print_help(std::span<const std::string> command) {
    if (command.empty()) {
        std::cout << "XDeviceBridge " XDB_VERSION_STRING "\n\n"
                     "Usage: xdb [options] <command> [args]\n\n"
                     "Commands:\n"
                     "  help [COMMAND...]   Show command help\n"
                     "  devices [-l]        List discovered devices\n"
                     "  mdns check/services Diagnose iOS service discovery\n"
                     "  get-state           Print the selected device state\n"
                     "  get-serialno        Print the selected device UDID\n"
                     "  wait-for-device     Wait for a connectable device\n"
                     "  pair                Pair with an iOS device\n"
                     "  connect HOST[:PORT] Keep a paired Wi-Fi connection in the server\n"
                     "  disconnect [HOST]   Close manual Wi-Fi connections\n"
                     "  ddi prepare         Download and mount a developer disk image\n"
                     "  ddi status          Query developer image installation/mount state\n"
                     "  ddi remove          Remove the developer image from the device\n"
                     "  screenshot FILE     Save a screenshot\n"
                     "  start-server        Start the background server\n"
                     "  kill-server         Stop the server\n"
                     "  server              Run the server in the foreground\n"
                     "  version             Show the local executable version\n"
                     "  rpc                 Send a JSON RPC request\n"
                     "  rpc-shell           Open the JSON debugging console\n\n"
                     "Options:\n"
                     "  -d, --usb           Use USB transport\n"
                     "  -e                  Use network transport\n"
                     "  --wifi [ADDRESS]    Use network transport, optionally by address\n"
                     "  -s, --serial UDID   Select a device (overrides XDB_SERIAL)\n"
                     "  --port PORT         Server port (default: 17423; env: XDB_PORT)\n"
                     "  --data-dir DIR      Pairing data directory for a new server\n"
                     "  --json              Print JSON responses\n"
                     "  -h, --help          Show help for the selected command\n"
                     "  --version           Show the local executable version\n\n"
                     "The server starts automatically when needed.\n"
                     "A sole discovered device is selected when -s is omitted.\n"
                     "Run 'xdb <command> --help' for command options and examples.\n";
        return;
    }
    if (command[0] == "ddi" && command.size() > 1 && command[1] != "prepare" &&
        command[1] != "status" && command[1] != "remove")
        throw std::invalid_argument("Unknown ddi command; run xdb ddi --help");
    if (command[0] == "mdns" && command.size() > 1 && command[1] != "check" && command[1] != "services")
        throw std::invalid_argument("Unknown mdns command; run xdb mdns --help");
    for (const auto &entry : commands) {
        if (command[0] != entry.name) continue;
        std::cout << "Usage: " << entry.usage << "\n\n" << entry.details
                  << "\nUse --port PORT or XDB_PORT to select the local server.\n";
        return;
    }
    throw std::invalid_argument("Unknown command; run xdb --help");
}
}
