# SPDX-License-Identifier: LGPL-3.0-only
import ctypes as C
import os
import signal
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

exe, client_lib, server_lib = map(Path, sys.argv[1:])
client = C.CDLL(str(client_lib.resolve()))
server = C.CDLL(str(server_lib.resolve()))
server.xdb_server_start.argtypes = [C.c_uint16,C.c_char_p]
server.xdb_server_start.restype = C.c_void_p
server.xdb_server_port.argtypes = [C.c_void_p]
server.xdb_server_port.restype = C.c_uint16
server.xdb_server_is_running.argtypes = [C.c_void_p]
server.xdb_server_stop.argtypes = [C.c_void_p]
server.xdb_server_last_error.restype = C.c_char_p
client.xdb_client_connect.argtypes = [C.c_uint16,C.c_void_p,C.c_void_p]
client.xdb_client_connect.restype = C.c_void_p
client.xdb_client_request.argtypes = [C.c_void_p,C.c_char_p,C.c_char_p,C.POINTER(C.c_void_p)]
client.xdb_client_destroy.argtypes = [C.c_void_p]
client.xdb_response_json.argtypes = [C.c_void_p]
client.xdb_response_json.restype = C.c_char_p
client.xdb_response_destroy.argtypes = [C.c_void_p]
client.xdb_last_error.restype = C.c_char_p
client.xdb_client_version.restype = C.c_char_p
client.xdb_client_server_version.argtypes = [C.c_void_p]
client.xdb_client_server_version.restype = C.c_char_p
assert client.xdb_client_version()
assert client.xdb_client_server_version(None) is None
client.xdb_client_kill_server.argtypes = [C.c_uint16]

def packet(value, binary=b''):
    text=json.dumps(value).encode()
    return b'XDB1'+struct.pack('>III',1,len(text),len(binary))+text+binary

def read_exact(s,n):
    out=b''
    while len(out)<n:
        part=s.recv(n-len(out))
        if not part:
            raise EOFError()
        out+=part
    return out

def receive(s):
    header=read_exact(s,16)
    assert header[:4]==b'XDB1'
    version,n,b=struct.unpack('>III',header[4:])
    assert version==1
    value=json.loads(read_exact(s,n))
    data=read_exact(s,b)
    return value,data

def hello(s):
    data=packet({'id':1,'method':'hello','params':{'protocol':1}})
    # TCP packet boundaries are not message boundaries.
    for start in range(0,len(data),3):
        s.sendall(data[start:start+3])
    value,_=receive(s)
    assert value['ok'], value

def request(c,method,params=None):
    out=C.c_void_p()
    assert client.xdb_client_request(c,method.encode(),json.dumps(params or {}).encode(),C.byref(out)), client.xdb_last_error()
    try:
        return json.loads(client.xdb_response_json(out))
    finally:
        client.xdb_response_destroy(out)

