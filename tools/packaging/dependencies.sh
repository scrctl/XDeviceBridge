#!/usr/bin/env bash
# Native desktop release dependencies. Versions and SHA256s follow android-deps.sh.
# Usage: tools/packaging/dependencies.sh OUTPUT_PREFIX
set -euo pipefail
[ "$#" -eq 1 ] || { echo "usage: $0 OUTPUT_PREFIX" >&2; exit 2; }
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$1"
prefix=$(cd "$1" && pwd)
work="$prefix/sources"
mkdir -p "$work"
jobs=${JOBS:-4}
case "$(uname -s)" in
    Darwin) platform=macos; export MACOSX_DEPLOYMENT_TARGET=13.3 ;;
    Linux) platform=linux ;;
    MINGW*|MSYS*) platform=windows ;;
    *) echo 'Unsupported desktop build host' >&2; exit 1 ;;
esac
# Read only the pinned source declarations, not the Android build implementation.
eval "$(sed -n '/^OPENSSL_VERSION=/p; /^OPENSSL_URL=/p; /^OPENSSL_SHA256=/p; /^CURL_VERSION=/p; /^CURL_URL=/p; /^CURL_SHA256=/p; /^FFMPEG_VERSION=/p; /^FFMPEG_URL=/p; /^FFMPEG_SHA256=/p' "$root/tools/android-deps.sh")"
fetch() {
    local url=$1 hash=$2 name=$3
    if [ ! -f "$work/$name" ] || [ "$(shasum -a 256 "$work/$name" | awk '{print $1}')" != "$hash" ]; then
        curl --fail --location --retry 3 --connect-timeout 30 --speed-limit 1024 --speed-time 300 -o "$work/$name.part" "$url"
        [ "$(shasum -a 256 "$work/$name.part" | awk '{print $1}')" = "$hash" ] || { echo "SHA256 mismatch: $name" >&2; exit 1; }
        mv "$work/$name.part" "$work/$name"
    fi
    if [ ! -d "$work/$4" ]; then tar -xf "$work/$name" -C "$work"; fi
}
fetch "$OPENSSL_URL" "$OPENSSL_SHA256" "openssl-$OPENSSL_VERSION.tar.gz" "openssl-$OPENSSL_VERSION"
fetch "$FFMPEG_URL" "$FFMPEG_SHA256" "ffmpeg-$FFMPEG_VERSION.tar.xz" "ffmpeg-$FFMPEG_VERSION"
# macOS uses the SDK's system curl and certificate store.
if [ "$platform" != macos ]; then
    fetch "$CURL_URL" "$CURL_SHA256" "curl-$CURL_VERSION.tar.xz" "curl-$CURL_VERSION"
fi
marker="$prefix/recipe.txt"
recipe="desktop-4 $platform $(uname -m) OpenSSL-$OPENSSL_VERSION curl-$CURL_VERSION FFmpeg-$FFMPEG_VERSION"
if [ ! -f "$marker" ] || [ "$(cat "$marker")" != "$recipe" ]; then
    (
        cd "$work/openssl-$OPENSSL_VERSION"
        args=(no-shared no-module no-tests --libdir=lib "--prefix=$prefix")
        case "$platform" in
            macos) args+=(-mmacosx-version-min=13.3) ;;
            linux) args+=(-fPIC) ;;
            windows) args=(mingw64 "${args[@]}") ;;
        esac
        perl ./Configure "${args[@]}"
        make -j"$jobs"
        make install_sw
    )
    if [ "$platform" != macos ]; then
        # Limit curl to HTTP(S), with native CA lookup on Windows and system CA paths on Linux.
        curl_args=(-DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF
            -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF
            -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF
            -DOPENSSL_ROOT_DIR="$prefix" -DOPENSSL_USE_STATIC_LIBS=ON)
        for protocol in DICT DOH FILE FTP GOPHER IMAP LDAP LDAPS MQTT POP3 RTSP SMTP TELNET TFTP WEBSOCKETS IPFS; do
            curl_args+=("-DCURL_DISABLE_${protocol}=ON")
        done
        if [ "$platform" = windows ]; then
            curl_args+=(-DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF)
        else
            curl_args+=(-DCURL_USE_OPENSSL=ON)
        fi
        cmake -S "$work/curl-$CURL_VERSION" -B "$work/curl-build" -G Ninja \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
            -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON "${curl_args[@]}"
        cmake --build "$work/curl-build" --parallel "$jobs"
        cmake --install "$work/curl-build"
    fi
    mkdir -p "$work/ffmpeg-build"
    (
        cd "$work/ffmpeg-build"
        args=(--prefix="$prefix" --libdir="$prefix/lib"
            --disable-autodetect --disable-everything --disable-programs --disable-doc --disable-debug
            --disable-network --disable-avdevice --disable-avfilter
            --enable-shared --disable-static --enable-pic
            --enable-avcodec --enable-avutil --enable-swscale --enable-swresample --enable-avformat
            --enable-decoder=hevc,png,aac --enable-parser=hevc,aac
            --enable-bsf=hevc_metadata
            --enable-muxer=mp4,matroska --enable-protocol=file --enable-zlib)
        if [ "$platform" = macos ]; then
            args+=(--enable-pthreads --install-name-dir=@rpath --extra-cflags=-mmacosx-version-min=13.3 --extra-ldflags=-mmacosx-version-min=13.3)
        elif [ "$platform" = windows ]; then
            args+=(--target-os=mingw32 --arch=x86_64 --cc=gcc --cxx=g++ --disable-pthreads --enable-w32threads
                --enable-d3d11va --enable-hwaccel=hevc_d3d11va,hevc_d3d11va2)
        else
            # FFmpeg uses libdrm to select a render node when no GPU device is specified.
            args+=(--enable-pthreads --enable-libdrm --enable-vaapi --enable-hwaccel=hevc_vaapi)
        fi
        "$work/ffmpeg-$FFMPEG_VERSION/configure" "${args[@]}"
        make -j"$jobs"
        make install
    )
    printf '%s\n' "$recipe" > "$marker"
fi
# Keep package builds away from installed Homebrew/MSYS2 JSON and XML packages.
cmake_prefix="$prefix"
if [ "$platform" = windows ]; then cmake_prefix=$(cygpath -m "$prefix"); fi
cat > "$prefix/xdb.cmake" <<EOF_CMAKE
set(CMAKE_PREFIX_PATH "$cmake_prefix" CACHE STRING "")
set(OPENSSL_ROOT_DIR "$cmake_prefix" CACHE PATH "")
set(OPENSSL_USE_STATIC_LIBS ON CACHE BOOL "")
set(CMAKE_DISABLE_FIND_PACKAGE_nlohmann_json ON CACHE BOOL "")
set(CMAKE_DISABLE_FIND_PACKAGE_pugixml ON CACHE BOOL "")
EOF_CMAKE
printf '\nDependencies ready. Configure with:\n  PKG_CONFIG_PATH="%s/lib/pkgconfig" cmake -C "%s/xdb.cmake" -S . -B build-release -DCMAKE_BUILD_TYPE=Release\n' "$prefix" "$prefix"
