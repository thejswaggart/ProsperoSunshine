# SPDX-License-Identifier: GPL-3.0-only
# PS4 build, included from CMakeLists.txt when the PS4 SDK's toolchain file (orbis-cmake) is used. The first milestone:
# Sunshine's protocol core with the PS4 replacements in src/sunshine/*_ps4.cpp, video from a recorded H.264 file
# (src/capture/replay_backend.cpp), silent audio and no input. scripts/build.sh with PROSPERO_TARGET=ps4 drives it.

# Portable pieces the PS5 build has as well.
add_library(prospero_ps4_core STATIC src/capture/capture_session.cpp src/capture/replay_backend.cpp)
target_include_directories(prospero_ps4_core PUBLIC src)
target_compile_options(prospero_ps4_core PRIVATE ${PROSPERO_WARNINGS})

add_library(prospero_ps4_diag STATIC src/platform/ps5/diag.c)
target_include_directories(prospero_ps4_diag PUBLIC src)
target_compile_options(prospero_ps4_diag PRIVATE ${PROSPERO_WARNINGS})

add_library(prospero_ps4_mdns STATIC src/net/mdns.cpp src/platform/ps5/mdns_responder.cpp)
target_include_directories(prospero_ps4_mdns PUBLIC src)
target_link_libraries(prospero_ps4_mdns PUBLIC prospero_ps4_diag)
target_compile_options(prospero_ps4_mdns PRIVATE ${PROSPERO_WARNINGS})

# The screen capture: PBS-Payload's PS4 capture (pinned in deps.lock) with the sources and defines of its own PS4 stream
# build (payload/build.sh, "stream" at 720p60: Avcap inside SceRemotePlay on a worker thread, the exported ReadVideo, the
# kernel layout found at run time), plus src/platform/ps4/pbs_session.c, which runs one session the way its main.c does.
set(PBS_SRC "${CMAKE_SOURCE_DIR}/.cache/deps/src/pbs-payload/payload/src" CACHE PATH "PBS-Payload's payload/src")
if(NOT EXISTS "${PBS_SRC}/platform/ps4/capture/capture.c")
    message(FATAL_ERROR "No PBS-Payload sources in ${PBS_SRC}: run PROSPERO_TARGET=ps4 scripts/deps.sh first")
endif()
# An object library, linked straight into the host as PBS's own build does: worker/remote.c only supplies the
# pbs_worker_remote section that boot.c finds through __start_/__stop_ symbols, which never pull a member from an archive.
add_library(prospero_ps4_pbs OBJECT
    "${PBS_SRC}/platform/ps4/capture/capture.c" "${PBS_SRC}/platform/ps4/capture/remote.c"
    "${PBS_SRC}/platform/ps4/capture/export_reader.c" "${PBS_SRC}/platform/ps4/worker/client.c"
    "${PBS_SRC}/platform/ps4/worker/remote.c" "${PBS_SRC}/platform/ps4/worker/boot.c"
    "${PBS_SRC}/platform/ps4/system/control.c" "${PBS_SRC}/platform/ps4/system/credentials.c"
    "${PBS_SRC}/platform/ps4/system/layout.c" "${PBS_SRC}/platform/ps4/system/layout_runtime.c"
    "${PBS_SRC}/common/process/inject.c"
    src/platform/ps4/pbs_session.c)
