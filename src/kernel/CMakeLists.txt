set(REXKERNEL_SOURCES
    kernel_init.cpp

    # XAM subsystem - provides __imp__Xam* symbols
    xam/xam_module.cpp
    xam/xam_avatar.cpp
    xam/xam_content.cpp
    xam/xam_content_aggregate.cpp
    xam/xam_content_device.cpp
    xam/xam_debug.cpp
    xam/xam_enum.cpp
    xam/xam_info.cpp
    xam/xam_input.cpp
    xam/xam_locale.cpp
    xam/xam_msg.cpp
    xam/xam_net.cpp
    xam/xam_notify.cpp
    xam/xam_nui.cpp
    xam/xam_party.cpp
    xam/xam_task.cpp
    xam/xam_ui.cpp
    xam/xam_user.cpp
    xam/xam_video.cpp
    xam/xam_voice.cpp
    xam/xam_misc.cpp
    xam/apps/xam_app.cpp
    xam/apps/xgi_app.cpp
    xam/apps/xlivebase_app.cpp
    xam/apps/xmp_app.cpp

    # WWE All Stars recomp: online play (.scratch/online-play/)
    xam/live/http.cpp
    xam/live/identity.cpp
    xam/live/live.cpp
    xam/live/relay.cpp
    xam/live/relay_protocol.cpp
    xam/live/upnp.cpp
    xam/live/session_data.cpp
    xam/live/xlive_json.cpp
    xam/live/xsession.cpp

    # XBDM subsystem - provides __imp__Xbdm* symbols
    xbdm/xbdm_module.cpp
    xbdm/xbdm_misc.cpp

    # xboxkrnl exports - provides __imp__ symbols for generated code
    # xboxkrnl/cert_monitor.cpp    # TODO: Translate JIT methods for AOT
    # xboxkrnl/debug_monitor.cpp   # TODO: Translate JIT methods for AOT
    xboxkrnl/xboxkrnl_crypt.cpp
    xboxkrnl/xboxkrnl_debug.cpp
    xboxkrnl/xboxkrnl_error.cpp
    xboxkrnl/xboxkrnl_hal.cpp
    xboxkrnl/xboxkrnl_hid.cpp
    xboxkrnl/xboxkrnl_io.cpp
    xboxkrnl/xboxkrnl_io_info.cpp
    xboxkrnl/xboxkrnl_memory.cpp
    xboxkrnl/xboxkrnl_misc.cpp
    xboxkrnl/xboxkrnl_module.cpp
    xboxkrnl/xboxkrnl_modules.cpp
    xboxkrnl/xboxkrnl_ob.cpp
    xboxkrnl/xboxkrnl_rtl.cpp
    xboxkrnl/xboxkrnl_strings.cpp
    xboxkrnl/xboxkrnl_threading.cpp
    # xboxkrnl/xboxkrnl_usbcam.cpp  # TODO: lol eventually.
    xboxkrnl/xboxkrnl_video.cpp
    xboxkrnl/xboxkrnl_xconfig.cpp
    xboxkrnl/xboxkrnl_audio.cpp
    xboxkrnl/xboxkrnl_audio_xma.cpp

    crt/heap.cpp
    crt/file.cpp
    crt/memory.cpp
    crt/string.cpp
    crt/threading.cpp
)

list(TRANSFORM REXKERNEL_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
target_sources(rexruntime PRIVATE ${REXKERNEL_SOURCES})

target_link_libraries(rexruntime PRIVATE o1heap rexcore rexfilesystem rexui rexaudio rexinput imgui)

# WWE All Stars recomp, online play: the client ported from the Xenia netplay fork talks
# HTTP(S) to the game server with libcurl (Schannel) and reads its JSON with rapidjson, from
# the source folders scripts/bootstrap.ps1 fetches at the fork's pinned commits.
set(REXGLUE_CURL_SOURCE_DIR "" CACHE PATH "libcurl source folder (online play)")
set(REXGLUE_RAPIDJSON_SOURCE_DIR "" CACHE PATH "rapidjson source folder (online play)")
if(NOT EXISTS "${REXGLUE_CURL_SOURCE_DIR}/CMakeLists.txt" OR
   NOT EXISTS "${REXGLUE_RAPIDJSON_SOURCE_DIR}/include/rapidjson/rapidjson.h")
    message(FATAL_ERROR "Set REXGLUE_CURL_SOURCE_DIR and REXGLUE_RAPIDJSON_SOURCE_DIR "
        "(scripts/bootstrap.ps1 fetches both).")
endif()
set(BUILD_SHARED_LIBS OFF)
foreach(_curl_on BUILD_STATIC_LIBS CURL_DISABLE_INSTALL HTTP_ONLY)
    set(${_curl_on} ON CACHE BOOL "" FORCE)
endforeach()
foreach(_curl_off BUILD_CURL_EXE BUILD_TESTING BUILD_LIBCURL_DOCS BUILD_MISC_DOCS
        ENABLE_CURL_MANUAL BUILD_EXAMPLES CURL_ZLIB CURL_BROTLI CURL_ZSTD USE_NGHTTP2
        USE_LIBIDN2 USE_WIN32_IDN CURL_USE_LIBPSL CURL_USE_LIBSSH2 CURL_USE_LIBSSH
        CURL_USE_GSSAPI CURL_USE_OPENSSL CURL_USE_MBEDTLS CURL_USE_WOLFSSL CURL_USE_GNUTLS
        CURL_USE_RUSTLS ENABLE_UNICODE CURL_LTO)
    set(${_curl_off} OFF CACHE BOOL "" FORCE)
endforeach()
if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(${REXGLUE_CURL_SOURCE_DIR} ${CMAKE_BINARY_DIR}/thirdparty/curl EXCLUDE_FROM_ALL)
target_link_libraries(rexruntime PRIVATE CURL::libcurl_static)
target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_RAPIDJSON_SOURCE_DIR}/include>)

# UPnP port mapping (--wwe_online_upnp) with miniupnpc, at the fork's pinned commit.
set(REXGLUE_MINIUPNPC_SOURCE_DIR "" CACHE PATH "miniupnpc source folder (online play)")
if(NOT EXISTS "${REXGLUE_MINIUPNPC_SOURCE_DIR}/include/miniupnpc.h")
    message(FATAL_ERROR "Set REXGLUE_MINIUPNPC_SOURCE_DIR (scripts/bootstrap.ps1 fetches it).")
endif()
foreach(_upnpc_off UPNPC_BUILD_SHARED UPNPC_BUILD_TESTS UPNPC_BUILD_SAMPLE)
    set(${_upnpc_off} OFF CACHE BOOL "" FORCE)
endforeach()
set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${REXGLUE_MINIUPNPC_SOURCE_DIR} ${CMAKE_BINARY_DIR}/thirdparty/miniupnpc
    EXCLUDE_FROM_ALL)
target_link_libraries(rexruntime PRIVATE libminiupnpc-static)
