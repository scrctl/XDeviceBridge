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
    devices[0]['candidates'].append(dict(transport='wifi', address='10.0.0.7', port=49152,
                                       interface='en0', connection_type='RemotePairing',
                                       paired=True, pairing='record_present'))
    devices[0]['candidates'].append(dict(transport='usb', address='', port=0,
                                       interface='', connection_type='USB',
                                       paired=False, pairing='unknown'))
    cancelled = False
    ddi_mounted = True
    ddi_error = False

    def handle(name, params):
        if name in ('ddi.status', 'ddi.unmount'):
            with server.lock:
                server.calls.append((name, params))
            if ddi_error:
                raise ValueError('ImageBusy')
            result = dict(udid=params['udid'], os_version='27.0.1', kind='cryptex',
                          image_type='Personalized', mounted=ddi_mounted if name == 'ddi.status' else False,
                          verification='CopyDevices entry' if ddi_mounted and name == 'ddi.status' else '')
            if name == 'ddi.unmount':
                result['changed'] = ddi_mounted
            return result, b''
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
    for command in (('-h',), ('devices',), ('pair',), ('ddi',), ('ddi', 'prepare'), ('ddi', 'status'), ('ddi', 'unmount'),
                    ('screenshot',), ('start-server',), ('kill-server',), ('server',),
                    ('rpc',), ('rpc-shell',)):
        args = command if command == ('-h',) else (*command, '--help')
        assert 'Usage:' in run(*args).stdout
    assert 'connectivity' in run('devices', '--help').stdout
    assert 'one-use code' in run('pair', '--help').stdout
    assert '--cache-dir' in run('ddi', 'prepare', '--help').stdout
    ddi_help = run('ddi').stdout
    assert ddi_help == run('ddi', '--help').stdout
    assert all(command in ddi_help for command in ('prepare', 'status', 'unmount'))
    for args in (('ddi', 'unknown'), ('ddi', 'status', 'extra')):
        error = run(*args, ok=False).stderr
        assert 'run xdb ddi --help' in error and 'screenshot' not in error
    assert 'not an iOS command shell' in run('rpc-shell', '--help').stdout
    assert 'Unknown command' in run('unknown', '--help', ok=False).stderr
    assert 'Unknown ddi command' in run('ddi', 'unknown', '--help', ok=False).stderr
    # Help must not read runtime configuration or contact/start a server.
    help_env = dict(os.environ, XDB_PORT='invalid', XDB_EXECUTABLE='/nonexistent/xdb')
    for args in ((), ('--help',), ('ddi',), ('server', '--help'), ('pair', '--help')):
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
    assert long.splitlines()[0] == 'List of iOS devices'
    assert 'name:Test iPhone' in long and 'pairing:record_present' not in long
    candidates = [line for line in long.splitlines() if line.startswith('  wifi ')]
    assert len(candidates) == 2 and '10.0.0.7:49152' in candidates[1]
    assert candidates[0].index('interface:') == candidates[1].index('interface:')
    assert all(line.index('type:') < line.index('interface:') for line in candidates)
    usb = next(line for line in long.splitlines() if line.startswith('  usb '))
    assert 'type:USB' in usb and 'pairing:' not in usb
    assert json.loads(run('devices', '--json').stdout)['result']['devices'] == devices
    saved_device = copy.deepcopy(devices[0])
    devices[0]['name'] = ''
    devices[0]['candidates'][0]['interface'] = ''
    devices[0]['candidates'][0]['connection_type'] = ''
    devices[0]['candidates'][1]['pairing'] = 'incomplete_record'
    details = run('devices', '-l').stdout
    assert 'name:' not in details and 'pairing:incomplete_record' in details
    empty_fields = next(line for line in details.splitlines() if '[fe80::1%4]' in line)
    assert 'interface:' not in empty_fields and 'type:' not in empty_fields
    assert json.loads(run('devices', '-l', '--json').stdout)['result']['devices'] == devices
    devices[0] = saved_device
    target = str(Path(tmp) / 'screen.png')
    run('screenshot', target, '--wifi')
    assert Path(target).read_bytes().startswith(b'\x89PNG')
    assert any(name == 'devices' and p['transport'] == 'wifi' for name, p in server.calls)
    assert any(name == 'connection.open' and p['udid'] == 'mock-iphone' and p['transport'] == 'wifi'
               for name, p in server.calls)
    run('ddi', 'prepare', '--wifi')
    assert any(name == 'ddi.prepare' and p['udid'] == 'mock-iphone' for name, p in server.calls)
    before_ddi = len(server.calls)
    assert 'DDI mounted: Personalized (mock-iphone)' in run('ddi', 'status', '--wifi').stdout
    assert 'DDI unmounted: mock-iphone' in run('ddi', 'unmount', '--wifi').stdout
    assert any(name == 'ddi.status' and p == dict(udid='mock-iphone', wifi='auto')
               for name, p in server.calls[before_ddi:])
    assert any(name == 'ddi.unmount' and p == dict(udid='mock-iphone', wifi='auto')
               for name, p in server.calls[before_ddi:])
    assert all(name != 'ddi.prepare' for name, _ in server.calls[before_ddi:])
    ddi_mounted = False
    assert 'DDI not mounted: mock-iphone' in run('-s', 'mock-iphone', 'ddi', 'status', '--usb').stdout
    assert 'DDI already unmounted: mock-iphone' in run('ddi', 'unmount', '--usb').stdout
    status = json.loads(run('ddi', 'status', '--json').stdout)
    assert status['ok'] and status['result']['mounted'] is False
    assert 'changed' not in status['result']
    unmounted = json.loads(run('ddi', 'unmount', '--json').stdout)
    assert unmounted['ok'] and unmounted['result']['changed'] is False
    assert all('wifi' not in p for name, p in server.calls[-2:] if name.startswith('ddi.'))
    ddi_error = True
    assert 'ImageBusy' in run('ddi', 'unmount', ok=False).stderr
    failure = json.loads(run('-s', 'mock-iphone', 'ddi', 'status', '--json', ok=False).stdout)
    assert not failure['ok'] and failure['error']['message'] == 'ImageBusy'
    ddi_error = False
    for action, flag in (('status', '--replace'), ('unmount', '--replace'), ('status', '--cache-dir')):
        flags = [flag, tmp] if flag == '--cache-dir' else [flag]
        assert 'apply only to ddi prepare' in run('ddi', action, *flags, ok=False).stderr
    # Multiple endpoints / duplicate identified rows still refer to one UDID.
    devices.append(copy.deepcopy(phone))
    run('screenshot', target)
    devices[-1]['udid'] = 'another-phone'
    before = server.methods().count('connection.open')
    result = run('screenshot', target, ok=False)
    assert 'Multiple devices' in result.stderr and 'another-phone' in result.stderr
    assert server.methods().count('connection.open') == before
    for action in ('status', 'unmount'):
        before = server.methods().count('ddi.' + action)
        assert 'Multiple devices' in run('ddi', action, ok=False).stderr
        assert server.methods().count('ddi.' + action) == before
        run('-s', 'mock-iphone', 'ddi', action, '--wifi', 'fe80::1%4')
        assert server.calls[-1] == ('ddi.' + action, dict(udid='mock-iphone', wifi='fe80::1%4'))
    run('-s', 'mock-iphone', 'screenshot', target)
    devices[-1]['candidates'][0]['address'] = '10.0.0.2'
    run('screenshot', target, '--wifi', 'fe80::1%4')
    devices[-1]['udid'] = ''
    assert 'Multiple devices' in run('screenshot', target, ok=False).stderr
    devices[:] = [devices[-1]]
    assert 'pair it first' in run('screenshot', target, ok=False).stderr
    devices.clear()
    assert 'No device found' in run('screenshot', target, ok=False).stderr
    for action in ('status', 'unmount'):
        assert 'No device found' in run('ddi', action, ok=False).stderr
    cancelled = True
    assert 'cancelled' in run('screenshot', target, ok=False).stderr
print('PASS: device listing, DDI commands, JSON results, transport and automatic selection errors')
