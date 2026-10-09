# Pinned native WebRTC stack for live transcription (plan 36, Carrier D):
# libdatachannel (PeerConnection/ICE/DTLS-SRTP/SCTP/RTP) + opus (encoder) +
# MbedTLS 3.6 LTS (DTLS/crypto backend). Everything is fetched by commit
# hash at configure time, built static (the app links /MT), and nothing is
# staged beside the executable. All components are open source and
# GPL-3.0-compatible (MPL-2.0 / BSD / MIT / Apache-2.0 or GPL-2.0+).
#
# MbedTLS 4.x is a breaking PSA split; libdatachannel v0.24.x targets the
# 3.x API, so the pin stays on the 3.6 LTS branch and both move together.
#
# Offline builds: set OPENZOOM_NATIVE_RTC_SOURCES to a directory containing
# pre-cloned `libdatachannel`, `opus`, and `mbedtls` trees at the pinned
# commits (submodules included).

include(FetchContent)

set(OPENZOOM_LIBDATACHANNEL_VERSION "v0.24.5")
set(OPENZOOM_LIBDATACHANNEL_COMMIT "443f6934d9007eb7076ab7825ba330f355fcbead")
set(OPENZOOM_OPUS_VERSION "v1.5.2")
set(OPENZOOM_OPUS_COMMIT "ddbe48383984d56acd9e1ab6a090c54ca6b735a6")
set(OPENZOOM_MBEDTLS_VERSION "mbedtls-3.6.7")
set(OPENZOOM_MBEDTLS_COMMIT "068ff080b369adfac81509f9b57b2afabaf82dc5")

if(DEFINED ENV{OPENZOOM_NATIVE_RTC_SOURCES})
    set(FETCHCONTENT_SOURCE_DIR_LIBDATACHANNEL
        "$ENV{OPENZOOM_NATIVE_RTC_SOURCES}/libdatachannel")
    set(FETCHCONTENT_SOURCE_DIR_OPUS
        "$ENV{OPENZOOM_NATIVE_RTC_SOURCES}/opus")
    set(FETCHCONTENT_SOURCE_DIR_MBEDTLS
        "$ENV{OPENZOOM_NATIVE_RTC_SOURCES}/mbedtls")
endif()

# --- MbedTLS: built and installed at configure time -------------------------
# libdatachannel and its vendored libSRTP both discover MbedTLS through
# disk-searching find modules, so an installed tree inside the build
# directory is the one layout that satisfies every consumer. The build runs
# once and is cached; reconfigures skip it.
FetchContent_Declare(mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG ${OPENZOOM_MBEDTLS_COMMIT}
    # Populate through the supported MakeAvailable API without adding a
    # second in-tree Mbed TLS build. libdatachannel's find modules consume
    # the private installed tree below.
    SOURCE_SUBDIR _openzoom_populate_only
)
FetchContent_MakeAvailable(mbedtls)

# DTLS-SRTP (used by WebRTC) is compiled out of MbedTLS by default; enable
# it in a build-local copy. Never edit the fetched/offline source tree: it
# may be read-only or shared by several builds. The custom copy is also
# installed over the private install tree's default header so Mbed TLS and
# libdatachannel compile against the exact same feature set.
set(OPENZOOM_MBEDTLS_CONFIG_DIR "${CMAKE_BINARY_DIR}/nativertc/config")
file(MAKE_DIRECTORY "${OPENZOOM_MBEDTLS_CONFIG_DIR}")
set(OPENZOOM_MBEDTLS_CONFIG_HEADER
    "${OPENZOOM_MBEDTLS_CONFIG_DIR}/mbedtls_config.h")
configure_file(
    "${mbedtls_SOURCE_DIR}/include/mbedtls/mbedtls_config.h"
    "${OPENZOOM_MBEDTLS_CONFIG_HEADER}"
    COPYONLY)
file(READ "${OPENZOOM_MBEDTLS_CONFIG_HEADER}" _mbedtls_config_text)
string(FIND "${_mbedtls_config_text}" "\n#define MBEDTLS_SSL_DTLS_SRTP"
       _mbedtls_srtp_enabled)
if(_mbedtls_srtp_enabled EQUAL -1)
    string(REPLACE "//#define MBEDTLS_SSL_DTLS_SRTP"
                   "#define MBEDTLS_SSL_DTLS_SRTP"
                   _mbedtls_config_text "${_mbedtls_config_text}")
    file(WRITE "${OPENZOOM_MBEDTLS_CONFIG_HEADER}"
         "${_mbedtls_config_text}")
endif()
file(SHA256 "${OPENZOOM_MBEDTLS_CONFIG_HEADER}" _mbedtls_config_sha256)

set(OPENZOOM_MBEDTLS_BUILD_DIR "${CMAKE_BINARY_DIR}/nativertc/mbedtls-build")
set(OPENZOOM_MBEDTLS_INSTALL_DIR "${CMAKE_BINARY_DIR}/nativertc/mbedtls-install")
set(_mbedtls_stamp "${OPENZOOM_MBEDTLS_INSTALL_DIR}/openzoom-build.txt")
set(_mbedtls_expected_stamp
    "commit=${OPENZOOM_MBEDTLS_COMMIT}\nconfig=${_mbedtls_config_sha256}\nruntime=MultiThreaded\n")
