# SPDX-License-Identifier: LGPL-3.0-only
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).parent / 'support'))
from fake_server import FakeServer

exe = str(Path(sys.argv[1]).resolve())
with FakeServer() as server, tempfile.TemporaryDirectory(prefix='xdb-cli-') as tmp:
    original = server.handle_call
    phone = dict(udid='mock-iphone', name='Test iPhone', state='paired', pairing='record_present',
                 connection='unverified', candidates=[dict(transport='wifi', address='fe80::1%4',
                 port=49152, interface='en0', connection_type='RemotePairing', paired=True,
                 pairing='record_present')])
    devices = [copy.deepcopy(phone)]
    cancelled = False

    def handle(name, params):
        if name == 'devices':
            with server.lock:
                server.calls.append((name, params))
            return dict(devices=devices, cancelled=cancelled, warnings=[]), b''
        return original(name, params)
    server.handle_call = handle

    def run(*args, ok=True):
        result = subprocess.run([exe, '--port', str(server.server_address[1]), *args],
                                text=True, capture_output=True, encoding='utf-8', timeout=5)
        assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
        return result

    before_help = server.methods()
    summary = run('--help').stdout
    assert 'Commands:' in summary and 'Options:' in summary
    assert 'record_present' not in summary and 'JSON debugging console, not an iOS' not in summary
    for command in (('-h',), ('devices',), ('pair',), ('ddi',), ('ddi', 'prepare'),
                    ('screenshot',), ('start-server',), ('kill-server',), ('server',),
                    ('rpc',), ('rpc-shell',)):
        args = command if command == ('-h',) else (*command, '--help')
        assert 'Usage:' in run(*args).stdout
    assert 'connectivity' in run('devices', '--help').stdout
    assert 'one-use code' in run('pair', '--help').stdout
    assert '--cache-dir' in run('ddi', 'prepare', '--help').stdout
    assert 'not an iOS command shell' in run('rpc-shell', '--help').stdout
    assert 'Unknown command' in run('unknown', '--help', ok=False).stderr
    assert 'Unknown ddi command' in run('ddi', 'unknown', '--help', ok=False).stderr
    # Help must not read runtime configuration or contact/start a server.
    help_env = dict(os.environ, XDB_PORT='invalid', XDB_EXECUTABLE='/nonexistent/xdb')
    for args in ((), ('--help',), ('server', '--help'), ('pair', '--help')):
        result = subprocess.run([exe, *args], env=help_env, text=True, capture_output=True,
                                encoding='utf-8', timeout=5)
        assert result.returncode == 0 and 'Usage:' in result.stdout, result.stderr
    assert server.methods() == before_help

    removed = run('pairing', 'import', '/nonexistent/record', ok=False)
    assert 'Unknown command' in removed.stderr
    assert 'pairing import' not in subprocess.check_output([exe, '--help'], text=True)
    assert 'pairing.import' not in server.methods()
    short = run('devices').stdout
    long = run('devices', '-l').stdout
    assert 'mock-iphone\tpaired' in short and 'en0' not in short
    assert 'connection:unverified' in long and '[fe80::1%4]:49152' in long and 'interface:en0' in long
    assert json.loads(run('devices', '--json').stdout)['result']['devices'] == devices
    target = str(Path(tmp) / 'screen.png')
    run('screenshot', target, '--wifi')
    assert Path(target).read_bytes().startswith(b'\x89PNG')
    assert any(name == 'devices' and p['transport'] == 'wifi' for name, p in server.calls)
    assert any(name == 'connection.open' and p['udid'] == 'mock-iphone' and p['transport'] == 'wifi'
               for name, p in server.calls)
    run('ddi', 'prepare', '--wifi')
    assert any(name == 'ddi.prepare' and p['udid'] == 'mock-iphone' for name, p in server.calls)
    # Multiple endpoints / duplicate identified rows still refer to one UDID.
    devices.append(copy.deepcopy(phone))
    run('screenshot', target)
    devices[-1]['udid'] = 'another-phone'
    before = server.methods().count('connection.open')
    result = run('screenshot', target, ok=False)
    assert 'Multiple devices' in result.stderr and 'another-phone' in result.stderr
    assert server.methods().count('connection.open') == before
    run('-s', 'mock-iphone', 'screenshot', target)
    devices[-1]['candidates'][0]['address'] = '10.0.0.2'
    run('screenshot', target, '--wifi', 'fe80::1%4')
    devices[-1]['udid'] = ''
    assert 'Multiple devices' in run('screenshot', target, ok=False).stderr
    devices[:] = [devices[-1]]
    assert 'pair it first' in run('screenshot', target, ok=False).stderr
    devices.clear()
    assert 'No device found' in run('screenshot', target, ok=False).stderr
    cancelled = True
    assert 'cancelled' in run('screenshot', target, ok=False).stderr
print('PASS: concise/detailed/JSON listing, automatic selection, transport, ambiguity and no-device errors')
