# SPDX-License-Identifier: LGPL-3.0-only
import copy
import json
import os
from pathlib import Path
import subprocess
import signal
import time
import threading
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
    cryptex_installed = True
    ddi_error = False
    branding_supported = True
    manual_connected = False
    manual_supported = True
    read_only_supported = True
    mdns_supported = True
    mdns_available = True

    def handle(name, params):
        global manual_connected
        if name == 'server.info':
            return dict(capabilities=dict(mdns_diagnostics=mdns_supported, pairing_display_brand=branding_supported, manual_connections=manual_supported, read_only_sessions=read_only_supported)), b''
        if name in ('connect', 'disconnect'):
            with server.lock:
                server.calls.append((name, params))
            if name == 'connect':
                result = dict(udid=params.get('udid', 'mock-iphone'), address=params['address'],
                              port=params.get('port', 49152), reused=manual_connected)
                manual_connected = True
                return result, b''
            manual_connected = False
            return dict(disconnected=[dict(udid='mock-iphone', address='10.0.0.7', port=49152)]), b''
        if name in ('mdns.check', 'mdns.services'):
            with server.lock:
                server.calls.append((name, params))
            result = dict(backend='bonjour', available=mdns_available, cancelled=False,
                          warnings=[] if mdns_available else ['Native daemon unavailable'],
                          service_type='_remotepairing._tcp.local.')
            if name == 'mdns.services':
                result['services'] = [dict(instance='Test._remotepairing._tcp.local.', name='',
                                          target='test.local.', endpoints=[dict(address='fe80::1%4', port=49152,
                                          interface='en0', interface_index=4)])] if mdns_available else []
            return result, b''
        if name == 'pair':
            with server.lock:
                server.calls.append((name, params))
            return dict(udid='mock-iphone', reused=False), b''
        if name in ('ddi.status', 'ddi.remove'):
            with server.lock:
                server.calls.append((name, params))
            if ddi_error:
                raise ValueError('ImageBusy')
            result = dict(udid=params['udid'], os_version='27.0.1', kind='cryptex',
                          image_type='Personalized', mounted=ddi_mounted if name == 'ddi.status' else False,
                          verification='CopyDevices entry' if ddi_mounted and name == 'ddi.status' else '',
                          cryptex_installed=cryptex_installed if name == 'ddi.status' else False,
                          cryptex_identifier='com.apple.MobileAsset.DDI' if cryptex_installed and name == 'ddi.status' else '',
                          cryptex_version='27.1.5228.8' if cryptex_installed and name == 'ddi.status' else '')
            if name == 'ddi.remove':
                result['changed'] = ddi_mounted or cryptex_installed
            return result, b''
        if name == 'devices':
            with server.lock:
                server.calls.append((name, params))
            return dict(devices=devices, cancelled=cancelled, warnings=[]), b''
        return original(name, params)
    server.handle_call = handle

    def run(*args, ok=True, serial=''):
        result = subprocess.run([exe, '--port', str(server.server_address[1]), *args],
                                env=dict(os.environ, XDB_SERIAL=serial),
                                text=True, capture_output=True, encoding='utf-8', timeout=5)
        assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
        return result

    before_help = server.methods()
    version = run('--version').stdout
    assert version.startswith('XDeviceBridge ') and 'Running on ' in version
    installed = next(line.removeprefix('Installed as ') for line in version.splitlines()
                     if line.startswith('Installed as '))
    assert Path(installed).samefile(exe), (installed, exe)
    assert run('version').stdout == version
    summary = run('--help').stdout
    assert run('help').stdout == summary
    assert run('help', 'devices').stdout == run('devices', '--help').stdout
    assert run('help', 'ddi', 'prepare').stdout == run('ddi', 'prepare', '--help').stdout
    assert 'Unknown command' in run('help', 'unknown', ok=False).stderr
    assert 'Commands:' in summary and 'Options:' in summary
    assert 'record_present' not in summary and 'JSON debugging console, not an iOS' not in summary
    for command in (('-h',), ('devices',), ('pair',), ('ddi',), ('ddi', 'prepare'), ('ddi', 'status'), ('ddi', 'remove'),
                    ('screenshot',), ('connect',), ('disconnect',), ('start-server',), ('kill-server',), ('server',),
                    ('rpc',), ('rpc-shell',), ('version',), ('get-state',), ('get-serialno',), ('wait-for-device',), ('mdns',), ('mdns', 'check'), ('mdns', 'services')):
        args = command if command == ('-h',) else (*command, '--help')
        assert 'Usage:' in run(*args).stdout
    assert 'connectivity' in run('devices', '--help').stdout
    assert 'one-use code' in run('pair', '--help').stdout
    assert '--cache-dir' in run('ddi', 'prepare', '--help').stdout
    ddi_help = run('ddi').stdout
    assert ddi_help == run('ddi', '--help').stdout
    assert all(command in ddi_help for command in ('prepare', 'status', 'remove'))
    for args in (('ddi', 'unknown'), ('ddi', 'status', 'extra')):
        error = run(*args, ok=False).stderr
        assert 'run xdb ddi --help' in error and 'screenshot' not in error
    assert 'not an iOS command shell' in run('rpc-shell', '--help').stdout
    assert 'liveness probe' in run('get-state', '--help').stdout
    assert 'Unknown command' in run('unknown', '--help', ok=False).stderr
    assert 'Unknown ddi command' in run('ddi', 'unknown', '--help', ok=False).stderr
    # Help must not read runtime configuration or contact/start a server.
    help_env = dict(os.environ, XDB_PORT='invalid', XDB_EXECUTABLE='/nonexistent/xdb')
    for args in ((), ('--help',), ('help',), ('help', 'devices'), ('ddi',), ('server', '--help'), ('pair', '--help')):
        result = subprocess.run([exe, *args], env=help_env, text=True, capture_output=True,
                                encoding='utf-8', timeout=5)
        assert result.returncode == 0 and 'Usage:' in result.stdout, result.stderr
    for args in (('--version',), ('version',)):
        result = subprocess.run([exe, *args], env=help_env, text=True, capture_output=True,
                                encoding='utf-8', timeout=5)
        assert result.returncode == 0 and result.stdout == version and not result.stderr, result.stderr
    assert server.methods() == before_help
    for args in (('connect',), ('connect', '10.0.0.7:0'), ('connect', '[::1'),
                 ('connect', '10.0.0.7:abc'), ('connect', '10.0.0.7:65536'),
                 ('connect', '10.0.0.7:'), ('connect', '[::1]:-1'),
                 ('connect', '10.0.0.7', '--usb'), ('connect', '10.0.0.7', '--timeout', '0'),
                 ('-d', '-e', 'get-state'), ('--usb', '-e', 'get-state'), ('-d', '--wifi', 'get-state'),
                 ('get-state', 'extra'), ('get-serialno', 'extra')):
        run(*args, ok=False)
    assert server.methods() == before_help, 'invalid endpoint contacted the server'
    assert 'Connected to 10.0.0.7:49152' in run('connect', '10.0.0.7').stdout
    assert server.calls[-1] == ('connect', dict(address='10.0.0.7', port=49152))
    assert 'Already connected to' in run('connect', '10.0.0.7:49152').stdout
    run('-s', 'selected', 'connect', '[fe80::1%4]:50000', '--timeout', '1000')
    assert server.calls[-1] == ('connect', dict(udid='selected', address='fe80::1%4', port=50000, timeout_ms=1000))
    run('connect', 'fe80::1%4')
    assert server.calls[-1] == ('connect', dict(address='fe80::1%4', port=49152))
    assert 'Disconnected' in run('disconnect', '10.0.0.7').stdout
    run('-s', 'selected', 'disconnect')
    assert server.calls[-1] == ('disconnect', dict(udid='selected'))
    run('disconnect')
    assert server.calls[-1] == ('disconnect', {})
    manual_supported = False
    before_connect = sum(name in ('connect', 'disconnect') for name, _ in server.calls)
    for args in (('connect', '10.0.0.7'), ('disconnect',)):
        assert 'upgrade xdb Server' in run(*args, ok=False).stderr
    assert sum(name in ('connect', 'disconnect') for name, _ in server.calls) == before_connect
    manual_supported = True
    run('pair', '--display-brand', 'scrctl')
    assert any(name == 'pair' and p['display_brand'] == 'scrctl' for name, p in server.calls)
    branding_supported = False
    before_pair = server.methods().count('pair')
    assert 'upgrade xdb Server' in run('pair', '--display-brand', 'scrctl', ok=False).stderr
    assert server.methods().count('pair') == before_pair
    assert 'only available for pair' in run('devices', '--display-brand', 'scrctl', ok=False).stderr

    removed = run('pairing', 'import', '/nonexistent/record', ok=False)
    assert 'Unknown command' in removed.stderr
    assert 'pairing import' not in subprocess.check_output([exe, '--help'], text=True)
    assert 'pairing.import' not in server.methods()
    short = run('devices').stdout
    long = run('devices', '-l').stdout
    assert 'mock-iphone\tpaired' in short and 'en0' not in short
    assert 'connection:' not in long and '[fe80::1%4]:49152' in long and 'interface:en0' in long
    assert long.splitlines()[0] == 'List of iOS devices'
    assert 'name:Test iPhone' in long and 'pairing:record_present' not in long
    candidates = [line for line in long.splitlines() if line.startswith('  wifi ')]
    assert len(candidates) == 2 and '10.0.0.7:49152' in candidates[1]
    assert candidates[0].index('interface:') == candidates[1].index('interface:')
    assert all(line.index('type:') < line.index('interface:') for line in candidates)
    usb = next(line for line in long.splitlines() if line.startswith('  usb '))
    assert 'type:USB' in usb and 'pairing:' not in usb
    assert json.loads(run('devices', '--json').stdout)['result']['devices'] == devices
    before_query = len(server.calls)
    assert run('get-serialno').stdout == 'mock-iphone\n'
    assert run('-d', 'get-serialno').stdout == 'mock-iphone\n'
    assert server.calls[-1] == ('devices', dict(transport='usb'))
    assert run('get-serialno', '-e').stdout == 'mock-iphone\n'
    assert server.calls[-1] == ('devices', dict(transport='wifi'))
    for state in ('discovered', 'paired', 'connected'):
        devices[0]['state'] = state
        assert run('get-state').stdout == state + '\n'
    devices[0]['state'] = 'paired'
    assert json.loads(run('get-serialno', '--json').stdout)['result'] == dict(udid='mock-iphone', state='paired')
    assert 'Device not found: missing' in run('-s', 'missing', 'get-state', ok=False).stderr
    assert all(name in ('hello', 'devices') for name, _ in server.calls[before_query:]), 'device queries opened a session'
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
    run('screenshot', '-e', target)
    assert any(name == 'connection.open' and p.get('transport') == 'wifi' for name, p in server.calls)
    run('screenshot', target, '--wifi')
    assert Path(target).read_bytes().startswith(b'\x89PNG')
    assert any(name == 'connection.open' and p.get('access') == 'read' for name, p in server.calls)
    read_only_supported = False
    start = len(server.calls)
    run('screenshot', target, '--wifi')
    assert any(name == 'connection.open' and 'access' not in p for name, p in server.calls[start:])
    read_only_supported = True
    assert any(name == 'devices' and p['transport'] == 'wifi' for name, p in server.calls)
    assert any(name == 'connection.open' and p['udid'] == 'mock-iphone' and p['transport'] == 'wifi'
               for name, p in server.calls)
    # A known manual connection takes precedence over unrelated LAN discovery.
    devices[0]['manual_connection'] = dict(address='10.0.0.7', port=49152)
    devices[0]['state'] = 'connected'
    devices.append(dict(udid='', name='', state='discovered', candidates=[]))
    run('screenshot', target)
    assert run('get-state').stdout == 'connected\n'
    assert run('get-serialno').stdout == 'mock-iphone\n'
    devices.pop()
    devices[0].pop('manual_connection')
    devices[0]['state'] = 'paired'
    run('ddi', 'prepare', '--wifi')
    assert any(name == 'ddi.prepare' and p['udid'] == 'mock-iphone' for name, p in server.calls)
    run('ddi', 'status', serial='environment-iphone')
    assert server.calls[-1] == ('ddi.status', dict(udid='environment-iphone'))
    run('-s', 'selected', 'ddi', 'status', serial='environment-iphone')
    assert server.calls[-1] == ('ddi.status', dict(udid='selected'))
    run('--serial', '', 'ddi', 'status', serial='environment-iphone')
    assert server.calls[-1] == ('ddi.status', dict(udid='mock-iphone'))
    before_ddi = len(server.calls)
    assert 'DDI mounted: Personalized (mock-iphone)' in run('ddi', 'status', '--wifi').stdout
    assert 'DDI removed: mock-iphone' in run('ddi', 'remove', '--wifi').stdout
    assert any(name == 'ddi.status' and p == dict(udid='mock-iphone', wifi='auto')
               for name, p in server.calls[before_ddi:])
    assert any(name == 'ddi.remove' and p == dict(udid='mock-iphone', wifi='auto')
               for name, p in server.calls[before_ddi:])
    assert all(name != 'ddi.prepare' for name, _ in server.calls[before_ddi:])
    ddi_mounted = False
    installed_only = run('-s', 'mock-iphone', 'ddi', 'status', '--wifi').stdout
    assert 'DDI not mounted' in installed_only and 'DDI Cryptex installed: com.apple.MobileAsset.DDI 27.1.5228.8' in installed_only
    installed_json = json.loads(run('-s', 'mock-iphone', 'ddi', 'status', '--json').stdout)['result']
    assert not installed_json['mounted'] and installed_json['cryptex_installed']
    assert 'DDI removed: mock-iphone' in run('ddi', 'remove', '--wifi').stdout
    cryptex_installed = False
    assert 'DDI not mounted: mock-iphone' in run('-s', 'mock-iphone', 'ddi', 'status', '--usb').stdout
    assert 'DDI already absent: mock-iphone' in run('ddi', 'remove', '--usb').stdout
    status = json.loads(run('ddi', 'status', '--json').stdout)
    assert status['ok'] and status['result']['mounted'] is False
    assert 'changed' not in status['result']
    removed = json.loads(run('ddi', 'remove', '--json').stdout)
    assert removed['ok'] and removed['result']['changed'] is False
    assert removed['result']['mounted'] is False and removed['result']['cryptex_installed'] is False
    cryptex_installed = None
    assert 'DDI Cryptex: unavailable' in run('-s', 'mock-iphone', 'ddi', 'status').stdout
    cryptex_installed = False
    assert 'Expected ddi prepare/status/remove' in run('ddi', 'unmount', ok=False).stderr
    assert all('wifi' not in p for name, p in server.calls[-2:] if name.startswith('ddi.'))
    ddi_error = True
    assert 'ImageBusy' in run('ddi', 'remove', ok=False).stderr
    failure = json.loads(run('-s', 'mock-iphone', 'ddi', 'status', '--json', ok=False).stdout)
    assert not failure['ok'] and failure['error']['message'] == 'ImageBusy'
    ddi_error = False
    for action, flag in (('status', '--replace'), ('remove', '--replace'), ('status', '--cache-dir')):
        flags = [flag, tmp] if flag == '--cache-dir' else [flag]
        assert 'apply only to ddi prepare' in run('ddi', action, *flags, ok=False).stderr
    # Multiple endpoints / duplicate identified rows still refer to one UDID.
    devices.append(copy.deepcopy(phone))
    run('screenshot', target)
    assert run('get-serialno').stdout == 'mock-iphone\n'
    devices[-1]['udid'] = 'another-phone'
    before = server.methods().count('connection.open')
    result = run('screenshot', target, ok=False)
    assert 'Multiple devices' in result.stderr and 'another-phone' in result.stderr
    assert server.methods().count('connection.open') == before
    for query in ('get-state', 'get-serialno'):
        assert 'Multiple devices' in run(query, ok=False).stderr
    assert run('get-serialno', serial='another-phone').stdout == 'another-phone\n'
    assert run('-s', 'mock-iphone', 'get-serialno', serial='another-phone').stdout == 'mock-iphone\n'
    for action in ('status', 'remove'):
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
    for query in ('get-state', 'get-serialno'):
        missing = run(query, ok=False)
        assert 'No device found' in missing.stderr and not missing.stdout
    for action in ('status', 'remove'):
        assert 'No device found' in run('ddi', action, ok=False).stderr
    start = len(server.calls)
    assert 'bonjour (available)' in run('mdns', 'check').stdout
    assert server.calls[-1] == ('mdns.check', {})
    services = run('mdns', 'services', '--timeout', '25').stdout
    assert 'Test._remotepairing._tcp.local.' in services and '[fe80::1%4]:49152' in services
    assert server.calls[-1] == ('mdns.services', dict(timeout_ms=25))
    assert json.loads(run('mdns', 'services', '--json').stdout)['result']['available']
    assert not any(name in ('devices', 'connection.open', 'connect', 'pair') for name, _ in server.calls[start:])
    mdns_available = False
    unavailable = run('mdns', 'check', ok=False)
    assert 'unavailable' in unavailable.stdout and 'Native daemon unavailable' in unavailable.stderr
    assert not json.loads(run('mdns', 'services', '--json', ok=False).stdout)['result']['available']
    mdns_supported = False
    assert 'upgrade xdb Server' in run('mdns', 'check', ok=False).stderr
    mdns_supported = True
    mdns_available = True
    assert run('mdns').stdout == run('mdns', '--help').stdout
    for params in (('mdns', 'bad'), ('mdns', 'check', 'extra'), ('mdns', 'services', '--timeout', '0'),
                   ('mdns', 'check', '--timeout', '60001')):
        run(*params, ok=False)
    # Waiting probes real connection acceptance, not just the paired snapshot.
    devices[:] = [copy.deepcopy(phone)]
    wait_opens = 0
    wait_hang = False
    wait_entered = threading.Event()
    def waiting(name, params):
        global wait_opens
        if name == 'connection.open':
            with server.lock:
                server.calls.append((name, params))
            assert params in (dict(udid='mock-iphone', transport='wifi', wifi='auto', access='read'),
                              dict(udid='mock-iphone', transport='wifi', wifi='10.0.0.7', access='read'))
            wait_opens += 1
            wait_entered.set()
            if wait_hang:
                time.sleep(2)
                raise ValueError('Device still locked')
            if wait_opens == 1:
                raise ValueError('Device still locked')
            return dict(udid='mock-iphone', access='read'), b''
        return handle(name, params)
    server.handle_call = waiting
    start = len(server.calls)
    waited = run('-e', 'wait-for-device', '--timeout', '2000')
    assert waited.stdout == '' and waited.stderr == '' and wait_opens == 2
    assert server.calls[-1] == ('connection.close', {})
    assert not any(name.startswith(('input.', 'video.', 'audio.')) or name == 'connect'
                   for name, _ in server.calls[start:])
    result = json.loads(run('-e', 'wait-for-device', '--json', '--timeout', '1000', serial='mock-iphone').stdout)
    assert result['result'] == dict(udid='mock-iphone', state='connected')
    devices.clear()
    start = len(server.calls)
    run('-s', 'mock-iphone', '--wifi', '10.0.0.7', 'wait-for-device', '--timeout', '1000')
    assert not any(name == 'devices' for name, _ in server.calls[start:]), 'explicit target must bypass discovery'
    devices[:] = [copy.deepcopy(phone)]

    devices.append(dict(phone, udid='another-phone'))
    before = wait_opens
    assert 'Multiple devices' in run('-e', 'wait-for-device', '--timeout', '1000', ok=False).stderr
    assert wait_opens == before
    devices.clear()
    timed = run('wait-for-device', '--timeout', '80', '--json', ok=False)
    assert timed.returncode == 124 and json.loads(timed.stdout)['error']['code'] == 'timeout'
    devices[:] = [copy.deepcopy(phone)]
    wait_hang = True
    wait_entered.clear()
    started = time.monotonic()
    timed = run('-e', 'wait-for-device', '--timeout', '100', ok=False)
    assert timed.returncode == 124 and time.monotonic() - started < 1.5
    assert wait_entered.is_set(), 'timeout did not cover an in-flight connection probe'
    # Windows consoles deliver Ctrl+C through a console process group, not Popen.send_signal.
    if os.name != 'nt':
        wait_entered.clear()
        process = subprocess.Popen([exe, '--port', str(server.server_address[1]), '-e', 'wait-for-device'],
                                   env=dict(os.environ, XDB_SERIAL=''), stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        try:
            assert wait_entered.wait(2)
            process.send_signal(signal.SIGINT)
            out, err = process.communicate(timeout=2)
            assert process.returncode == 130 and not out and 'cancelled' in err
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    server.handle_call = handle
    devices.clear()
    cancelled = True
    assert 'cancelled' in run('screenshot', target, ok=False).stderr
print('PASS: device listing, DDI commands, JSON results, transport and automatic selection errors')
