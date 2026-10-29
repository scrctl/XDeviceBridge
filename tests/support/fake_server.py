# SPDX-License-Identifier: LGPL-3.0-only
"""Deterministic XDB1 peer for consumer integration tests; no device is accessed."""
import json
import socket
import socketserver
import struct
import threading
import time
import zlib

DISPLAY = dict(width=3, height=5, orientation='rot90', serial=1)
PIXELS = bytes([9, 41, 93, 255]) * 4 * 6


def png():
    def chunk(name, data):
        return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 5, 3, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress((b'\0' + bytes([93, 41, 9]) * 5) * 3)) + chunk(b'IEND', b''))


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise EOFError
        data.extend(part)
    return data


class FakeServer(socketserver.ThreadingTCPServer):
    server_version = "1.4.0"
    allow_reuse_address = True
    daemon_threads = True
    block_on_close = False

    def __init__(self):
        super().__init__(('127.0.0.1', 0), Handler)
        self.calls = []
        self.lock = threading.Lock()
        self.clipboard = ''
        self.bad_frame = False
        self.no_video = False
        self.shared_control = False
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.shutdown()
        self.server_close()
        self.thread.join()

    @property
    def config(self):
        return dict(server_port=self.server_address[1])

    @property
    def environment(self):
        return dict(XDB_PORT=str(self.server_address[1]))

    def methods(self):
        with self.lock:
            return [name for name, _ in self.calls]

    def handle_call(self, name, p):
        with self.lock:
            self.calls.append((name, p))
        if name == 'hello':
            assert p == dict(protocol=1)
            return dict(protocol=1, version=self.server_version), b''
        if name == 'server.info':
            return dict(capabilities=dict(shared_control_sessions=self.shared_control)), b''
        if name == 'devices':
            return dict(devices=[dict(name='Test iPhone', udid='mock-iphone', candidates=[dict(transport='wifi', address='127.0.0.2', port=49152, paired=True)])], warnings=[], capabilities=dict(video_decode=True)), b''
        if name == 'connection.open':
            assert p['udid'] in ('mock-iphone', '')
            result = dict(udid='mock-iphone', attachment='test-attachment')
            if p.get('access') == 'shared':
                assert self.shared_control
                result['access'] = 'shared'
            return result, b''
        if name == 'connection.attach':
            assert p['attachment'] == 'test-attachment'
        if name == 'device.info':
            return dict(udid='mock-iphone', os_version='26.0', product_type='TestPhone'), b''
        if name == 'display.info':
            return DISPLAY, b''
        if name == 'video.start' and self.no_video:
            raise ValueError('Video unavailable')
        if name == 'video.frame':
            time.sleep(.02)
            return dict(format='bgra', serial=p.get('since', 0) + 1, width=4, height=6, row_pitch=16, display=DISPLAY), PIXELS[:-1] if self.bad_frame else PIXELS
        if name == 'media.status':
            return dict(error='', video_unusable=False), b''
        if name == 'screenshot':
            if p.get('format') == 'bgra':
                return dict(format='bgra', width=5, height=3, row_pitch=20, display=DISPLAY), bytes([9, 41, 93, 255]) * 15
            return dict(format='png', display=DISPLAY), png()
        if name == 'audio.start':
            return dict(sample_rate=48000, channels=2, preroll=2400), b''
        if name == 'audio.read':
            return dict(frames=p['frames'], buffered=p['frames']), b'\0' * p['frames'] * 4
        if name == 'clipboard.set':
            self.clipboard = p['text']
        if name == 'clipboard.get':
            return dict(text=self.clipboard), b''
        if name == 'display.rotate':
            return dict(target='landscapeLeft'), b''
        if name == 'app.list':
            return [dict(bundle_id='test.bundle', name='Test application')], b''
        return {}, b''


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            while True:
                header = exact(self.request, 16)
                assert header[:4] == b'XDB1'
                version, size, binary = struct.unpack('>III', header[4:])
                assert version == 1 and 0 < size <= 1024 * 1024 and binary == 0
                call = json.loads(exact(self.request, size))
                try:
                    result, data = self.server.handle_call(call['method'], call['params'])
                    value = dict(id=call['id'], ok=True, result=result)
                except ValueError as error:
                    value = dict(id=call['id'], ok=False, error=dict(code='operation_failed', message=str(error)))
                    data = b''
                text = json.dumps(value).encode()
                self.request.sendall(b'XDB1' + struct.pack('>III', 1, len(text), len(data)) + text + data)
        except (EOFError, ConnectionError, OSError):
            pass