with tempfile.TemporaryDirectory(prefix='xdb-ipc-') as tmp:
    data=str(Path(tmp)/'private-data').encode()
    assert not server.xdb_server_start(0,b'relative'), 'relative data path accepted'
    handle=server.xdb_server_start(0,data)
    assert handle, server.xdb_server_last_error()
    port=server.xdb_server_port(handle)
    assert port
    assert server.xdb_server_is_running(handle)
    try:
        c=client.xdb_client_connect(port,None,None)
        assert c, client.xdb_last_error()
        try:
            info = request(c,'server.info')['result']
            assert info['protocol']==1 and info['version']
            assert client.xdb_client_version().decode() == info['version']
            assert client.xdb_client_server_version(c).decode() == info['version']
            unknown = request(c,'future.optional.method')
            assert not unknown['ok'] and unknown['error']['code']=='not_connected'
            for method in ('input.not_a_method', 'input.keyboard.future'):
                unknown_input = request(c, method, {'text': None})
                assert not unknown_input['ok'] and unknown_input['error']['code'] == 'unknown_method', unknown_input
            assert request(c, 'server.info')['ok'], 'rejecting unknown input must retain the RPC connection'

            assert request(c,'server.info',{'future_optional_field':True})['ok']
            os.environ['XDB_PORT'] = str(port)
            default_client = client.xdb_client_connect(0,None,None)
            assert default_client, client.xdb_last_error()
            client.xdb_client_destroy(default_client)
            assert request(c,'devices',{'transport':'wifi','timeout_ms':0})['ok']
            removed = request(c,'pairing.import',{'record':'invalid'})
            assert not removed['ok'] and removed['error']['code']=='not_connected'
            assert not Path(data.decode()).exists(), 'removed endpoint wrote persistent state'
            assert not request(c,'connection.open',{'udid':'offline','transport':'invalid'})['ok']
            assert not request(c,'connection.attach',{'attachment':'invalid'})['ok']
            assert request(c,'connection.close')['ok']
            assert not request(c,'screenshot')['ok']
            assert request(c,'server.info')['ok'], 'failed operation poisoned connection'
        finally:
            client.xdb_client_destroy(c)
        with socket.create_connection(('127.0.0.1',port),timeout=2) as s:
            hello(s)
            s.sendall(packet({'id':2,'method':'server.info','params':{}}))
            value,binary=receive(s)
            assert value['id']==2 and value['ok'] and binary==b''
        for params in ({'protocol': 2}, {}):
            with socket.create_connection(('127.0.0.1',port),timeout=2) as s:
                s.sendall(packet({'id':1,'method':'hello','params':params}))
                value,_=receive(s)
                assert not value['ok'] and value['error']['code']=='handshake_rejected'
                assert s.recv(1)==b'', 'rejected handshake did not close connection'
        for bad_header in [b'NOPE'+struct.pack('>III',1,2,0), b'XDB1'+struct.pack('>III',2,2,0), b'XDB1'+struct.pack('>III',1,1024*1024+1,0)]:
            with socket.create_connection(('127.0.0.1',port),timeout=2) as s:
                s.sendall(bad_header)
                assert s.recv(1)==b'', 'invalid wire header did not close connection'
        result=subprocess.run([str(exe.resolve()),'--port',str(port),'rpc','server.info'],text=True,capture_output=True,timeout=5)
        assert result.returncode==0, result.stderr
        assert json.loads(result.stdout)['ok']
        result=subprocess.run([str(exe.resolve()),'--port',str(port),'devices','--wifi','--timeout','0'],text=True,capture_output=True,timeout=5)
        assert result.returncode==0 and result.stdout.splitlines()[0]=='List of iOS devices', (result.stdout, result.stderr)
        # A full pool of sessionless peers (including a partial frame) cannot
        # prevent a new management request from reaching the daemon.
        pool = []
        try:
            for index in range(16):
                peer = socket.create_connection(('127.0.0.1',port),timeout=3)
                hello(peer)
                pool.append(peer)
                if index == 0:
                    peer.sendall(b'X')
            with socket.create_connection(('127.0.0.1',port),timeout=3) as replacement:
                hello(replacement)
                replacement.sendall(packet({'id':2,'method':'server.info','params':{}}))
                assert receive(replacement)[0]['ok'], 'idle peers exhausted worker capacity'
            try:
                assert pool[0].recv(1) == b'', 'partial-frame idle peer was not reclaimed'
            except ConnectionResetError:
                pass  # Windows may abort a socket that has unread bytes.
        finally:
            for peer in pool:
                peer.close()
        # Stop must release idle clients blocked in recv, without requiring their cooperation.
        idle=socket.create_connection(('127.0.0.1',port),timeout=2)
        hello(idle)
    finally:
        before=time.monotonic()
        server.xdb_server_stop(handle)
        assert time.monotonic()-before<3, 'idle-client shutdown blocked'
    assert idle.recv(1)==b''
    idle.close()
    embedded=server.xdb_server_start(0,data)
    assert embedded
    try:
        assert client.xdb_client_kill_server(server.xdb_server_port(embedded)), client.xdb_last_error()
        assert not server.xdb_server_is_running(embedded)
    finally:
        server.xdb_server_stop(embedded)
    process=subprocess.Popen([str(exe.resolve()),'server','--port','0','--data-dir',data.decode()],
                             text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        ready=json.loads(process.stdout.readline())
        assert set(ready)=={'ready','protocol','host','port'} and ready['ready'], ready
        assert ready['host']=='127.0.0.1'
        result=subprocess.run([str(exe.resolve()),'--port',str(ready['port']),'rpc','server.info'],
                              text=True,capture_output=True,timeout=5)
        assert result.returncode==0 and json.loads(result.stdout)['ok'], result.stderr
    finally:
        # SIGTERM on Windows terminates the process; the embedded test above covers graceful stop.
        process.send_signal(signal.SIGTERM)
        process.communicate(timeout=5)
print('PASS: embedded lifecycle, C client, CLI, framing, protocol negotiation, invalid requests, no-side-effect failures and shutdown')
