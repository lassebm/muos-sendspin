cmake_minimum_required(VERSION 3.24)
project(muos-sendspin LANGUAGES C CXX HOMEPAGE_URL "https://github.com/lassebm/muos-sendspin")

# C++23 (sendspin-cpp needs at least C++20). GCC 14 (device container) and Apple clang 21.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(MUOS_DEVICE "Build for the muOS device (inside docker/, against upstream SDL 2.28.5)" OFF)
if(MUOS_DEVICE)
    # Each function and object in its own section, so the link drops what's unused (--gc-sections
    # below). Set before the dependencies are added, so it covers them too.
    add_compile_options(-ffunction-sections -fdata-sections)
endif()

include(FetchContent)

# ------------------------------------------------------------------------------
# sendspin-cpp (official Sendspin SDK)
# ------------------------------------------------------------------------------
# Roles we use: player, metadata, artwork, controller (+ color, for theming the UI from artwork).
# Source (mic capture) and visualizer aren't needed on a handheld receiver.
set(SENDSPIN_ENABLE_PLAYER     ON  CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_METADATA   ON  CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_ARTWORK    ON  CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_CONTROLLER ON  CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_COLOR      ON  CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_SOURCE     OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_VISUALIZER OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES             OFF CACHE BOOL "" FORCE)
# Debug aid: log inbound protocol messages and audio chunk timestamps (sync investigations).
option(MUOS_SENDSPIN_TRACE "Trace inbound Sendspin messages and audio timestamps" OFF)
set(SENDSPIN_ENABLE_MESSAGE_TRACE ${MUOS_SENDSPIN_TRACE} CACHE BOOL "" FORCE)

FetchContent_Declare(
    sendspin_cpp
    GIT_REPOSITORY https://github.com/Sendspin/sendspin-cpp.git
    GIT_TAG        b95cf854a784ed308b1f8fe9697f414b02768ddd # v0.9.5
)
FetchContent_MakeAvailable(sendspin_cpp)

# ------------------------------------------------------------------------------
# LVGL (the UI toolkit; rendered into an SDL texture, see src/ui/backend.h)
# ------------------------------------------------------------------------------
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        80ca777e37a2b176770726a02e07a6fb79ef0b39  # v9.6.0
)
FetchContent_MakeAvailable(lvgl)

# ------------------------------------------------------------------------------
# SDL2
# ------------------------------------------------------------------------------
# Desktops: the system's SDL2 (Homebrew's is sdl2-compat). Device: the device's own patched
# libSDL2 (muOS's patches add rotation and controller mapping). Device SDL is 2.28.5, so don't use
# any API newer than that.
if(MUOS_DEVICE)
    # Compile and link against upstream SDL 2.28.5, built into the docker image. muOS's patched
    # libSDL2 has the same SONAME and API (its patches change behaviour; it exports a superset:
    # checked, every SDL symbol we use is in both). At runtime the SONAME resolves to the device's
    # /usr/lib copy: no rpath, never bundle SDL. Pinning the version keeps any API newer than the
    # device's out at compile time.
    set(SDL2_INCLUDE_DIR "/opt/sdl2/include/SDL2" CACHE PATH "SDL 2.28.5 headers")
    set(SDL2_LINK_LIB "/opt/sdl2/lib/libSDL2-2.0.so.0" CACHE FILEPATH "SDL 2.28.5 library to link against")
    add_library(SDL2::SDL2 SHARED IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION "${SDL2_LINK_LIB}"
        IMPORTED_SONAME "libSDL2-2.0.so.0"
        INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIR}")
else()
    find_package(SDL2 REQUIRED CONFIG)
endif()

# ------------------------------------------------------------------------------
# App
# ------------------------------------------------------------------------------
add_executable(muos-sendspin
    src/app/main.cpp
    src/app/app.cpp
    src/app/options.cpp
    src/net/discovery.cpp
    src/net/mdns_responder.cpp
    src/net/netinfo.cpp
    src/platform/alsa.cpp
    src/platform/input.cpp
    src/platform/log.cpp
    src/platform/muos.cpp
    src/platform/paths.cpp
    src/player/artwork.cpp
    src/player/audio_sdl.cpp
    src/player/sendspin_service.cpp
    src/player/settings.cpp
    src/player/stb_impl.cpp
    src/ui/backend.cpp
    src/ui/glyphs.cpp
    src/ui/layout.cpp
    src/ui/ui.cpp
    src/ui/view.cpp
    # The SDK's reference persistence provider (atomic JSON file: write tmp, fsync, rename).
    ${sendspin_cpp_SOURCE_DIR}/examples/common/file_persistence_provider.cpp
)

target_include_directories(muos-sendspin PRIVATE src third_party/stb
    ${sendspin_cpp_SOURCE_DIR}/examples/common)
