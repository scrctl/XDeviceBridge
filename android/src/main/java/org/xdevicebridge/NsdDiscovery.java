// SPDX-License-Identifier: LGPL-3.0-only
package org.xdevicebridge;

import android.annotation.TargetApi;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Base64;
import org.json.JSONArray;
import org.json.JSONObject;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.Executor;

/**
 * Process-shared NSD lease for embedded xdb servers. Acquire before starting a
 * server; close after its clients/workers have stopped. No Activity is retained.
 * API 34+ uses service-info subscriptions; older releases keep direct mDNS.
 * Compile this source into the host App (compileSdk >= 34) and load xdb_server.
 */
public final class NsdDiscovery implements AutoCloseable {
    private static Backend shared;
    private static int leases;
    private boolean released;
    private static native void install(Object backend);

    public static synchronized NsdDiscovery acquire(Context context) {
        if (Build.VERSION.SDK_INT >= 34 && shared == null) {
            System.loadLibrary("xdb_server");
            Backend next = new Backend(context.getApplicationContext());
            try { install(next); shared = next; }
            catch (RuntimeException | Error error) { next.close(); throw error; }
        }
        ++leases;
        return new NsdDiscovery();
    }
    private NsdDiscovery() {}
    @Override public void close() {
        synchronized (NsdDiscovery.class) {
            if (released) return;
            released = true;
            if (--leases == 0 && shared != null) {
                install(null);
                shared.close();
                shared = null;
            }
        }
    }
    // Diagnostic/test surface, contains advertisements only, never credentials.
    public static synchronized String status() {
        return shared == null ? "{\"available\":false,\"services\":[]}" :
            new String(shared.snapshot(), java.nio.charset.StandardCharsets.UTF_8);
    }

    @TargetApi(34)
    private static final class Backend implements AutoCloseable {
        private static final String TYPE = "_remotepairing._tcp.";
        private final NsdManager nsd;
        private final ConnectivityManager connectivity;
        private final HandlerThread thread = new HandlerThread("xdb-nsd");
        private final Handler handler;
        private final Executor executor;
        private final Map<String, Entry> entries = new LinkedHashMap<>();
        private volatile boolean closed;
        private boolean started;
        private boolean networkRegistered;
        private String warning = "";
        private volatile byte[] published = "{\"available\":false,\"services\":[]}".getBytes(java.nio.charset.StandardCharsets.UTF_8);
        private final NetworkRequest networks = new NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET).build();

