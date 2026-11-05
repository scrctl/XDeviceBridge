# SPDX-License-Identifier: LGPL-3.0-only
"""Exercise the standalone client SDK against compatible and incompatible peers."""
import ctypes as C
import json
from pathlib import Path
import socket
import struct
import sys
import threading

client = C.CDLL(str(Path(sys.argv[1]).resolve()))
client.xdb_client_version.restype = C.c_char_p
client.xdb_client_connect.argtypes = [C.c_uint16, C.c_void_p, C.c_void_p]
client.xdb_client_connect.restype = C.c_void_p
client.xdb_client_server_version.argtypes = [C.c_void_p]
client.xdb_client_server_version.restype = C.c_char_p
client.xdb_client_destroy.argtypes = [C.c_void_p]
client.xdb_last_error.restype = C.c_char_p
assert client.xdb_client_version().decode() == sys.argv[2]
assert client.xdb_client_server_version(None) is None

def exact(sock, size):
    data = b''
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise EOFError('client disconnected during hello')
        data += part
    return data

for advertised in ({'protocol': 1, 'version': '99.2.3'}, {'protocol': 1}, {'protocol': 2}):
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(5)
        errors = []
        def serve():
            try:
                with listener.accept()[0] as peer:
                    peer.settimeout(5)
                    header = exact(peer, 16)
                    protocol, length, binary = struct.unpack('>III', header[4:])
                    assert header[:4] == b'XDB1' and protocol == 1 and not binary
                    request = json.loads(exact(peer, length))
                    assert request['method'] == 'hello' and request['params']['protocol'] == 1
                    body = json.dumps({'id': request['id'], 'ok': True, 'result': advertised}).encode()
                    peer.sendall(b'XDB1' + struct.pack('>III', 1, len(body), 0) + body)
                    assert peer.recv(1) == b'', 'client did not close its socket'
            except BaseException as error:
                errors.append(error)
        worker = threading.Thread(target=serve)
        worker.start()
        handle = None
        try:
            handle = client.xdb_client_connect(listener.getsockname()[1], None, None)
            if advertised['protocol'] == 1:
                assert handle, client.xdb_last_error()
                assert client.xdb_client_server_version(handle).decode() == advertised.get('version', '')
            else:
                assert not handle and b'incompatible protocol' in client.xdb_last_error()
        finally:
            if handle:
                client.xdb_client_destroy(handle)
            worker.join(6)
        assert not worker.is_alive() and not errors, errors
print('Standalone SDK version, optional hello fields and protocol rejection passed')

import time
Cancel = C.CFUNCTYPE(C.c_int, C.c_void_p)
client.xdb_client_connect_cancellable.argtypes = [C.c_uint16, C.c_void_p, C.c_void_p, Cancel, C.c_void_p]
client.xdb_client_connect_cancellable.restype = C.c_void_p
with socket.socket() as listener:
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    errors = []
    def stalled_hello():
        try:
            with listener.accept()[0] as peer:
                peer.settimeout(3)
                header = exact(peer, 16)
                _, length, _ = struct.unpack('>III', header[4:])
                exact(peer, length)
                assert peer.recv(1) == b'', 'cancel did not close the stalled hello socket'
        except BaseException as error:
            errors.append(error)
    worker = threading.Thread(target=stalled_hello)
    worker.start()
    calls = []
    start = time.monotonic()
    cancel = Cancel(lambda _: calls.append(time.monotonic()) or time.monotonic() - start >= .1)
    handle = client.xdb_client_connect_cancellable(listener.getsockname()[1], None, None, cancel, None)
    elapsed = time.monotonic() - start
    assert not handle and b'cancelled' in client.xdb_last_error(), client.xdb_last_error()
    assert elapsed < .6, elapsed
    count = len(calls)
    worker.join(3)
    time.sleep(.05)
    assert len(calls) == count and not worker.is_alive() and not errors, errors
cancel = Cancel(lambda _: 1)
assert not client.xdb_client_connect_cancellable(1, None, None, cancel, None)
assert b'cancelled' in client.xdb_last_error()
print('Cancelled hello and pre-cancelled connection passed')

# A short-lived launcher never opens a listener, keeping startup retries active.
import os
import tempfile
if sys.platform != 'win32':
    with tempfile.TemporaryDirectory() as directory:
        launcher = Path(directory) / 'xdb'
        launcher.write_text('#!/bin/sh\nexit 0\n')
        launcher.chmod(0o700)
        previous = os.environ.get('XDB_EXECUTABLE')
        os.environ['XDB_EXECUTABLE'] = str(launcher)
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            port = reserve.getsockname()[1]
        started = threading.Event()
        outcome = []
        def starting_connection():
            begin = time.monotonic()
            def stop(_):
                started.set()
                return time.monotonic() - begin > .5
            callback = Cancel(stop)
            handle = client.xdb_client_connect_cancellable(port, None, None, callback, None)
            outcome.append((handle, client.xdb_last_error()))
        worker = threading.Thread(target=starting_connection)
        try:
            worker.start()
            assert started.wait(1)
            time.sleep(.05)
            begin = time.monotonic()
            callback = Cancel(lambda _: time.monotonic() - begin > .1)
            handle = client.xdb_client_connect_cancellable(port, None, None, callback, None)
            assert not handle and b'cancelled' in client.xdb_last_error()
            assert time.monotonic() - begin < .6
            worker.join(2)
            assert not worker.is_alive() and outcome and not outcome[0][0] and b'cancelled' in outcome[0][1], outcome
        finally:
            worker.join(2)
            if previous is None:
                os.environ.pop('XDB_EXECUTABLE', None)
            else:
                os.environ['XDB_EXECUTABLE'] = previous
    print('Cancelled startup retries and concurrent startup-lock wait passed')