set(_mbedtls_actual_stamp "")
if(EXISTS "${_mbedtls_stamp}")
    file(READ "${_mbedtls_stamp}" _mbedtls_actual_stamp)
endif()
if(NOT _mbedtls_actual_stamp STREQUAL _mbedtls_expected_stamp)
    file(REMOVE_RECURSE "${OPENZOOM_MBEDTLS_BUILD_DIR}")
    file(REMOVE_RECURSE "${OPENZOOM_MBEDTLS_INSTALL_DIR}")
endif()
if(NOT EXISTS "${OPENZOOM_MBEDTLS_INSTALL_DIR}/include/mbedtls/ssl.h")
    message(STATUS "Building pinned MbedTLS ${OPENZOOM_MBEDTLS_VERSION} (once)")
    # A failed earlier attempt must not leak a poisoned compiler cache into
    # this configure.
    file(REMOVE_RECURSE "${OPENZOOM_MBEDTLS_BUILD_DIR}")
    # The parent project enables CXX only; the child auto-detects the C
    # compiler from the same developer environment. When the parent knows a
    # C compiler, pass it through explicitly.
    set(_mbedtls_compiler_args "")
    if(CMAKE_C_COMPILER)
        list(APPEND _mbedtls_compiler_args
             "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S "${mbedtls_SOURCE_DIR}"
            -B "${OPENZOOM_MBEDTLS_BUILD_DIR}"
            -G "${CMAKE_GENERATOR}"
            -DCMAKE_BUILD_TYPE=Release
            ${_mbedtls_compiler_args}
            # Match the application's static runtime; MbedTLS is C, so one
            # Release/MT build serves every app configuration. The policy
            # default makes MSVC_RUNTIME_LIBRARY effective despite MbedTLS's
            # older cmake_minimum_required.
            -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
            -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DENABLE_PROGRAMS=OFF
            -DENABLE_TESTING=OFF
            -DGEN_FILES=OFF
            "-DMBEDTLS_CONFIG_FILE:FILEPATH=${OPENZOOM_MBEDTLS_CONFIG_HEADER}"
            -DCMAKE_INSTALL_PREFIX=${OPENZOOM_MBEDTLS_INSTALL_DIR}
        RESULT_VARIABLE _mbedtls_configure_result)
    if(NOT _mbedtls_configure_result EQUAL 0)
        message(FATAL_ERROR "MbedTLS configure failed")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build "${OPENZOOM_MBEDTLS_BUILD_DIR}"
            --target install --config Release
        RESULT_VARIABLE _mbedtls_build_result)
    if(NOT _mbedtls_build_result EQUAL 0)
        message(FATAL_ERROR "MbedTLS build failed")
    endif()
    file(COPY_FILE
        "${OPENZOOM_MBEDTLS_CONFIG_HEADER}"
        "${OPENZOOM_MBEDTLS_INSTALL_DIR}/include/mbedtls/mbedtls_config.h"
        ONLY_IF_DIFFERENT)
    file(WRITE "${_mbedtls_stamp}" "${_mbedtls_expected_stamp}")
endif()
list(APPEND CMAKE_PREFIX_PATH "${OPENZOOM_MBEDTLS_INSTALL_DIR}")

# --- opus -------------------------------------------------------------------
FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG ${OPENZOOM_OPUS_COMMIT}
)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
# OpenZoom and every other native dependency use the static MSVC runtime.
# Matching Opus to /MT prevents cross-CRT allocation and debug/release traps.
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)

# --- libdatachannel (vendored libjuice/libsrtp/usrsctp/plog submodules) -----
FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG ${OPENZOOM_LIBDATACHANNEL_COMMIT}
)
# libdatachannel's own option() would set the global BUILD_SHARED_LIBS cache
# entry to ON, flipping opus to a DLL on reconfigure. Everything here is
# static, always.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
# The vendored libSRTP registers its driver binaries with ctest by default;
# they are not OpenZoom's tests and are excluded from the build.
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(opus libdatachannel)

# The application enables Qt AUTOGEN globally. None of these plain C/C++
# dependencies contains Qt metadata; opt them out explicitly so CMake does not
# schedule no-op autogen scans or emit one "No valid Qt version" warning per
# native target (including libdatachannel's excluded alternate static target).
foreach(_openzoom_native_target IN ITEMS
        opus datachannel datachannel-static usrsctp srtp2 juice juice-static)
    if(TARGET ${_openzoom_native_target})
        set_target_properties(${_openzoom_native_target} PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF)
    endif()
endforeach()

# Bounded surface handed to the application.
if(NOT TARGET OpenZoom::NativeRtc)
    add_library(openzoom_native_rtc INTERFACE)
    add_library(OpenZoom::NativeRtc ALIAS openzoom_native_rtc)
    target_link_libraries(openzoom_native_rtc INTERFACE
        # BUILD_SHARED_LIBS is forced OFF above, so the primary target is
        # already static. Linking the separate *Static target compiled the
        # same library twice because the primary target remains in ALL.
        LibDataChannel::LibDataChannel
        opus
    )
endif()