target_include_directories(prospero_ps4_pbs PRIVATE "${PBS_SRC}" src)
# PBS's C as its build compiles it (-Wall -Wextra -Wno-unused-parameter, no -Werror).
target_compile_options(prospero_ps4_pbs PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(prospero_ps4_pbs PRIVATE PBS_PS4
    PBS_PS4_CAPTURE_USAGE=0 PBS_PS4_IMAGE_PASTE=0 PBS_PS4_READ_WAIT_EVENTS=0 PBS_PS4_REMOTE_AVCAP=1
    PBS_PS4_REMOTE_LIFECYCLE=0 PBS_PS4_NATIVE_SOURCE=0 PBS_PS4_ENCODER_STATE=0 PBS_PS4_ADMISSION_PROBE=0
    PBS_PS4_LATCH_BYPASS=0 PBS_PS4_BUDGET_UNLOCK=0 PBS_PS4_LOW_MEMORY=0 PBS_PS4_REMOTE_1080=0 PBS_PS4_MEMORY_COUNT=3
    PBS_PS4_DAEMON_DUMP=0 PBS_PS4_VENC_JOURNAL=0 PBS_PS4_CAPTURE_HEIGHT=720 PBS_PS4_CAPTURE_FPS=60
    PBS_PS4_REMOTE_REQUEST=3 PBS_PS4_REMOTE_IDLE_RUN=0 PBS_PS4_WORKER_BOOT=1 PBS_PS4_EXPORTED_READ=1
    PBS_PS4_DYNAMIC_LAYOUT=1)

add_library(prospero_ps4_capture STATIC src/platform/ps4/pbs_backend.cpp)
target_include_directories(prospero_ps4_capture PUBLIC src)
target_compile_options(prospero_ps4_capture PRIVATE ${PROSPERO_WARNINGS})
target_link_libraries(prospero_ps4_capture PUBLIC prospero_ps4_core)

set(SUNSHINE_SRC "${CMAKE_SOURCE_DIR}/.cache/deps/src/sunshine" CACHE PATH "Extracted Sunshine tree")
set(MOONLIGHT_SRC "${CMAKE_SOURCE_DIR}/.cache/deps/src/moonlight-common-c" CACHE PATH "moonlight-common-c tree")
if(NOT EXISTS "${PROSPERO_DEPS}/include/boost" OR NOT EXISTS "${PROSPERO_DEPS}/lib/libavcodec.a")
    message(FATAL_ERROR "No PS4 dependencies in ${PROSPERO_DEPS}: run PROSPERO_TARGET=ps4 scripts/deps.sh first")
endif()

file(GLOB BOOST_LIBS "${PROSPERO_DEPS}/lib/libboost_*.a")
set(SUNSHINE_KEPT
    nvhttp.cpp httpcommon.cpp rtsp.cpp stream.cpp crypto.cpp network.cpp audio.cpp input.cpp config.cpp
    logging.cpp cbs.cpp file_handler.cpp globals.cpp stat_trackers.cpp)
list(TRANSFORM SUNSHINE_KEPT PREPEND "${SUNSHINE_SRC}/src/")

# Cover art for the single app, embedded so the host can install it without a separate upload. The PS5 picture for now.
set(COVER_PNG "${CMAKE_SOURCE_DIR}/assets/ps5_screen.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${COVER_PNG}")
file(READ "${COVER_PNG}" COVER_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," COVER_BYTES "${COVER_HEX}")
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated/cover_png.cpp" CONTENT
    "// Generated from assets/ps5_screen.png by cmake/ps4.cmake\n#include <cstddef>\nextern const unsigned char prospero_cover_png[] = {@COVER_BYTES@};\nextern const std::size_t prospero_cover_png_size = sizeof prospero_cover_png;\n")

add_executable(prospero_sunshine_ps4 "${CMAKE_BINARY_DIR}/generated/cover_png.cpp"
    ${SUNSHINE_KEPT}
    "${MOONLIGHT_SRC}/src/RtspParser.c"
    src/sunshine/main_ps4.cpp src/sunshine/video_ps4.cpp src/sunshine/process_ps4.cpp src/sunshine/platform_ps4.cpp
    src/sunshine/pin_page_ps4.cpp src/sunshine/stubs_ps5.cpp src/apps/titles.cpp)
# As on the PS5: Sunshine includes its own headers as "src/..." and "..."; FFmpeg's internal headers include a "config.h"
# that must not resolve to Sunshine's, hence -idirafter for the Sunshine tree.
target_include_directories(prospero_sunshine_ps4 PRIVATE src "${SUNSHINE_SRC}" "${CMAKE_BINARY_DIR}/third-party")
target_include_directories(prospero_sunshine_ps4 SYSTEM PRIVATE "${PROSPERO_DEPS}/include" "${PROSPERO_DEPS}/include/nanors")
target_compile_options(prospero_sunshine_ps4 PRIVATE "SHELL:-idirafter ${SUNSHINE_SRC}/src")
# Sunshine expects moonlight-common-c headers under <moonlight-common-c/src/...>.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/third-party")
file(CREATE_LINK "${MOONLIGHT_SRC}" "${CMAKE_BINARY_DIR}/third-party/moonlight-common-c" SYMBOLIC)
set(PROSPERO_COMMIT "$ENV{PROSPERO_COMMIT}")
if(NOT PROSPERO_COMMIT)
    set(PROSPERO_COMMIT "unknown")
endif()
target_compile_definitions(prospero_sunshine_ps4 PRIVATE
    PROJECT_NAME="ProsperoSunshine" PROJECT_VERSION="0.6.6-ps4${PROSPERO_FLAVOR_SUFFIX}" PROJECT_VERSION_MAJOR="0"
    PROJECT_VERSION_MINOR="6" PROJECT_VERSION_PATCH="6" PROJECT_VERSION_COMMIT="${PROSPERO_COMMIT}"
    PROJECT_FQDN="dev.prosperosunshine" SUNSHINE_ASSETS_DIR="/data/prosperosunshine/assets"
    SUNSHINE_PUBLISHER_NAME="ProsperoSunshine" SUNSHINE_PUBLISHER_WEBSITE=""
    SUNSHINE_PUBLISHER_ISSUE_URL="" SUNSHINE_TRAY=0 BOOST_ALL_NO_LIB)
target_link_libraries(prospero_sunshine_ps4 PRIVATE prospero_ps4_pbs prospero_ps4_capture prospero_ps4_core prospero_ps4_mdns prospero_ps4_diag
    "-Wl,--start-group" ${BOOST_LIBS} "${PROSPERO_DEPS}/lib/libdisplaydevice_common.a"
    "${PROSPERO_DEPS}/lib/libavcodec.a" "${PROSPERO_DEPS}/lib/libswscale.a" "${PROSPERO_DEPS}/lib/libavutil.a"
    dep::ssl dep::crypto dep::opus dep::enet dep::nanors dep::miniupnpc "-Wl,--end-group" c++experimental
    # libc++'s std::filesystem needs openat, unlinkat, fchmodat and pathconf, which only the libkernel_sys stub exports. At
    # run time the SDK's loader (crt/rtld.c) maps libkernel, libkernel_web and libkernel_sys to the same module.
    kernel_sys)
