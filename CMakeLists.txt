cmake_minimum_required(VERSION 3.16)
project(AirBandScan C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig)

# ---------------------------------------------------------------------------
# RTL-SDR (required)
# ---------------------------------------------------------------------------
if(PkgConfig_FOUND)
    pkg_check_modules(RTLSDR QUIET librtlsdr)
endif()
if(NOT RTLSDR_FOUND)
    find_library(RTLSDR_LIBRARIES NAMES rtlsdr librtlsdr REQUIRED)
    find_path(RTLSDR_INCLUDE_DIRS NAMES rtl-sdr.h REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Optional SDR back-ends (same detection as AirScope)
# ---------------------------------------------------------------------------
find_library(AIRSPY_LIBRARIES NAMES airspy libairspy)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SDRPLAY_ARCH x64)
else()
    set(SDRPLAY_ARCH x86)
endif()
find_path(SDRPLAY_INCLUDE_DIRS NAMES sdrplay_api.h
    HINTS ${SDRPLAY_API_ROOT} "$ENV{SDRPLAY_API_PATH}" "$ENV{ProgramFiles}/SDRplay/API"
    PATH_SUFFIXES inc)
find_library(SDRPLAY_LIBRARIES NAMES sdrplay_api
    HINTS ${SDRPLAY_API_ROOT} "$ENV{SDRPLAY_API_PATH}" "$ENV{ProgramFiles}/SDRplay/API"
    PATH_SUFFIXES ${SDRPLAY_ARCH})
find_path(HACKRF_INCLUDE_DIRS NAMES libhackrf/hackrf.h)
find_library(HACKRF_LIBRARIES NAMES hackrf libhackrf)

option(WITH_AIRSPY  "Airspy support"  ON)
option(WITH_SDRPLAY "SDRplay support" ON)
option(WITH_HACKRF  "HackRF support"  ON)

# ---------------------------------------------------------------------------
# Web UI embedded into the binary (rebuilt when ui.html changes)
# ---------------------------------------------------------------------------
set(UI_SRC ${CMAKE_SOURCE_DIR}/src/web/ui.html)
set(UI_CPP ${CMAKE_BINARY_DIR}/ui_html.cpp)
add_custom_command(
    OUTPUT ${UI_CPP}
    COMMAND ${CMAKE_COMMAND} -DIN=${UI_SRC} -DOUT=${UI_CPP} -DNAME=kUiHtml
            -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${UI_SRC} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Embedding web UI")

add_executable(airbandscan
    src/main.cpp
    src/engine/config.cpp
    src/engine/scanner.cpp
    src/engine/channel.cpp
    src/engine/classifier.cpp
    src/engine/recording_store.cpp
    src/audio/audio_files.cpp
    src/web/http_server.cpp
    src/web/api.cpp
    src/sdr/rtl_sdr_source.cpp
    src/sdr/wav_file_source.cpp
    src/dsp/jfft.cpp
    src/util/log.cpp
    ${UI_CPP}
)
target_include_directories(airbandscan PRIVATE src ${RTLSDR_INCLUDE_DIRS})
target_link_directories(airbandscan PRIVATE ${RTLSDR_LIBRARY_DIRS})
target_compile_definitions(airbandscan PRIVATE _USE_MATH_DEFINES)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(airbandscan PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
target_link_libraries(airbandscan PRIVATE ${RTLSDR_LIBRARIES} Threads::Threads)
set(APP_VERSION "" CACHE STRING "Version string baked into the binary (default: src/version.h)")
if(APP_VERSION)
    target_compile_definitions(airbandscan PRIVATE AIRBANDSCAN_VERSION="${APP_VERSION}")
endif()

if(WITH_AIRSPY AND AIRSPY_LIBRARIES)
    message(STATUS "Airspy: ${AIRSPY_LIBRARIES}")
    target_sources(airbandscan PRIVATE src/sdr/airspy_source.cpp)
    target_compile_definitions(airbandscan PRIVATE HAS_AIRSPY=1)
    target_include_directories(airbandscan PRIVATE ${CMAKE_SOURCE_DIR}/third_party/airspy)
    target_link_libraries(airbandscan PRIVATE ${AIRSPY_LIBRARIES})
endif()
if(WITH_SDRPLAY AND SDRPLAY_INCLUDE_DIRS AND SDRPLAY_LIBRARIES)
    message(STATUS "SDRplay: ${SDRPLAY_LIBRARIES}")
    target_sources(airbandscan PRIVATE src/sdr/sdrplay_source.cpp)
    target_compile_definitions(airbandscan PRIVATE HAS_SDRPLAY=1)
    target_include_directories(airbandscan PRIVATE ${SDRPLAY_INCLUDE_DIRS})
    target_link_libraries(airbandscan PRIVATE ${SDRPLAY_LIBRARIES})
endif()
if(WITH_HACKRF AND HACKRF_INCLUDE_DIRS AND HACKRF_LIBRARIES)
    message(STATUS "HackRF: ${HACKRF_LIBRARIES}")
    target_sources(airbandscan PRIVATE src/sdr/hackrf_source.cpp)
    target_compile_definitions(airbandscan PRIVATE HAS_HACKRF=1)
    target_include_directories(airbandscan PRIVATE ${HACKRF_INCLUDE_DIRS})
    target_link_libraries(airbandscan PRIVATE ${HACKRF_LIBRARIES})
endif()

if(WIN32)
    target_link_libraries(airbandscan PRIVATE ws2_32)
    target_compile_definitions(airbandscan PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # Copy the MinGW runtime + SDR DLLs next to the exe so it runs standalone.
    get_filename_component(MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(dll libgcc_s_seh-1.dll libwinpthread-1.dll libstdc++-6.dll
                librtlsdr.dll libusb-1.0.dll libmp3lame-0.dll libairspy.dll libhackrf.dll)
        if(EXISTS "${MINGW_BIN}/${dll}")
            add_custom_command(TARGET airbandscan POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MINGW_BIN}/${dll}" "$<TARGET_FILE_DIR:airbandscan>")
        endif()
    endforeach()
else()
    target_link_libraries(airbandscan PRIVATE ${CMAKE_DL_LIBS})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
        target_link_libraries(airbandscan PRIVATE stdc++fs)
    endif()
endif()

install(TARGETS airbandscan RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Tests (not built by default): ninja -C build hop_test && ./build/hop_test
# ---------------------------------------------------------------------------
add_executable(hop_test EXCLUDE_FROM_ALL
    tests/hop_test.cpp src/engine/config.cpp src/engine/scanner.cpp src/engine/channel.cpp
    src/engine/classifier.cpp src/engine/recording_store.cpp src/audio/audio_files.cpp
    src/sdr/rtl_sdr_source.cpp src/sdr/wav_file_source.cpp src/dsp/jfft.cpp src/util/log.cpp)
target_include_directories(hop_test PRIVATE src tests ${RTLSDR_INCLUDE_DIRS})
target_compile_definitions(hop_test PRIVATE AIRBANDSCAN_SIM _USE_MATH_DEFINES)
target_link_libraries(hop_test PRIVATE ${RTLSDR_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS})
