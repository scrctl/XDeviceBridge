#!/usr/bin/env python3
"""Stage a desktop runtime and a separate matching source archive."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def run(*args, **kwargs):
    return subprocess.check_output([str(a) for a in args], text=True, encoding='utf-8', **kwargs).strip()


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def mingw_prefix():
    return Path(run('cygpath', '-m', os.environ['MINGW_PREFIX']))


def dependencies(stage, build, prefix):
    windows = platform.system() == 'Windows'
    executable = stage / 'bin' / ('xdb.exe' if windows else 'xdb')
    destination = stage / ('bin' if windows else 'lib')
    origins = {}
    if platform.system() == 'Darwin':
        # GET_RUNTIME_DEPENDENCIES does not fall back to DIRECTORIES for @rpath
        # on macOS. Resolve each install name against the isolated release prefix.
        queue = [executable, *destination.glob('*.dylib')]
        visited = set()
        while queue:
            binary = queue.pop()
            if binary.resolve() in visited:
                continue
            visited.add(binary.resolve())
            lines = run('otool', '-L', binary).splitlines()
            for line in lines[2 if binary.suffix == '.dylib' else 1:]:
                old = line.strip().split(' (', 1)[0]
                if old.startswith(('/usr/lib/', '/System/Library/')):
                    continue
                target = destination / Path(old).name
                if not target.exists():
                    source = Path(old) if old.startswith('/') else prefix / 'lib' / target.name
                    real = source.resolve()
                    if not real.is_relative_to(prefix) or not real.is_file():
                        raise RuntimeError(f'Dependency escaped the release prefix: {old}')
                    shutil.copy2(real, target)
                    origins[target.name] = str(real)
                queue.append(target)
    else:
        listing = build / 'runtime-dependencies.txt'
        search = [stage / 'lib', stage / 'bin', prefix / 'lib', prefix / 'bin']
        if windows:
            search.append(mingw_prefix() / 'bin')
        run('cmake', f'-Dexecutable={executable}', f'-Doutput={listing}',
            '-Dsearch_directories=' + ';'.join(map(str, search)), '-P', ROOT / 'tools/packaging/runtime.cmake')
        for line in listing.read_text(encoding='utf-8').splitlines():
            if not line:
                continue
            source = Path(line)
            real = source.resolve()
            if real.is_relative_to(stage):
                continue
            if not real.is_relative_to(prefix):
                # Only the MinGW language runtime and zlib may come from the compiler installation.
                if not (windows and re.fullmatch(r'(libgcc_s_.*|libstdc\+\+-6|libwinpthread-1|zlib1|libz-1)\.dll', source.name, re.I)):
                    raise RuntimeError(f'Dependency escaped the release prefix: {source}')
            target = destination / source.name
            if target.exists() and digest(target) != digest(real):
                raise RuntimeError(f'Conflicting runtime library: {source.name}')
            shutil.copy2(real, target)
            origins[target.name] = str(real)
    binaries = [executable] + [p for p in destination.iterdir() if p.is_file() and not p.is_symlink() and (p.suffix == '.dylib' or '.so' in p.name)]
    if platform.system() == 'Darwin':
        for binary in binaries:
            for line in run('otool', '-L', binary).splitlines()[2 if binary.suffix == '.dylib' else 1:]:
                old = line.strip().split(' (', 1)[0]
                if old.startswith(('/usr/lib/', '/System/Library/')):
                    continue
                name = Path(old).name
                if not (destination / name).exists():
                    raise RuntimeError(f'Unbundled dependency {old} in {binary}')
                relative = '@loader_path/../lib/' if binary == executable else '@loader_path/'
                run('install_name_tool', '-change', old, relative + name, binary)
            if binary != executable:
                # Keep the ABI install name (e.g. libxdb_client.0.dylib), not the full version filename.
                install_name = Path(run('otool', '-D', binary).splitlines()[1].strip()).name
                run('install_name_tool', '-id', '@rpath/' + install_name, binary)
            # Remove build-machine search paths, retaining only loader-relative paths.
            load_commands = run('otool', '-l', binary)
            for rpath in re.findall(r'cmd LC_RPATH\n.*?\n\s*path (.*?) \(offset', load_commands):
                if rpath.startswith('/'):
                    run('install_name_tool', '-delete_rpath', rpath, binary)
            run('codesign', '--force', '--sign', '-', binary)
            run('codesign', '--verify', '--strict', binary)
    elif not windows:
        for binary in binaries:
            run('patchelf', '--set-rpath', '$ORIGIN/../lib' if binary == executable else '$ORIGIN', binary)
    return origins


def materials(stage, sources, build, prefix, origins):
    licenses = stage / 'share/licenses/XDeviceBridge'
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / 'THIRD_PARTY_NOTICES.md', licenses)
    # Preserve exact fetched sources separately; never copy build caches into the runtime.
    external = sources / 'dependencies'
    external.mkdir(parents=True)
    for archive in sorted(p for p in (prefix / 'sources').glob('*.tar.*') if p.name.endswith(('.tar.gz', '.tar.xz'))):
        shutil.copy2(archive, external)
    cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
    for name in ('nlohmann_json', 'pugixml', 'mdns', 'lwip'):
        match = re.search(rf'^{name}_SOURCE_DIR:[^=]+=(.+)$', cache, re.M)
        path = Path(match.group(1)) if match else build / '_deps' / (name + '-src')
        if not path.is_dir():
            raise RuntimeError(f'Missing bundled dependency sources: {name}')
        with tarfile.open(external / (name + '.tar.gz'), 'w:gz') as archive:
            archive.add(path, arcname=name)
        target = licenses / name
        target.mkdir()
        found = False
        for filename in ('LICENSE', 'LICENSE.txt', 'LICENSE.md', 'LICENSE.MIT', 'COPYING', 'COPYING.txt'):
            if (path / filename).is_file():
                shutil.copy2(path / filename, target)
                found = True
        if name == 'mdns' and not found:
            # Upstream keeps its Unlicense in the single header.
            shutil.copy2(path / 'mdns.h', target)
            found = True
        if not found:
            raise RuntimeError(f'Missing license text: {name}')
    for name, pattern, texts in (
        ('OpenSSL', 'openssl-*', ('LICENSE.txt',)),
        ('curl', 'curl-*', ('COPYING',)),
        ('FFmpeg', 'ffmpeg-*', ('COPYING.LGPLv2.1', 'COPYING.LGPLv3', 'LICENSE.md')),
    ):
        candidates = [p for p in (prefix / 'sources').glob(pattern) if p.is_dir() and (p / texts[0]).is_file()]
        if not candidates:
            if name == 'curl' and platform.system() == 'Darwin':
                continue  # macOS system library
            raise RuntimeError(f'Missing license sources: {name}')
        target = licenses / name
        target.mkdir()
        for filename in texts:
            if (candidates[0] / filename).is_file():
                shutil.copy2(candidates[0] / filename, target)
    if platform.system() == 'Windows':
        # Compiler DLLs use GCC's runtime exception; preserve MSYS2 package notices.
        for name in ('gcc-libs', 'libgcc', 'libstdc++', 'libwinpthread', 'winpthreads', 'zlib'):
            path = mingw_prefix() / 'share/licenses' / name
            if path.is_dir():
                shutil.copytree(path, licenses / name)
        if not (licenses / 'gcc-libs').exists() and not all(
                (licenses / name).is_dir() for name in ('libgcc', 'libstdc++')):
            raise RuntimeError('Missing MinGW GCC runtime license texts')
    logs = sources / 'build-information'
    logs.mkdir()
    for relative in ('recipe.txt', 'sources/ffmpeg-build/config.h', 'sources/ffmpeg-build/ffbuild/config.mak',
                     'sources/ffmpeg-build/ffbuild/config.log', 'sources/curl-build/CMakeCache.txt'):
        path = prefix / relative
        if path.is_file():
            shutil.copy2(path, logs / path.name)
    shutil.copy2(build / 'CMakeCache.txt', logs / 'xdb-CMakeCache.txt')
    openssl = next(p for p in (prefix / 'sources').glob('openssl-*') if p.is_dir() and (p / 'configdata.pm').is_file())
    (logs / 'openssl-configuration.txt').write_text(run('perl', 'configdata.pm', '--dump', cwd=openssl) + '\n', encoding='utf-8')
    # Only committed repository contents are release sources. Dirty packaging is refused below.
    with (sources / 'XDeviceBridge.tar').open('wb') as stream:
        subprocess.run(['git', '-C', str(ROOT), 'archive', 'HEAD'], stdout=stream, check=True)
    manifest = {'commit': run('git', '-C', ROOT, 'rev-parse', 'HEAD'),
                'platform': platform.platform(), 'runtime_origins': origins,
                'runtime_sha256': {str(p.relative_to(stage)): digest(p) for p in stage.rglob('*') if p.is_file()},
                'sources_sha256': {str(p.relative_to(sources)): digest(p) for p in sources.rglob('*') if p.is_file()}}
    (sources / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')


def verify_clean():
    # MSYS Git and Git for Windows can disagree about checkout line endings and
    # executable bits. Normalize these Windows checkout conventions for this check.
    options = ('-c', 'core.autocrlf=true', '-c', 'core.filemode=false') if platform.system() == 'Windows' else ()
    # Compare contents instead of cached stat data produced by another Git build.
    changes = set()
    for args in (('diff', '--name-only', 'HEAD', '--'),
                 ('diff', '--cached', '--name-only', 'HEAD', '--'),
                 ('ls-files', '--others', '--exclude-standard')):
        changes.update(run('git', *options, '-C', ROOT, *args).splitlines())
    if changes:
        raise RuntimeError('Commit source changes before producing matching runtime/source archives:\n' + '\n'.join(sorted(changes)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--dependencies', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--config', default='Release')
    parser.add_argument('--developer-id-sign', action='store_true',
                        help='Sign staged macOS code before recording source/runtime hashes')
    args = parser.parse_args()
    if args.developer_id_sign and platform.system() != 'Darwin':
        parser.error('--developer-id-sign requires macOS')
    verify_clean()
    build, prefix, output = args.build.resolve(), args.dependencies.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    label = 'xdb-' + platform.system().lower() + '-' + platform.machine().lower()
    with tempfile.TemporaryDirectory(prefix='xdb-package-') as temporary:
        work = Path(temporary)
        stage, sources = work / label, work / (label + '-sources')
        sources.mkdir()
        run('cmake', '--install', build, '--prefix', stage, '--config', args.config, '--component', 'Runtime')
        # CMake also installs the SDK. Runtime bundles carry no headers/import/static libraries.
        shutil.rmtree(stage / 'include', ignore_errors=True)
        for path in (stage / 'lib').glob('*.a'):
            path.unlink()
        for path in (stage / 'lib').glob('*.lib'):
            path.unlink()
        for name in ('INSTALL.md', 'INSTALL_zh.md', 'ANDROID.md', 'ANDROID_zh.md'):
            shutil.copy2(ROOT / 'docs' / name, stage / name)
        if platform.system() != 'Windows':
            shutil.copy2(ROOT / 'tools/packaging/install.sh', stage / 'install.sh')
        origins = dependencies(stage, build, prefix)
        if args.developer_id_sign:
            subprocess.run(['bash', str(ROOT / 'tools/packaging/sign_macos_package.sh'), str(stage), 'xdb'], check=True)
        materials(stage, sources, build, prefix, origins)
        for directory in (stage, sources):
            if platform.system() == 'Windows':
                with zipfile.ZipFile(output / (directory.name + '.zip'), 'w', zipfile.ZIP_DEFLATED) as archive:
                    for path in directory.rglob('*'):
                        if path.is_file():
                            archive.write(path, path.relative_to(work))
            else:
                with tarfile.open(output / (directory.name + '.tar.gz'), 'w:gz') as archive:
                    archive.add(directory, arcname=directory.name)
        packages = sorted(p for p in output.glob(label + '*') if p.name.endswith(('.tar.gz', '.zip')))
        (output / (label + '-SHA256SUMS.txt')).write_text(''.join(f'{digest(p)}  {p.name}\n' for p in packages if p.is_file()), encoding='utf-8')
        print('\n'.join(map(str, packages)))


if __name__ == '__main__':
    main()