# The version comes from git tags (cmake/version.cmake): generated now, so tools that read
# compile_commands.json find version.h, and refreshed before every build.
set(VERSION_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(VERSION_HEADER "${CMAKE_BINARY_DIR}/generated/version.h")
include(cmake/version.cmake)
add_custom_target(version
    COMMAND ${CMAKE_COMMAND} -DVERSION_SOURCE_DIR=${VERSION_SOURCE_DIR} -DVERSION_HEADER=${VERSION_HEADER} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake
    BYPRODUCTS ${VERSION_HEADER})
add_dependencies(muos-sendspin version)
target_include_directories(muos-sendspin PRIVATE ${CMAKE_BINARY_DIR}/generated)
# Info shows the repository as owner/name.
string(REGEX REPLACE "^https://github.com/" "" github_repo "${PROJECT_HOMEPAGE_URL}")
target_compile_definitions(muos-sendspin PRIVATE MUOS_SENDSPIN_GITHUB="${github_repo}")
target_compile_options(muos-sendspin PRIVATE -Wall -Wextra)
set_source_files_properties(src/player/stb_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

target_link_libraries(muos-sendspin PRIVATE SDL2::SDL2 sendspin lvgl)
if(APPLE)
    # CoreAudio: query the output latency SDL2 doesn't expose (audio_sdl.cpp).
    target_link_libraries(muos-sendspin PRIVATE "-framework CoreAudio")
endif()
if(MUOS_DEVICE)
    # The code's device/desktop choices (fullscreen, where state lives, ...) key on this; the
    # platform's own APIs (CoreAudio, Bonjour, ...) key on the OS.
    target_compile_definitions(muos-sendspin PRIVATE MUOS_DEVICE)
    # Only glibc (<= the device's 2.44) and libSDL2 (the device's) stay dynamic. libSDL2's own
    # dependencies (ALSA, ...) exist only on the device, hence --allow-shlib-undefined.
    target_link_options(muos-sendspin PRIVATE -static-libstdc++ -static-libgcc -Wl,--allow-shlib-undefined
                        -Wl,--gc-sections)
    # No rpath: libSDL2-2.0.so.0 must resolve to the device's /usr/lib copy (muOS-patched).
    set_target_properties(muos-sendspin PROPERTIES SKIP_BUILD_RPATH ON)
endif()

# Assets live next to the binary (same layout as the muOS app folder).
add_custom_command(TARGET muos-sendspin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:muos-sendspin>/assets
    COMMENT "Copying assets"
)

# ------------------------------------------------------------------------------
# Licenses of everything compiled into the binary, gathered from the pinned sources so they
# match what's built. Shipped in the package; the bundled Noto Sans license is assets/fonts/OFL.txt.
# ------------------------------------------------------------------------------
foreach(dep arduinojson ixwebsocket micro_flac micro_opus noise_c)
    FetchContent_GetProperties(${dep})
endforeach()
set(lvgl_fonts "${lvgl_SOURCE_DIR}/scripts/generators/built_in_font/font_license")
file(READ third_party/stb/stb_image.h stb_source)
string(FIND "${stb_source}" "This software is available under 2 licenses" stb_license_at)
string(SUBSTRING "${stb_source}" ${stb_license_at} -1 stb_license)
set(THIRD_PARTY_LICENSES "${CMAKE_BINARY_DIR}/THIRD_PARTY_LICENSES.txt")
file(WRITE "${THIRD_PARTY_LICENSES}" "Third-party software in muos-sendspin\n")
foreach(entry
        "sendspin-cpp|${sendspin_cpp_SOURCE_DIR}/LICENSE"
        "LVGL|${lvgl_SOURCE_DIR}/LICENCE.txt"
        "Montserrat font (LVGL built-in)|${lvgl_fonts}/Montserrat/OFL.txt"
        "Font Awesome 5 Free (assets/fonts/Symbols.ttf, and LVGL built-in)|${lvgl_fonts}/FontAwesome5/LICENSE.txt"
        "ArduinoJson|${arduinojson_SOURCE_DIR}/LICENSE.txt"
        "IXWebSocket|${ixwebsocket_SOURCE_DIR}/LICENSE.txt"
        "micro-flac|${micro_flac_SOURCE_DIR}/LICENSE"
        "micro-opus|${micro_opus_SOURCE_DIR}/LICENSE"
        "Opus|${micro_opus_SOURCE_DIR}/lib/opus/COPYING"
        "micro-ogg-demuxer|${micro_flac_SOURCE_DIR}/lib/micro-ogg-demuxer/LICENSE"
        "noise-c|${noise_c_SOURCE_DIR}/LICENSE")
    string(REPLACE "|" ";" entry "${entry}")
    list(GET entry 0 name)
    list(GET entry 1 path)
    file(READ "${path}" text)
    file(APPEND "${THIRD_PARTY_LICENSES}" "\n==== ${name} ====\n\n${text}")
endforeach()
file(APPEND "${THIRD_PARTY_LICENSES}" "\n==== stb_image ====\n\n${stb_license}")

# ------------------------------------------------------------------------------
# muOS package: dist/Sendspin/ (copy to /run/muos/storage/application/) and dist/Sendspin.zip
# ------------------------------------------------------------------------------
set(MUOS_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist/Sendspin")
add_custom_target(package-muos
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${MUOS_APP_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MUOS_APP_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/packaging/muos/Sendspin" "${MUOS_APP_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${MUOS_APP_DIR}/assets"
    # Stripped: the symbols stay in the build's own copy (same build ID), for reading a core dump.
    COMMAND ${CMAKE_STRIP} -o "${MUOS_APP_DIR}/muos-sendspin" $<TARGET_FILE:muos-sendspin>
    COMMAND ${CMAKE_COMMAND} -E copy "${THIRD_PARTY_LICENSES}" "${MUOS_APP_DIR}/THIRD_PARTY_LICENSES.txt"
    COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_CURRENT_SOURCE_DIR}/dist" ${CMAKE_COMMAND} -E tar cf Sendspin.zip --format=zip Sendspin
    DEPENDS muos-sendspin
    COMMENT "Packaging dist/Sendspin"
)
