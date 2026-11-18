#!/usr/bin/env python3
"""Check a relocated package and SDK executable lookup without touching real devices."""
import ctypes as C
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import subprocess
import sys
import tempfile

def check_hevc_hardware_config(codec, relocated, windows):
    class CodecHwConfig(C.Structure):
        _fields_ = [('pixel_format', C.c_int), ('methods', C.c_int), ('device_type', C.c_int)]

    directory = relocated / ('bin' if windows else 'lib')
    pattern = '*avutil*.dll' if windows else 'libavutil.so*'
    util = C.CDLL(str(sorted(directory.glob(pattern))[0]))
    util.av_hwdevice_find_type_by_name.argtypes = [C.c_char_p]
    util.av_hwdevice_find_type_by_name.restype = C.c_int
    backend = b'd3d11va' if windows else b'vaapi'
    device = util.av_hwdevice_find_type_by_name(backend)
    assert device, f'Bundled FFmpeg lacks the {backend.decode()} device'
    codec.avcodec_find_decoder_by_name.argtypes = [C.c_char_p]
    codec.avcodec_find_decoder_by_name.restype = C.c_void_p
    codec.avcodec_get_hw_config.argtypes = [C.c_void_p, C.c_int]
    codec.avcodec_get_hw_config.restype = C.POINTER(CodecHwConfig)
    hevc = codec.avcodec_find_decoder_by_name(b'hevc')
    assert hevc, 'Bundled FFmpeg lacks the HEVC decoder'
    # Match the hardware-device-context method used by FFmpegDecoder.cpp.
    index = 0
    while config := codec.avcodec_get_hw_config(hevc, index):
        if config.contents.device_type == device and config.contents.methods & 1:
            return
        index += 1
    raise AssertionError(f'Bundled HEVC decoder lacks the {backend.decode()} hardware configuration')


def probe(relocated, temporary):
    windows = platform.system() == 'Windows'
    executable = relocated / 'bin' / ('xdb.exe' if windows else 'xdb')
    library = relocated / ('bin/libxdb_client.dll' if windows else 'lib/libxdb_client.dylib' if platform.system() == 'Darwin' else 'lib/libxdb_client.so')
    os.environ.pop('XDB_EXECUTABLE', None)
    os.environ.update(HOME=str(temporary), XDG_DATA_HOME=str(temporary), LOCALAPPDATA=str(temporary))
    if windows:
        kernel = C.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [C.c_uint32, C.c_int, C.c_uint32]
        kernel.OpenProcess.restype = C.c_void_p
        kernel.WaitForSingleObject.argtypes = [C.c_void_p, C.c_uint32]
        kernel.WaitForSingleObject.restype = C.c_uint32
        kernel.CloseHandle.argtypes = [C.c_void_p]
        dll_directory = os.add_dll_directory(str(relocated / 'bin'))
    # Recording finalization parses HEVC parameters through this optional libav filter.
    codecs = list((relocated / ('bin' if windows else 'lib')).glob(
        '*avcodec*.dll' if windows else 'libavcodec*.dylib' if platform.system() == 'Darwin' else 'libavcodec.so*'))
    assert codecs, 'Bundled libavcodec is missing'
    codec = C.CDLL(str(sorted(codecs)[0]))
    codec.av_bsf_get_by_name.argtypes = [C.c_char_p]
    codec.av_bsf_get_by_name.restype = C.c_void_p
    assert codec.av_bsf_get_by_name(b'hevc_metadata'), 'HEVC recording filter is missing'
    if platform.system() in ('Linux', 'Windows'):
        check_hevc_hardware_config(codec, relocated, windows)
    # Test both SDK sibling-bin lookup and a consumer shipping its own client on PATH.
    for from_path in (False, True):
        candidate = library
        if from_path:
            consumer = temporary / 'consumer'
            consumer.mkdir()
            candidate = consumer / library.name
            shutil.copy2(library, candidate)
            os.environ['PATH'] = str(relocated / 'bin') + os.pathsep + os.environ['PATH']
        client = C.CDLL(str(candidate))
        client.xdb_client_connect.argtypes = [C.c_uint16, C.c_void_p, C.c_void_p]
        client.xdb_client_connect.restype = C.c_void_p
        client.xdb_client_destroy.argtypes = [C.c_void_p]
        client.xdb_client_request.argtypes = [C.c_void_p, C.c_char_p, C.c_char_p, C.POINTER(C.c_void_p)]
        client.xdb_response_json.argtypes = [C.c_void_p]
        client.xdb_response_json.restype = C.c_char_p
        client.xdb_response_destroy.argtypes = [C.c_void_p]
        client.xdb_last_error.restype = C.c_char_p
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        handle = client.xdb_client_connect(port, None, None)
        assert handle, client.xdb_last_error()
        server_process = None
        try:
            response = C.c_void_p()
            assert client.xdb_client_request(handle, b'server.info', b'{}', C.byref(response)), client.xdb_last_error()
            try:
                result = json.loads(client.xdb_response_json(response))
                assert result['ok'] and result['result']['protocol'] == 1, result
                if windows:
                    server_process = kernel.OpenProcess(0x00100000, False, result['result']['pid'])
                    assert server_process, C.get_last_error()
            finally:
                client.xdb_response_destroy(response)
        finally:
            client.xdb_client_destroy(handle)
            try:
                subprocess.run([str(executable), '--port', str(port), 'kill-server'], check=True, timeout=15)
                if server_process:
                    assert kernel.WaitForSingleObject(server_process, 5000) == 0, 'Server did not exit'
            finally:
                if server_process:
                    kernel.CloseHandle(server_process)


def main():
    if sys.argv[1] == '--probe':
        probe(Path(sys.argv[2]), Path(sys.argv[3]))
        return
    package = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='xdb installed ') as temporary:
        temporary = Path(temporary)
        relocated = temporary / 'relocated package'
        shutil.copytree(package, relocated)
        # A separate process releases all ctypes DLL handles before Windows cleanup.
        subprocess.run([sys.executable, str(Path(__file__).resolve()), '--probe',
                        str(relocated), str(temporary)], check=True, timeout=60)
        for document in ('INSTALL.md', 'INSTALL_zh.md', 'ANDROID.md', 'ANDROID_zh.md'):
            assert (relocated / document).is_file(), document
        assert not list(relocated.rglob('CMakeCache.txt'))
        assert not list(relocated.rglob('*.a'))
    print('Relocated CLI, SDK sibling-bin lookup and consumer PATH lookup passed')


if __name__ == '__main__':
    main()