        Backend(Context context) {
            nsd = context.getSystemService(NsdManager.class);
            connectivity = context.getSystemService(ConnectivityManager.class);
            thread.start(); handler = new Handler(thread.getLooper());
            executor = command -> handler.post(command);
            handler.post(() -> {
                if (closed) return;
                try {
                    connectivity.registerNetworkCallback(networks, changes, handler);
                    networkRegistered = true;
                    nsd.discoverServices(TYPE, NsdManager.PROTOCOL_DNS_SD, networks, executor, discovery);
                } catch (RuntimeException error) {
                    warning = "Android NSD start failed: " + error.getClass().getSimpleName();
                    publish();
                }
            });
        }
        // Called by JNI on arbitrary native threads. Immutable, no locks, JNI
        // callbacks, binder calls or periodic scan needed to read the cache.
        public byte[] snapshot() { return published; }
        private final ConnectivityManager.NetworkCallback changes = new ConnectivityManager.NetworkCallback() {
            @Override public void onLost(Network network) {
                if (closed) return;
                for (Entry entry : new ArrayList<>(entries.values())) {
                    if (network.equals(entry.network)) remove(entry);
                }
                publish();
            }
            @Override public void onLinkPropertiesChanged(Network network, LinkProperties properties) {
                if (closed) return;
                for (Entry entry : entries.values()) {
                    if (network.equals(entry.network) && entry.info != null) entry.update(entry.info);
                }
                publish();
            }
        };
        private String key(NsdServiceInfo info) {
            return info.getServiceName() + "@" + info.getNetwork();
        }
        private final NsdManager.DiscoveryListener discovery = new NsdManager.DiscoveryListener() {
            @Override public void onDiscoveryStarted(String type) {
                if (closed) { try { nsd.stopServiceDiscovery(this); } catch (RuntimeException ignored) {} return; }
                started = true; publish();
            }
            @Override public void onDiscoveryStopped(String type) { started = false; publish(); }
            @Override public void onStartDiscoveryFailed(String type, int code) {
                started = false; warning = "Android NSD discovery failed: " + code; publish();
            }
            @Override public void onStopDiscoveryFailed(String type, int code) {
                started = false; warning = "Android NSD stop failed: " + code; publish();
            }
            @Override public void onServiceFound(NsdServiceInfo info) {
                if (closed || !TYPE.equalsIgnoreCase(info.getServiceType()) ||
                    info.getServiceName() == null || info.getServiceName().getBytes(java.nio.charset.StandardCharsets.UTF_8).length > 63) return;
                String key = key(info);
                if (entries.containsKey(key)) return;
                if (entries.size() >= 256) { warning = "Android NSD service limit reached"; publish(); return; }
                Entry entry = new Entry(key, info.getNetwork());
                entries.put(key, entry);
                try { nsd.registerServiceInfoCallback(info, executor, entry); }
                catch (RuntimeException error) {
                    entries.remove(key); warning = "Android NSD subscribe failed: " + error.getClass().getSimpleName();
                    publish();
                }
            }
            @Override public void onServiceLost(NsdServiceInfo info) {
                if (closed) return;
                Entry entry = entries.get(key(info));
                if (entry != null) { remove(entry); publish(); }
            }
        };
        private final class Entry implements NsdManager.ServiceInfoCallback {
            final String key;
            final Network network;
            NsdServiceInfo info;
            JSONObject resolved;
            Entry(String key, Network network) { this.key = key; this.network = network; }
            boolean current() { return !closed && entries.get(key) == this; }
            @Override public void onServiceInfoCallbackRegistrationFailed(int code) {
                if (!current()) return;
                entries.remove(key); warning = "Android NSD service-info failed: " + code; publish();
            }
            @Override public void onServiceInfoCallbackUnregistered() {}
            @Override public void onServiceLost() {
                if (current()) { info = null; resolved = null; publish(); }
            }
            @Override public void onServiceUpdated(NsdServiceInfo next) {
                if (current()) { info = next; update(next); publish(); }
            }
            void update(NsdServiceInfo value) {
                resolved = null;
                try {
                    if (value.getPort() < 1 || value.getPort() > 65535) return;
                    LinkProperties properties = network == null ? null : connectivity.getLinkProperties(network);
                    NetworkInterface iface = properties == null || properties.getInterfaceName() == null ? null :
                        NetworkInterface.getByName(properties.getInterfaceName());
                    // Tethering callbacks may not have a Network. Only use a
                    // real local IPv6 scope in that case, never a guessed route.
                    if (iface == null) {
                        for (InetAddress address : value.getHostAddresses()) {
                            if (address instanceof Inet6Address) {
                                Inet6Address v6 = (Inet6Address) address;
                                if (v6.getScopeId() > 0) { iface = NetworkInterface.getByIndex(v6.getScopeId()); break; }
                            }
                        }
                    }
                    if (iface == null || iface.getIndex() <= 0) {
                        warning = "Android NSD returned a service without a local interface"; return;
                    }
                    JSONArray addresses = new JSONArray();
                    for (InetAddress address : value.getHostAddresses()) {
                        if (addresses.length() >= 32) break;
                        if (address.isAnyLocalAddress() || address.isLoopbackAddress() || address.isMulticastAddress()) continue;
                        if (address instanceof Inet6Address) {
                            int scope = ((Inet6Address) address).getScopeId();
                            if (scope != 0 && scope != iface.getIndex()) continue;
                        }
                        // Native code adds the validated interface scope again.
                        addresses.put(address.getHostAddress().split("%", 2)[0]);
                    }
                    if (addresses.length() == 0) return;
                    JSONObject txt = new JSONObject();
                    int total = 0;
                    for (Map.Entry<String, byte[]> field : value.getAttributes().entrySet()) {
                        byte[] bytes = field.getValue() == null ? new byte[0] : field.getValue();
                        total += field.getKey().length() + bytes.length + 2;
                        if (bytes.length > 255 || total > 4096 || txt.length() >= 128) return;
                        txt.put(field.getKey(), Base64.encodeToString(bytes, Base64.NO_WRAP));
                    }
                    resolved = new JSONObject().put("name", value.getServiceName()).put("port", value.getPort())
                        .put("index", iface.getIndex()).put("interface", iface.getName())
                        .put("addresses", addresses).put("txt", txt);
                } catch (Exception error) {
                    warning = "Android NSD ignored an invalid service: " + error.getClass().getSimpleName();
                }
            }
        }
        private void remove(Entry entry) {
            entries.remove(entry.key);
            try { nsd.unregisterServiceInfoCallback(entry); } catch (RuntimeException ignored) {}
        }
        private void publish() {
            try {
                JSONArray services = new JSONArray();
                if (!closed) for (Entry entry : entries.values()) if (entry.resolved != null) services.put(entry.resolved);
                // A platform error selects direct mDNS rather than publishing an
                // incomplete native view as a successful empty discovery.
                byte[] bytes = new JSONObject().put("available", !closed && started && warning.isEmpty())
                    .put("warning", warning).put("services", services).toString().getBytes(java.nio.charset.StandardCharsets.UTF_8);
                published = bytes.length <= 1024 * 1024 ? bytes :
                    "{\"available\":false,\"warning\":\"Android NSD snapshot limit reached\",\"services\":[]}".getBytes(java.nio.charset.StandardCharsets.UTF_8);
            } catch (Exception ignored) {} // Retain the preceding immutable snapshot.
        }
        @Override public void close() {
            closed = true;
            // Clear synchronously so a concurrent request cannot reuse peers
            // from the closed lifecycle while callbacks finish unregistering.
            published = "{\"available\":false,\"services\":[]}".getBytes(java.nio.charset.StandardCharsets.UTF_8);
            handler.post(() -> {
                for (Entry entry : new ArrayList<>(entries.values())) remove(entry);
                try { nsd.stopServiceDiscovery(discovery); } catch (RuntimeException ignored) {}
                if (networkRegistered) connectivity.unregisterNetworkCallback(changes);
                thread.quitSafely();
            });
        }
    }
}
