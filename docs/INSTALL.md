# Install xdb

English | [简体中文](INSTALL_zh.md)

## Homebrew (macOS and Linux)

```sh
brew install scrctl/tap/xdb
# The desktop frontend installs xdb automatically:
brew install scrctl/tap/scrctl
```

The [official tap](https://github.com/scrctl/homebrew-tap) builds pinned release
sources and provides bottles for macOS 15 ARM64 and Linux x86-64. Other supported
hosts build from source. Bottles are separate builds from the notarized release
ZIPs. No login service is installed; clients start xdb on demand.

When switching from a user-local package, stop the old server with `xdb kill-server`
and remove its old command link from PATH. Pairing data and DDI caches are separate
from the runtime installation and should be retained.

## Desktop runtime packages

Download the runtime archive for your operating system and CPU from
[GitHub Releases](https://github.com/scrctl/XDeviceBridge/releases). For development
snapshots, use a successful [packages workflow run](https://github.com/scrctl/XDeviceBridge/actions/workflows/packages.yml):
open its Artifacts section, download the matching `xdb-<OS>-<ARCH>` artifact,
then extract the runtime archive inside it. Workflow artifacts expire and GitHub
requires sign-in to download them. Developer ID signed macOS development packages
are available from the [notarization workflow](https://github.com/scrctl/XDeviceBridge/actions/workflows/macos-notarize.yml)
as `xdb-macos-notarized`. The separate
`*-sources` archive contains the matching sources and build information; it is
not needed to run xdb. Verify the archive using the included `SHA256SUMS.txt` or `*-SHA256SUMS.txt`.

The runtime includes `bin/xdb` (`xdb.exe` on Windows), the client/server libraries,
minimal shared FFmpeg libraries, and dependency license texts. Keep the extracted
folder structure together. Do not copy only the executable.

### macOS and Linux

Extract the archive, enter its directory, then run:

```sh
./install.sh
export PATH="$HOME/.local/bin:$PATH"  # if this directory is not already on PATH
xdb devices -l
```

The installer copies the runtime to `~/.local/opt/xdb` and links
`~/.local/bin/xdb`. It needs no administrator privileges and does not edit shell
profiles. Add the PATH line to your shell profile if needed for future terminals.
Custom absolute install and command directories can be supplied:

```sh
./install.sh /absolute/path/to/xdb /absolute/path/to/bin
```

macOS packages target macOS 13.3 or newer, for the archive's CPU architecture.
Ordinary `packages` artifacts are ad hoc signed; separately labelled `notarized`
ZIPs are Developer ID signed and Apple Accepted. See the online ticket check below.
Linux packages built on Ubuntu 24.04 require a compatible glibc/libstdc++ runtime
and system zlib; they are not universal Linux binaries. Linux USB access also
requires usbmuxd.

### Windows

Extract the entire runtime folder to a permanent location, for example
`C:\Tools\xdb`. Add `C:\Tools\xdb\bin` to your **user PATH** through
System Settings, then open a new terminal:

```powershell
xdb devices -l
```

The initial Windows archive targets x86-64 and includes the MinGW runtime DLLs.
USB requires Apple's mobile-device driver/service. Wi-Fi pairing does not
require that USB driver.

## scrctl and MaaFramework

Their SDK clients find an installed `xdb` on PATH and start its server as needed.
For applications launched with a different PATH (for example a GUI launched by
Finder), set `XDB_EXECUTABLE` in the application's environment to the full path:

```sh
export XDB_EXECUTABLE="$HOME/.local/opt/xdb/bin/xdb"
```

A downstream app may instead ship the full runtime alongside its client library,
using the same `bin/` and `lib/` layout. It must retain dependency license texts.
Android uses the embedded-server model described in [ANDROID.md](ANDROID.md);
these desktop installers are not Android App packages.

Before updating a running server, stop it with `xdb kill-server`, replace the
installed runtime, then use any command to start the new version. Installation
and replacement do not remove pairing records or the DDI cache in the user data
directory. To uninstall a user-local runtime, stop its server, remove the
`~/.local/bin/xdb` symlink and the `~/.local/opt/xdb` installation directory.

## Build packages from source

Native release prerequisites: C++20 compiler, CMake >= 3.21, Ninja, Make, Perl,
Python >= 3.10, pkg-config, curl, xz, and zlib development headers. x86-64 builds
also need NASM. Linux packaging needs patchelf; Windows uses MSYS2 UCRT64 GCC.
The workflow contains the platform package lists.

```sh
tools/packaging/dependencies.sh "$PWD/build-release-deps"
export PKG_CONFIG_PATH="$PWD/build-release-deps/lib/pkgconfig"
cmake -C build-release-deps/xdb.cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DXDB_BUILD_TESTS=OFF
cmake --build build-release --parallel 4
# Commit changes before packaging so the source archive matches the binary.
python3 tools/packaging/package.py --build build-release \
  --dependencies build-release-deps --output build-packages
```

The dependency recipe downloads pinned, SHA256-verified OpenSSL/curl/FFmpeg
sources. OpenSSL and curl are static; FFmpeg is shared and built without GPL or
nonfree options, with only the decoders/muxers required by xdb. macOS uses system
curl. Packaging rejects unexpected dependencies outside this prefix, except for
platform system libraries and the Windows compiler/zlib runtime.

Runtime and source archives are separate. Source materials include the exact
repository commit, fetched dependency source trees, upstream source archives,
configure/build metadata and hashes. Build caches and absolute-path provenance
stay exclusively in the source archive.

### Upgrade behavior

The Unix installer stages the complete new package on the installation filesystem,
checks `xdb --help`, stops the old service with its original executable, then replaces
its managed directory. Stale libraries are removed; activation failure restores the
previous directory. Unmanaged directories and unrelated commands are never overwritten.
End active tasks before upgrading; stopping the service disconnects its clients.
`XDB_PORT` selects the service port; stop servers on other ports separately.
The next connection starts the installed version, including after a rollback.
Pairing records and DDI cache remain in the separate user data directory.

For manual/Windows installations, stop the service with the old `xdb kill-server`,
keep the old directory, extract into a new empty directory, check `bin/xdb --help`,
and update PATH / XDB_EXECUTABLE. Roll back those paths on startup failure. Do not
replace loaded DLLs in place. Incompatible listeners are rejected, never automatically
killed. See [protocol version policy](PROTOCOL.md#version-and-upgrade-policy).

## macOS signing and notarization

Ordinary packages CI builds are ad hoc signed. Separately labelled `notarized`
macOS ZIPs are Developer ID signed and Apple Accepted; tickets are queried online
because a bare CLI ZIP cannot be stapled.

Version tags matching the CMake project version (`vX.Y.Z`) automatically run
macOS signing/notarization through the packages workflow. The tagged commit must
be in main history. Release publication waits for the Linux/Windows builds and
Apple's `Accepted` result, then uploads the notarized macOS ZIP, matching source
archives and checksums to the same GitHub Release. A failed build or notarization
blocks publication. The repository's `Build` environment must allow the `main`
branch and `v*` tags. Manual main runs produce development artifacts; normal
pushes and PRs do not submit to Apple.


For a notarized CLI package, verify the code and its online notarization ticket
from the extracted runtime directory:

```sh
codesign --verify --strict --check-notarization --test-requirement='=notarized' ./bin/xdb
```
