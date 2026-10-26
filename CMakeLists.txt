cmake_minimum_required(VERSION 3.20)
project(XDeviceBridge VERSION 1.0.1 LANGUAGES CXX)
configure_file(cmake/version.h.in include/xdb/version.h @ONLY)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX AND NOT ANDROID)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
option(XDB_BUILD_SERVER "Build the iOS server backend" ON)
option(XDB_BUILD_CLI "Build the command-line frontend" ON)
option(XDB_INSTALL "Install xdb targets and SDK" ON)
if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(MSVC)
    # Embedders may enable /WX; getenv is intentional for endpoint configuration.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()
option(XDB_BUILD_TESTS "Build offline and IPC tests" ON)
# Embedded builds must not download dependencies unless their owner opts in.
set(xdb_fetch_default OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(xdb_fetch_default ON)
endif()
option(XDB_FETCH_DEPENDENCIES "Download missing pinned dependencies" ${xdb_fetch_default})
include(cmake/Json.cmake)
find_package(Threads REQUIRED)
if(XDB_BUILD_SERVER)
include(cmake/Dependencies.cmake)
include(cmake/Lwip.cmake)
include(cmake/Threading.cmake)
# ------------------------------------------------------- Apple frameworks ---
# Apple 媒体框架通过 C API 调用，解码器使用 .cpp，无需 Objective-C++ 或 ARC。
# 非 Apple 平台使用下方按依赖选择的解码后端。
add_library(XdbAppleFrameworks INTERFACE)
if(APPLE)
    target_link_libraries(XdbAppleFrameworks INTERFACE
            "-framework VideoToolbox"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework CoreFoundation"
            "-framework IOKit"
            "-framework AudioToolbox"
            "-framework CoreAudio")
endif()

# ------------------------------------------------------------- OpenSSL ------
# lockdown 的 StartSession 可能要求双向 TLS，客户端证书来自配对记录。
# 本项目通过 usbmuxd 获取配对材料，避免要求读取受保护的 /var/db/lockdown。
# OpenSSL 是 Apache-2.0，与本项目许可兼容。
find_package(OpenSSL REQUIRED)
find_package(PkgConfig QUIET)
add_library(XdbCurl INTERFACE)
if(APPLE)
    # 固定系统 HTTPS 后端和系统证书库，避免 pkg-config 选择构建机的 brew curl。
    if(IS_ABSOLUTE "${CMAKE_OSX_SYSROOT}")
        set(xdb_curl_sdk "${CMAKE_OSX_SYSROOT}")
    else()
        set(xdb_curl_sdk_name "${CMAKE_OSX_SYSROOT}")
        if(NOT xdb_curl_sdk_name)
            set(xdb_curl_sdk_name macosx)
        endif()
        execute_process(COMMAND xcrun --sdk "${xdb_curl_sdk_name}" --show-sdk-path
                OUTPUT_VARIABLE xdb_curl_sdk OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
    endif()
    if(NOT EXISTS "${xdb_curl_sdk}/usr/lib/libcurl.tbd" OR
            NOT EXISTS "${xdb_curl_sdk}/usr/include/curl/curl.h")
        message(FATAL_ERROR "macOS SDK 缺少系统 libcurl；请安装 Xcode 或 Command Line Tools。")
    endif()
    # SDK 头文件已在编译器默认搜索路径；显式 -isystem usr/include 会破坏 libc++ 顺序。
    target_link_libraries(XdbCurl INTERFACE "${xdb_curl_sdk}/usr/lib/libcurl.tbd")
else()
    find_package(CURL REQUIRED)
    if(PkgConfig_FOUND AND NOT CMAKE_CROSSCOMPILING)
        pkg_check_modules(XDB_CURL QUIET IMPORTED_TARGET libcurl)
    endif()
    if(TARGET PkgConfig::XDB_CURL)
        target_link_libraries(XdbCurl INTERFACE PkgConfig::XDB_CURL)
        foreach(xdb_curl_library IN LISTS XDB_CURL_LINK_LIBRARIES)
            # Ubuntu runner 的 /usr/local 可能只有 libcurl.a；普通 pkg-config
            # 目标没有 Libs.private 的压缩/TLS/PSL 依赖。Windows 的 .dll.a
            # 是动态导入库，不能按静态库处理；只为实际静态 curl 追加闭包。
            if(xdb_curl_library MATCHES "(^|[/\\\\])libcurl\\.a$")
                # FindPkgConfig 3.20 不保证生成 STATIC_LINK_LIBRARIES；使用
                # 有文档保证的库名、搜索目录和其他选项，经接口目标传到最终链接。
                target_link_directories(XdbCurl INTERFACE ${XDB_CURL_STATIC_LIBRARY_DIRS})
                target_link_libraries(XdbCurl INTERFACE ${XDB_CURL_STATIC_LIBRARIES})
                target_link_options(XdbCurl INTERFACE ${XDB_CURL_STATIC_LDFLAGS_OTHER})
                target_compile_options(XdbCurl INTERFACE ${XDB_CURL_STATIC_CFLAGS_OTHER})
                break()
            endif()
        endforeach()
    else()
        target_link_libraries(XdbCurl INTERFACE CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
        if(ANDROID)
            target_link_libraries(XdbCurl INTERFACE z dl m)
        endif()
    endif()
endif()

# ---------------------------------------------------------------- 解码后端 ---
# 当前 VideoToolbox 适配器使用 2 字节 NAL 长度前缀，单个 NAL 最大为 65535 字节；
# 已有设备样本包含 70101 字节的 NAL。此前 4 字节配置失败的范围见解码器注释，
# 不能据此推断所有系统和码流都有相同限制。
# 找到 libavcodec 时编入软件 HEVC 解码，供 macOS 处理超限 NAL，并作为
# Linux / Windows 的视频后端。缺少开发包时仍可构建：macOS 只保留平台后端，
# 非 Apple 平台则没有可用的视频解码后端。
option(XDB_LIBAV "Use optional FFmpeg video decoding" ON)
option(XDB_LIBAV_AUDIO "Use optional FFmpeg AAC-ELD decoding" ON)
set(XDB_LIBAV_FOUND OFF)
if(XDB_LIBAV)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND AND NOT CMAKE_CROSSCOMPILING)
        pkg_check_modules(LIBAV QUIET IMPORTED_TARGET libavcodec libavutil libswscale)
    endif()
    if(CMAKE_CROSSCOMPILING AND XDB_LIBAV)
        find_path(XDB_AV_INCLUDE libavcodec/avcodec.h)
        foreach(name avcodec avutil swscale)
            string(TOUPPER "${name}" upper)
            find_library(XDB_${upper}_LIBRARY NAMES ${name} REQUIRED)
            list(APPEND xdb_av_libraries "${XDB_${upper}_LIBRARY}")
        endforeach()
        if(NOT TARGET PkgConfig::LIBAV)
            add_library(xdb_cross_libav INTERFACE)
            add_library(PkgConfig::LIBAV ALIAS xdb_cross_libav)
            target_include_directories(xdb_cross_libav INTERFACE "${XDB_AV_INCLUDE}")
            target_link_libraries(xdb_cross_libav INTERFACE ${xdb_av_libraries})
        endif()
        set(LIBAV_FOUND ON)
    endif()
    if(LIBAV_FOUND)
        set(XDB_LIBAV_FOUND ON)
    elseif(XDB_LIBAV STREQUAL "ON")
        message(WARNING
                "没找到 FFmpeg 开发库（libavcodec/libavutil/libswscale/libswresample）。软件解码后端不编，"
                "关键帧超过 65535 字节的码流会出现无法解码的丢帧。"
                "装 ffmpeg 开发件后重新配置即可。")
    endif()
endif()

# libavformat 只用于容器录制；缺少它不影响已有解码器或裸 HEVC 文件。
option(XDB_LIBAVFORMAT "使用 FFmpeg 封装 MP4 / MKV 录制" ON)
set(XDB_LIBAVFORMAT_FOUND OFF)
set(XDB_LIBAV_AUDIO_FOUND OFF)
if(XDB_LIBAV_FOUND AND XDB_LIBAV_AUDIO)
    find_path(XDB_SWRESAMPLE_INCLUDE libswresample/swresample.h)
    find_library(XDB_SWRESAMPLE_LIBRARY NAMES swresample)
    if(XDB_SWRESAMPLE_INCLUDE AND XDB_SWRESAMPLE_LIBRARY)
        set(XDB_LIBAV_AUDIO_FOUND ON)
    endif()
endif()
if(XDB_LIBAVFORMAT AND XDB_LIBAV_FOUND AND NOT CMAKE_CROSSCOMPILING)
    pkg_check_modules(RECORDING_AVFORMAT IMPORTED_TARGET libavformat)
    if(RECORDING_AVFORMAT_FOUND)
        set(XDB_LIBAVFORMAT_FOUND ON)
    endif()
endif()

# ------------------------------------------------------------- core lib -----
add_library(xdb_core STATIC
        src/util/Hex.cpp
        src/util/OutputFile.cpp
        src/ddi/File.cpp
        src/ddi/Download.cpp
        src/ddi/Selection.cpp
        src/ddi/Catalog.cpp
        src/ddi/Tss.cpp
        src/ddi/Mounter.cpp
        src/ddi/Cryptex.cpp
        src/ddi/ServiceTransport.cpp
        src/json/Json.cpp
        src/plist/Plist.cpp
        src/plist/PlistValue.cpp
        src/util/Base64.cpp
        src/xpc/XpcValue.cpp
        src/http2/Framing.cpp
        src/util/Deflate.cpp
        src/media/MediaOffer.cpp
        src/media/StreamSession.cpp
        src/media/FramePump.cpp
        src/media/ScreenshotSource.cpp
        src/media/AudioPump.cpp
        src/media/AudioRegulator.cpp
        src/media/RecordingClock.cpp
        src/media/RecordingVideoConfig.cpp
        src/media/RecordingMuxer.cpp
        src/media/Recorder.cpp
        src/rt/RtpHevc.cpp
        src/rt/RtpSeq.cpp
        src/rt/RtpTimestamp.cpp
        src/rt/Rtcp.cpp
        src/transport/Socket.cpp
        src/transport/SocketPlatform.cpp
        src/transport/Usbmux.cpp
        src/transport/TcpConnect.cpp
        src/transport/TlsChannel.cpp
        src/transport/Lockdown.cpp
        src/transport/Tunnel.cpp
        src/net/LwipRuntime.cpp
        src/net/Stack.cpp
        src/net/TcpStream.cpp
        src/net/UdpSocket.cpp
        src/remote/RemoteXpc.cpp
        src/remote/Rsd.cpp
        src/remote/Device.cpp
        src/remote/Discovery.cpp
        src/remote/Pasteboard.cpp
        src/remote/OrientationControl.cpp
        src/remote/DisplayInfo.cpp
        src/remote/App.cpp
        src/remote/PairingChannel.cpp
        src/bitstream/AnnexB.cpp
        src/hid/Hid.cpp
        src/wifi/Crypto.cpp
        src/wifi/HostMetadata.cpp
        src/wifi/Srp.cpp
        src/wifi/Opack.cpp
        src/wifi/Tlv.cpp
        src/wifi/Rppairing.cpp
        src/wifi/PairRecord.cpp
        src/wifi/Mdns.cpp
        src/wifi/DiscoveryIdentity.cpp
        src/wifi/PairingIdentity.cpp
        src/wifi/PairVerify.cpp
        src/wifi/PairSetup.cpp
        src/wifi/PairableHost.cpp
        src/remote/Pairing.cpp
        src/remote/WifiPairing.cpp
        src/wifi/RemotePairing.cpp)
target_include_directories(xdb_core PUBLIC src)
target_link_libraries(xdb_core PUBLIC XdbAppleFrameworks OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)
target_link_libraries(xdb_core PRIVATE pugixml::pugixml xdb_lwip Xdb::Mdns)
target_link_libraries(xdb_core PRIVATE XdbCurl)
target_link_libraries(xdb_core PUBLIC Xdb::Threading)
if(WIN32)
    target_sources(xdb_core PRIVATE src/wifi/PairRecordWindows.cpp)
    target_link_libraries(xdb_core PUBLIC ws2_32 PRIVATE advapi32 iphlpapi)
endif()
# -Wall/-Wextra 是 GCC/Clang 选项；MSVC 把 /Wextra 当作非法的警告等级数字报 D8021。
# MSVC 保持默认警告级别，避免与嵌入方的 /WX 设置产生新的告警错误。
if(NOT MSVC)
    target_compile_options(xdb_core PRIVATE -Wall -Wextra)
endif()

if(XDB_LIBAVFORMAT_FOUND)
    target_link_libraries(xdb_core PUBLIC PkgConfig::RECORDING_AVFORMAT)
    target_compile_definitions(xdb_core PUBLIC XDB_HAVE_LIBAVFORMAT=1)
endif()

if(XDB_LIBAV_AUDIO_FOUND)
    target_sources(xdb_core PRIVATE src/decode/FFmpegEldDecoder.cpp)
    target_compile_definitions(xdb_core PUBLIC XDB_HAVE_LIBAV_AUDIO=1)
    target_include_directories(xdb_core PRIVATE "${XDB_SWRESAMPLE_INCLUDE}")
    target_link_libraries(xdb_core PRIVATE "${XDB_SWRESAMPLE_LIBRARY}")
endif()

# Apple 平台启用 VideoToolbox 和 AudioToolbox；其他平台没有对应的系统后端。
# 软件视频解码另按 libavcodec 的可用性选择实现或报告不可用的占位实现。
if(APPLE)
    target_sources(xdb_core PRIVATE src/decode/VideoToolboxDecoder.cpp
            src/decode/AudioToolboxEldDecoder.cpp)
else()
    target_sources(xdb_core PRIVATE src/decode/PlatformDecoderFallback.cpp
            src/decode/AudioDecoderNone.cpp)
endif()
if(XDB_LIBAV_FOUND)
    target_sources(xdb_core PRIVATE src/decode/FFmpegDecoder.cpp)
    # 静态库的 libav 符号由最终可执行文件链接，需要向使用方传递链接依赖。
    target_link_libraries(xdb_core PUBLIC PkgConfig::LIBAV)
    # Decoder.h 的 kHaveDecoder 依赖这个宏；库与调用方必须看到相同定义，
    # 才能一致判断后端是否可用，并保持头文件中的内联定义一致。
    target_compile_definitions(xdb_core PUBLIC XDB_HAVE_LIBAV=1)
else()
    target_sources(xdb_core PRIVATE src/decode/SoftwareDecoderStub.cpp)
endif()


endif()

add_library(xdb_wire STATIC src/ipc/Wire.cpp src/transport/Socket.cpp src/transport/SocketPlatform.cpp)
target_include_directories(xdb_wire PUBLIC src)
target_link_libraries(xdb_wire PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
if(WIN32)
    target_link_libraries(xdb_wire PUBLIC ws2_32)
endif()
add_library(xdb_client SHARED src/client/Client.cpp src/client/Process.cpp)
target_link_libraries(xdb_client PRIVATE xdb_wire ${CMAKE_DL_LIBS})
target_include_directories(xdb_client PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_definitions(xdb_client PRIVATE XDB_CLIENT_EXPORTS)
target_include_directories(xdb_client PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
set_target_properties(xdb_client PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
if(APPLE)
    target_link_options(xdb_client PRIVATE "-Wl,-headerpad_max_install_names")
endif()
if(XDB_BUILD_SERVER)
add_library(xdb_server SHARED src/server/Server.cpp src/server/Backend.cpp src/server/Prepare.cpp)
target_link_libraries(xdb_server PRIVATE xdb_core xdb_wire)
target_include_directories(xdb_server PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_definitions(xdb_server PRIVATE XDB_SERVER_EXPORTS)
target_include_directories(xdb_server PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)
set_target_properties(xdb_server PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
endif()
if(APPLE AND XDB_BUILD_SERVER)
    target_link_options(xdb_server PRIVATE "-Wl,-headerpad_max_install_names")
endif()
if(XDB_BUILD_CLI AND XDB_BUILD_SERVER)
add_executable(xdb src/cli/Main.cpp src/cli/Help.cpp)
target_link_libraries(xdb PRIVATE xdb_client xdb_server nlohmann_json::nlohmann_json)
target_include_directories(xdb PRIVATE src)
if(APPLE)
    target_link_options(xdb PRIVATE "-Wl,-headerpad_max_install_names")
endif()
if(XDB_INSTALL)
install(TARGETS xdb RUNTIME DESTINATION bin COMPONENT Runtime)
endif()
endif()
if(XDB_INSTALL)
install(TARGETS xdb_client RUNTIME DESTINATION bin COMPONENT Runtime
    LIBRARY DESTINATION lib COMPONENT Runtime ARCHIVE DESTINATION lib COMPONENT Development)
if(XDB_BUILD_SERVER)
    install(TARGETS xdb_server RUNTIME DESTINATION bin COMPONENT Runtime
        LIBRARY DESTINATION lib COMPONENT Runtime ARCHIVE DESTINATION lib COMPONENT Development)
endif()
install(DIRECTORY include/ DESTINATION include COMPONENT Development)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/xdb/version.h" DESTINATION include/xdb COMPONENT Development)
install(FILES LICENSE COPYING.GPLv3 DESTINATION share/licenses/XDeviceBridge COMPONENT Runtime)
endif()
if(XDB_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME xdb_client_api COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/client_api_test.py $<TARGET_FILE:xdb_client> ${PROJECT_VERSION})
    set_tests_properties(xdb_client_api PROPERTIES TIMEOUT 15)
endif()
if(XDB_BUILD_TESTS AND XDB_BUILD_SERVER AND XDB_BUILD_CLI)
    add_test(NAME xdb_ipc COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ipc_test.py $<TARGET_FILE:xdb> $<TARGET_FILE:xdb_client> $<TARGET_FILE:xdb_server>)
    set_tests_properties(xdb_ipc PROPERTIES TIMEOUT 30)
    if(NOT ANDROID)
        add_test(NAME xdb_lifecycle COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/lifecycle_test.py $<TARGET_FILE:xdb> $<TARGET_FILE:xdb_client>)
        set_tests_properties(xdb_lifecycle PROPERTIES TIMEOUT 60)
        if(UNIX)
            add_test(NAME xdb_install COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_test.py)
            set_tests_properties(xdb_install PROPERTIES TIMEOUT 30)
        endif()
    endif()
    foreach(name ddi_selection json plist hid host_metadata discovery_status text_cancellation api_parameters output_file)
        add_executable(xdb_${name}_test tests/${name}_test.cpp)
        target_link_libraries(xdb_${name}_test PRIVATE xdb_core)
        add_test(NAME xdb_${name} COMMAND xdb_${name}_test)
    endforeach()
    add_test(NAME xdb_api_docs COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/api_docs_test.py)
    add_test(NAME xdb_cli_devices COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_devices_test.py $<TARGET_FILE:xdb>)
    set_tests_properties(xdb_cli_devices PROPERTIES TIMEOUT 30)
endif()

if(XDB_BUILD_TESTS AND XDB_BUILD_SERVER AND UNIX)
    foreach(name ddi_download ddi_tss ddi_mounter ddi_cryptex ddi_service_transport rtp rtp_timestamp recording_clock recording_video_config recorder pasteboard orientation_control displayinfo net_runtime service_cancellation net transport tcp_connect rsd media wifi pairing_identity pairing_workflow discovery_identity discovery usbmux_discovery_timeout xpc http2 remote_channel audio_buffer frame_capture)
        add_executable(xdb_${name}_test tests/${name}_test.cpp)
        target_link_libraries(xdb_${name}_test PRIVATE xdb_core xdb_lwip Xdb::Mdns)
        add_test(NAME xdb_${name} COMMAND xdb_${name}_test)
        set_tests_properties(xdb_${name} PROPERTIES TIMEOUT 30)
    endforeach()
endif()

if(XDB_BUILD_TESTS AND XDB_BUILD_SERVER AND UNIX)
    target_sources(xdb_wifi_test PRIVATE tests/pair_setup_reply_test.cpp)
    if(XDB_LIBAV_FOUND)
        add_executable(xdb_ffmpeg_decoder_test tests/ffmpeg_decoder_test.cpp src/decode/FFmpegDecoder.cpp)
        target_compile_definitions(xdb_ffmpeg_decoder_test PRIVATE
                avcodec_send_packet=xdb_test_send_packet avcodec_receive_frame=xdb_test_receive_frame)
        target_link_libraries(xdb_ffmpeg_decoder_test PRIVATE xdb_core)
        add_test(NAME xdb_ffmpeg_decoder COMMAND xdb_ffmpeg_decoder_test)
        set_tests_properties(xdb_ffmpeg_decoder PROPERTIES TIMEOUT 30)
    endif()
    if(XDB_LIBAV_AUDIO_FOUND)
        foreach(name audio_regulator audio_decoder)
            add_executable(xdb_${name}_test tests/${name}_test.cpp)
            target_link_libraries(xdb_${name}_test PRIVATE xdb_core)
            add_test(NAME xdb_${name} COMMAND xdb_${name}_test)
            set_tests_properties(xdb_${name} PROPERTIES TIMEOUT 30)
        endforeach()
    endif()
    set_tests_properties(xdb_remote_channel PROPERTIES TIMEOUT 60)
endif()
