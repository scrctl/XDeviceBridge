# JNI invokes snapshot on the provider; keep its name through R8.
-keep class org.xdevicebridge.NsdDiscovery { *; }
-keep class org.xdevicebridge.NsdDiscovery$Backend { public byte[] snapshot(); }
