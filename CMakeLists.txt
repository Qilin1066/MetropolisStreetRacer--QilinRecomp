cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on; so runtime code avoids std::format,
# which Apple's libc++ only has from 13.3. The developer tools (qilin, the
# tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(MetropolisStreetRacerQilinRecomp VERSION 0.9.4 LANGUAGES C CXX)  # C: SDL3 and volk

# ---- Language: C++23 everywhere (recompiler, runtime, generated code) ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++ modules anywhere: without this, Ninja builds scan every C++ source
# for them with clang-scan-deps, which zig's Clang (a Windows PC without LLVM,
# as Toy Commander's CLAUDE.md describes) doesn't have.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Minimum compilers for the C++23 language and library features we use:
# GCC 14, Clang 19, Xcode 16 (AppleClang 16), Visual Studio 2022 17.10
# (std::expected, std::print, std::ranges, std::string::contains).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old: C++23 <print> needs GCC 14 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
    # Clang 18 lacks the C++20 concepts level libstdc++ requires for std::expected.
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Clang 19 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Xcode 16 or newer")
elseif(MSVC AND MSVC_VERSION LESS 1940)
    message(FATAL_ERROR "MSVC ${MSVC_VERSION} is too old: needs Visual Studio 2022 17.10 or newer")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "64-bit hosts only (see docs/roadmap-and-design-targets.md)")
endif()

option(QILIN_WERROR "Treat warnings in our own code as errors" OFF)
option(QILIN_REQUIRE_LAUNCHER_TESTS "Require the launcher's build test (CI): a native build, not a cross-compile" OFF)

# ---- Warnings for our own targets ----
add_library(qilin_warnings INTERFACE)
if(MSVC)
    target_compile_options(qilin_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    # fopen/strerror etc. are standard C++; MSVC's "use the _s variants" advice is not.
    target_compile_definitions(qilin_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(qilin_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE -Werror)
    endif()
endif()

# Developer tools on macOS: macOS 13.3 or later (std::format).
function(qilin_tool target)
    if(APPLE)
        target_compile_options(${target} PRIVATE -mmacosx-version-min=13.3)
        target_link_options(${target} PRIVATE -mmacosx-version-min=13.3)
    endif()
endfunction()

# Checked file storage (whole files through a temporary, read with their size checked), independent
# of SDL and Vulkan: the settings, the saves' history, flash memory (Toy Commander's, kept in step).
add_library(qilin_file_store STATIC host/src/file_store.cpp)
target_include_directories(qilin_file_store PUBLIC host/include)
target_link_libraries(qilin_file_store PRIVATE qilin_warnings)
add_executable(qilin_file_store_test host/tests/file_store_test.cpp)
target_include_directories(qilin_file_store_test PRIVATE recomp/runtime)
target_link_libraries(qilin_file_store_test PRIVATE qilin_file_store qilin_warnings)
add_test(NAME qilin_file_store_test COMMAND qilin_file_store_test)

# ---- libchdr (BSD-3-Clause): reading .chd disc images ----
# As Toy Commander--QilinRecomp has it: v0.3.0, fetched and pinned, built
# statically with what it bundles: LZMA (public domain), miniz (Unlicense),
# zstd's decompressor (taken under its BSD licence) and dr_flac (public
# domain). Its own CMake project is added by hand (SOURCE_SUBDIR points
# nowhere) so its test programs stay out of the build.
include(FetchContent)
FetchContent_Declare(libchdr
    URL https://github.com/rtissera/libchdr/archive/93d8c239ff0d4e8d7722985992649fce12d2463b.tar.gz
    URL_HASH SHA256=f38fb1b3e907c8927271a57ad6e3c02347eaa2d5803f910880b4b3c3021b3657
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(libchdr)
set(qilin_saved_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
set(WITH_SYSTEM_ZLIB OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_ZSTD OFF CACHE BOOL "" FORCE)
set(WITH_LZMA_ASM OFF CACHE BOOL "" FORCE)  # plain C: no assembler needed on any target
add_subdirectory(${libchdr_SOURCE_DIR} ${libchdr_BINARY_DIR} EXCLUDE_FROM_ALL SYSTEM)
set(BUILD_SHARED_LIBS ${qilin_saved_shared})
set(QILIN_CHDR_LIBRARIES chdr-static chdr-lzma miniz zstd)
foreach(library IN LISTS QILIN_CHDR_LIBRARIES)
    set_target_properties(${library} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(${library} PRIVATE /W0)  # third-party code: its warnings aren't ours
    else()
        target_compile_options(${library} PRIVATE -w)
    endif()
endforeach()

# ---- Disc image, IP.BIN and boot-file loaders: runtime code too (no std::format) ----
add_library(qilin_disc STATIC recomp/src/gdrom.cpp recomp/src/bootfile.cpp recomp/src/sha256.cpp)
target_include_directories(qilin_disc PUBLIC recomp/src recomp/runtime)  # recomp/runtime: path.hpp, command_line.hpp
target_link_libraries(qilin_disc PRIVATE chdr-static qilin_warnings)
if(WIN32)
    target_link_libraries(qilin_disc PUBLIC shell32)  # CommandLineToArgvW (command_line.hpp)
endif()

# ---- Recompiler core ----
add_library(qilin_core STATIC
    recomp/src/report.cpp
    recomp/src/identify.cpp
    recomp/src/decoder.cpp
    recomp/src/cfg.cpp
    recomp/src/lifter.cpp
    recomp/src/arm/decoder.cpp
    recomp/src/arm/lifter.cpp
)
target_include_directories(qilin_core PUBLIC recomp/src)
target_link_libraries(qilin_core PUBLIC qilin_disc PRIVATE qilin_warnings)
qilin_tool(qilin_core)

add_executable(qilin recomp/src/main.cpp)
target_link_libraries(qilin PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin)
target_compile_definitions(qilin PRIVATE QILIN_VERSION="${PROJECT_VERSION}")

# The downloads' launcher (tools/player-kit/launcher, the same files in every
# project: ADOPTING.md there): its core and tests here, its program further
# down (qilin_launcher_program, where SDL3 and Dear ImGui are known). Its
# build test runs the real build with stand-in children, and qilin's identify.
enable_testing()
if(QILIN_REQUIRE_LAUNCHER_TESTS AND CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "the launcher's build test needs a native build")
endif()
set(QILIN_LAUNCHER_QILIN qilin)
include(tools/player-kit/launcher/launcher.cmake)

# ---- What lifted code runs on: header-only, plus the dispatch table lookup ----
add_library(qilin_runtime INTERFACE)
target_include_directories(qilin_runtime INTERFACE recomp/runtime)
if(MSVC)
    target_compile_options(qilin_runtime INTERFACE /fp:precise)
else()
    # -frounding-math: FPSCR.RM changes the host's rounding mode at run time (rt::set_rounding).
    target_compile_options(qilin_runtime INTERFACE -ffp-contract=off -fno-fast-math -frounding-math)
endif()

# ---- The guest address space (recomp/runtime/memory.hpp) ----
# and fibers (recomp/runtime/fiber.hpp): the platform layer, independent of guest.hpp.
add_library(qilin_platform STATIC recomp/runtime/memory.cpp recomp/runtime/fiber.cpp)
target_include_directories(qilin_platform PUBLIC recomp/runtime)
target_link_libraries(qilin_platform PRIVATE qilin_warnings)
if(WIN32)
    # VirtualAlloc2, MapViewOfFile3, UnmapViewOfFile2 (Windows 10 1803+) are
    # found at run time (memory.cpp): no import library. MinGW's headers
    # declare them from Windows 10 1803 (RS4) only, which newer releases
    # (llvm-mingw's) don't assume by default.
    if(NOT MSVC)
        target_compile_definitions(qilin_platform PRIVATE NTDDI_VERSION=0x0A000005)
    endif()
endif()

find_package(Threads REQUIRED)
add_executable(dreamcast_clock_test dreamcast/tests/clock_test.cpp dreamcast/src/clock.cpp)
target_include_directories(dreamcast_clock_test PRIVATE dreamcast/src)
target_link_libraries(dreamcast_clock_test PRIVATE qilin_warnings Threads::Threads)
add_test(NAME dreamcast_clock_test COMMAND dreamcast_clock_test)
set_tests_properties(dreamcast_clock_test PROPERTIES TIMEOUT 20)

# ---- The host platform: window, input and audio (SDL3), Vulkan ----
# The sister projects' host layer (host/, from Toy Commander--QilinRecomp,
# which has it from PunchOutWii--QilinRecomp), kept in step. Fetched at
# configure time, pinned, built statically: SDL3, the Vulkan headers and volk
# (which loads Vulkan at run time, so no SDK is needed to build or run).
option(QILIN_HOST "Build the host platform layer (window, Vulkan, input, audio)" ON)
if(QILIN_HOST)
    include(FetchContent)
    set(FETCHCONTENT_QUIET ON)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)  # synthetic input: not needed
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL GIT_TAG release-3.4.20 GIT_SHALLOW ON)
    FetchContent_Declare(VulkanHeaders GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers
                         GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW ON)
    # volk is one C file: fetched without its own CMake project (SOURCE_SUBDIR points nowhere).
    FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW ON
                         SOURCE_SUBDIR no-cmake)
    FetchContent_MakeAvailable(SDL3 VulkanHeaders volk)
    # SDL's Wayland protocol headers, made from its own XML, before the
    # system's (as Toy Commander's): SDL adds them as a system folder, and
    # zig's compiler (the Linux download's) searches the system's
    # /usr/include first, whose older wayland-client-protocol.h lacks what SDL
    # uses (WL_KEYBOARD_KEY_STATE_REPEATED). SDL includes them in quotes, and
    # -iquote folders come before every -I and -isystem one.
    if(TARGET SDL3-static AND EXISTS "${sdl3_BINARY_DIR}/wayland-generated-protocols" AND NOT MSVC)
        target_compile_options(SDL3-static BEFORE PRIVATE "-iquote${sdl3_BINARY_DIR}/wayland-generated-protocols")
    endif()
    add_library(qilin_volk STATIC ${volk_SOURCE_DIR}/volk.c)
    target_include_directories(qilin_volk PUBLIC ${volk_SOURCE_DIR})
    target_link_libraries(qilin_volk PUBLIC Vulkan::Headers ${CMAKE_DL_LIBS})
    target_compile_definitions(qilin_volk PUBLIC VK_NO_PROTOTYPES)

    # Dear ImGui (MIT) for the settings menu, as the other Qilin recomps draw
    # theirs: the same commit (v1.93.0), its core and its SDL3 and Vulkan
    # backends, fetched and pinned. Its Vulkan backend loads its functions
    # through volk (no prototypes). Where a proxy refuses the archive, clone it
    # at that commit and configure with -DFETCHCONTENT_SOURCE_DIR_IMGUI=<clone>.
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/c1aaac65e896a45f81f47a4d68f9959e0f86c859.tar.gz
        URL_HASH SHA256=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114)
    FetchContent_MakeAvailable(imgui)
    add_library(qilin_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
    target_include_directories(qilin_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_compile_definitions(qilin_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES VK_NO_PROTOTYPES IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(qilin_imgui PUBLIC SDL3::SDL3-static Vulkan::Headers)
    if(MSVC)
        target_compile_options(qilin_imgui PRIVATE /W0)  # third-party code: its warnings aren't ours
    else()
        target_compile_options(qilin_imgui PRIVATE -w)
    endif()

    # pl_mpeg (MIT, one header) decodes the intro movie's MPEG-1 video and
    # separates its streams (msr/src/movie.cpp); pinned to a commit.
    FetchContent_Declare(pl_mpeg GIT_REPOSITORY https://github.com/phoboslab/pl_mpeg
                         GIT_TAG c871f2be022ece7ef4f64230b4fb8e1fb9eb6023 SOURCE_SUBDIR no-cmake)
    FetchContent_MakeAvailable(pl_mpeg)
    add_library(qilin_pl_mpeg STATIC msr/src/pl_mpeg.c)
    target_include_directories(qilin_pl_mpeg PUBLIC ${pl_mpeg_SOURCE_DIR})
    if(MSVC)
        target_compile_options(qilin_pl_mpeg PRIVATE /W0)  # third-party code: its warnings aren't ours
    else()
        target_compile_options(qilin_pl_mpeg PRIVATE -w)
    endif()

    # The host layer and the settings menu: the game's name in messages, the
    # window's title, the data folder (qilin/msr) and the settings file (msr.ini).
    # SMAA 1x and AMD FidelityFX FSR 1 for the picture (host/src/smaa, host/src/fsr): the upstream
    # files the port builds with, downloaded once at their pinned commits and checked by hash
    # (THIRD_PARTY.md). Their shaders are compiled once to SPIR-V and committed (tools/*_spirv.sh).
    set(QILIN_THIRD_PARTY ${CMAKE_BINARY_DIR}/third_party)
    function(qilin_fetch_file url file sha256)
        if(NOT EXISTS ${QILIN_THIRD_PARTY}/${file})
            file(DOWNLOAD ${url} ${QILIN_THIRD_PARTY}/${file} EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS status)
            list(GET status 0 code)
            if(NOT code EQUAL 0)
                message(FATAL_ERROR "can't download ${url}: ${status}")
            endif()
        endif()
        file(SHA256 ${QILIN_THIRD_PARTY}/${file} got)
        if(NOT got STREQUAL sha256)
            message(FATAL_ERROR "${QILIN_THIRD_PARTY}/${file} isn't the pinned file (SHA-256 ${got}); delete it to download it again")
        endif()
    endfunction()
    set(fsr https://raw.githubusercontent.com/GPUOpen-Effects/FidelityFX-FSR/a21ffb8f6c13233ba336352bdff293894c706575)
    set(smaa https://raw.githubusercontent.com/iryoku/smaa/71c806a838bdd7d517df19192a20f0c61b3ca29d)
    qilin_fetch_file(${fsr}/ffx-fsr/ffx_a.h fsr1/ffx_a.h f60e2722fcd13989523b9164d776ab382b3692791767f3bf8bb19967f763f3fb)
    qilin_fetch_file(${fsr}/ffx-fsr/ffx_fsr1.h fsr1/ffx_fsr1.h 93c3922362ea7fc99cbcc698ca30c98de4f8c246d1fbb0b09e015ddef38ce3a5)
    qilin_fetch_file(${fsr}/license.txt fsr1/LICENSE.txt db089274ce766da70f5b7d791029c3486f9f9e27c8c79c652689603d3192e802)
    qilin_fetch_file(${smaa}/Textures/AreaTex.h smaa/AreaTex.h 1933bf43ce86ba71f7ed5f0fa94b148610c634dc3395b9bc67a817d51ee73af2)
    qilin_fetch_file(${smaa}/Textures/SearchTex.h smaa/SearchTex.h 1fac315ab5f87b60083c9b8387af17f056204d3d1bf21500f2acc5b3e8c1a37a)
    qilin_fetch_file(${smaa}/SMAA.hlsl smaa/SMAA.hlsl 4db53d92ef0b45661f8450303589ffabce93c352cae4282a6f32dd2b80efbeb9)
    qilin_fetch_file(${smaa}/LICENSE.txt smaa/LICENSE.txt 2b848b67ba50ac40b53e11cf1ccdaee2a190ca83b4652cba695a5f1ce81f52ae)

    add_library(qilin_host STATIC host/src/vulkan.cpp host/src/display.cpp host/src/audio.cpp host/src/paths.cpp host/src/image.cpp
        host/src/fsr/fsr_constants.cpp)
    target_include_directories(qilin_host PUBLIC host/include PRIVATE host/src ${QILIN_THIRD_PARTY}/fsr1 ${QILIN_THIRD_PARTY}/smaa)
    target_compile_definitions(qilin_host PUBLIC QILIN_APP="msr" QILIN_APP_TITLE="Metropolis Street Racer")
    target_link_libraries(qilin_host PUBLIC qilin_volk PRIVATE SDL3::SDL3-static qilin_warnings)
    add_library(dreamcast_menu STATIC dreamcast/src/settings.cpp dreamcast/src/menu.cpp dreamcast/src/save_history.cpp)
    target_include_directories(dreamcast_menu PUBLIC dreamcast/src recomp/runtime)
    target_link_libraries(dreamcast_menu PUBLIC qilin_host qilin_imgui PRIVATE SDL3::SDL3-static qilin_warnings qilin_file_store)
    target_compile_definitions(dreamcast_menu PRIVATE QILIN_VERSION="${PROJECT_VERSION}")
    # The Dreamcast's hardware as the port provides it (Toy Commander's dreamcast/ layer, kept in
    # step): the PowerVR2 and system bus registers, video memory, the tile accelerator, the
    # renderer, the SH-4's timer and DMA units, guest time, interrupts, the Maple bus, the
    # sound chip's channels and flash memory. Built without the game, so CI compiles it.
    add_library(dreamcast_hw STATIC dreamcast/src/pvr.cpp dreamcast/src/texture.cpp dreamcast/src/renderer.cpp
        dreamcast/src/video.cpp dreamcast/src/maple.cpp dreamcast/src/sh4.cpp dreamcast/src/clock.cpp dreamcast/src/ta.cpp
        dreamcast/src/interrupts.cpp dreamcast/src/sound.cpp dreamcast/src/flash.cpp dreamcast/src/aica.cpp)
    target_include_directories(dreamcast_hw PUBLIC dreamcast/src)
    target_link_libraries(dreamcast_hw PUBLIC qilin_runtime dreamcast_menu qilin_disc qilin_file_store PRIVATE SDL3::SDL3-static qilin_warnings)
    target_compile_definitions(dreamcast_hw PRIVATE QILIN_VERSION="${PROJECT_VERSION}")
    # Vulkan structures are written {sType} with the rest zeroed: that's the idiom, not a mistake.
    if(NOT MSVC)
        set_source_files_properties(host/src/vulkan.cpp host/src/display.cpp host/src/audio.cpp host/src/paths.cpp host/tests/vktest.cpp
                                    dreamcast/src/renderer.cpp dreamcast/src/menu.cpp dreamcast/src/video.cpp dreamcast/tests/render_test.cpp
                                    PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()

    # The Vulkan layer's own test: a device, an offscreen frame, read back. Skips
    # (exit 77) where there's no Vulkan 1.3 device.
    add_executable(qilin_vktest host/tests/vktest.cpp)
    target_link_libraries(qilin_vktest PRIVATE qilin_host qilin_warnings)
    add_test(NAME qilin_vktest COMMAND qilin_vktest)
    set_tests_properties(qilin_vktest PROPERTIES SKIP_RETURN_CODE 77)
    # Controllers reach the game (SDL's virtual joysticks).
    add_executable(qilin_padtest host/tests/padtest.cpp)
    target_link_libraries(qilin_padtest PRIVATE qilin_host SDL3::SDL3-static qilin_warnings)
    add_test(NAME qilin_padtest COMMAND qilin_padtest)
    set_tests_properties(qilin_padtest PROPERTIES SKIP_RETURN_CODE 77)
    # The texture decoder and the renderer, without the game.
    add_executable(dreamcast_render_test dreamcast/tests/render_test.cpp dreamcast/src/texture.cpp dreamcast/src/renderer.cpp)
    target_include_directories(dreamcast_render_test PRIVATE dreamcast/src recomp/runtime)
    target_link_libraries(dreamcast_render_test PRIVATE qilin_host qilin_warnings)
    add_test(NAME dreamcast_render_test COMMAND dreamcast_render_test)
    set_tests_properties(dreamcast_render_test PROPERTIES SKIP_RETURN_CODE 77)
    # The settings file's reader and writer, and the saves' history.
    add_executable(dreamcast_settings_test dreamcast/tests/settings_test.cpp dreamcast/src/settings.cpp)
    target_include_directories(dreamcast_settings_test PRIVATE dreamcast/src recomp/runtime host/include)
    target_link_libraries(dreamcast_settings_test PRIVATE qilin_warnings qilin_file_store)
    target_compile_definitions(dreamcast_settings_test PRIVATE QILIN_APP="msr" QILIN_APP_TITLE="Metropolis Street Racer")
    add_test(NAME dreamcast_settings_test COMMAND dreamcast_settings_test)
    add_executable(dreamcast_save_history_test dreamcast/tests/save_history_test.cpp dreamcast/src/save_history.cpp)
    target_include_directories(dreamcast_save_history_test PRIVATE dreamcast/src recomp/runtime)
    target_link_libraries(dreamcast_save_history_test PRIVATE qilin_warnings qilin_file_store)
    add_test(NAME dreamcast_save_history_test COMMAND dreamcast_save_history_test)
    add_executable(qilin_frame_times_test host/tests/frame_times_test.cpp)
    target_include_directories(qilin_frame_times_test PRIVATE host/include)
    target_link_libraries(qilin_frame_times_test PRIVATE qilin_warnings)
    add_test(NAME qilin_frame_times_test COMMAND qilin_frame_times_test)
    # The sound queue under SDL's real callbacks (its dummy device).
    add_executable(qilin_audio_callback_test host/tests/audio_callback_test.cpp)
    target_link_libraries(qilin_audio_callback_test PRIVATE qilin_host SDL3::SDL3-static qilin_warnings)
    add_test(NAME qilin_audio_callback_test COMMAND qilin_audio_callback_test)
    set_tests_properties(qilin_audio_callback_test PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy;SDL_AUDIO_DEVICE_SAMPLE_FRAMES=128" TIMEOUT 25)

    # The downloads' launcher, `Play Metropolis Street Racer` (Windows: .exe;
    # macOS: the app's program), which reads kit/game.txt
    # (tools/player-kit/launcher/launcher.cmake).
    qilin_launcher_program(IMGUI_DIR ${imgui_SOURCE_DIR} LIBRARIES qilin_imgui qilin_volk SDL3::SDL3-static Threads::Threads)
    if(WIN32)
        # The launcher's icon for the game too, which the player's PC links
        # (the player kit's icon.o; QILIN_WINDOWS_RESOURCES is launcher.cmake's).
        if(QILIN_WINDOWS_RESOURCES)
            add_library(qilin_game_icon OBJECT tools/player-kit/game_icon.rc)
        elseif(QILIN_PLAYER_KIT)
            message(FATAL_ERROR "the player kit needs QILIN_WINDOWS_RESOURCES=ON (the game's icon)")
        endif()
    endif()
endif()

# The macOS download's parts (tools/player-kit/macos.cmake): the universal kit.
if(APPLE)
    include(tools/player-kit/macos.cmake)
endif()

# ---- The port: the hardware and system software the game runs on (msr/src),
# built without the lifted code too, so CI compiles it ----
add_library(msr_port STATIC msr/src/onchip.cpp msr/src/bios.cpp msr/src/rtc.cpp msr/src/drive.cpp msr/src/launcher.cpp msr/src/adx.cpp msr/src/romfont.cpp msr/src/console.cpp msr/src/frame_rate.cpp)
target_include_directories(msr_port PUBLIC msr/src)
target_link_libraries(msr_port PUBLIC qilin_runtime qilin_disc PRIVATE qilin_warnings)
if(QILIN_HOST)
    target_link_libraries(msr_port PUBLIC dreamcast_hw)  # flash memory (the boot ROM's FLASHROM calls)
else()
    target_include_directories(msr_port PRIVATE dreamcast/src)
endif()

# ---- The game's program without the lifted code (msr_game) ----
# The program players run (msr/src/main.cpp and the game's side), the runtime
# lifted code calls (dispatch, system, bus) and everything they link: built
# whether or not lifted code is here, and linked with stand-ins for it
# (msr_link_check, tools/player-kit/link_check.cpp), so CI checks the whole
# program with every compiler. The game is msr_game plus the lifted code
# (QILIN_GEN_DIR below), and the player kit is msr_game in one archive.
# During the bring-up: each access notes its instruction, so a stop names it
# (docs/runtime.md). The lifted code is compiled with the same definitions
# (msr_game's, and the player kit's build-flags.txt).
option(QILIN_BRINGUP "Note the guest instruction of each access in the game build" ON)
option(QILIN_PLAYER_KIT "Build the player kit (tools/player-kit): the game's program without the lifted code as one archive, for the downloads" OFF)

# ---- The player kit: the downloads (tools/player-kit, as Toy Commander's) ----
# Everything but the lifted code in one archive (libmsr_kit.a): msr_game (as msr_game_archive) and
# all it links, SDL3 included. Beside it the runtime's headers, the recompiler,
# and build-flags.txt: how the player's computer compiles and links the code it
# lifts from its own disc (tools/player-kit/launcher/build.cpp reads it).
# Windows: cross-compiled on Linux with llvm-mingw (tools/player-kit/package.sh).
# Linux: built with the zig the download bundles, so the library's libc++ is
# the one the player's compiles link with (package-linux.sh). macOS:
# universal, merged by Apple's libtool (tools/player-kit/macos.cmake).
# The lifted code's flags: what qilin_runtime and msr_game give it here, so
# both sides of the link agree (QILIN_BRINGUP changes guest.hpp's accesses).
set(QILIN_KIT_LIBRARIES msr_game_archive msr_port dreamcast_hw dreamcast_menu qilin_host qilin_imgui qilin_volk qilin_pl_mpeg
                        qilin_disc ${QILIN_CHDR_LIBRARIES} qilin_platform qilin_file_store SDL3-static)
set(QILIN_KIT_CXXFLAGS -ffp-contract=off -fno-fast-math -frounding-math)
if(QILIN_BRINGUP)
    list(APPEND QILIN_KIT_CXXFLAGS -DQILIN_BRINGUP)
endif()
function(qilin_player_kit)
    if(APPLE)
        qilin_player_kit_macos()  # tools/player-kit/macos.cmake
        return()
    endif()
    if(NOT TARGET SDL3-static)
        message(FATAL_ERROR "the player kit needs SDL3 built statically (QILIN_HOST=ON)")
    endif()
    if(QILIN_WATCH)
        message(FATAL_ERROR "the player kit is a release build: configure it with -DQILIN_WATCH=OFF")
    endif()
    set(kit_archive "${CMAKE_BINARY_DIR}/kit/libmsr_kit.a")
    set(mri "CREATE ${kit_archive}\n")
    foreach(library IN LISTS QILIN_KIT_LIBRARIES)
        string(APPEND mri "ADDLIB $<TARGET_FILE:${library}>\n")
    endforeach()
    string(APPEND mri "SAVE\nEND\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/kit/kit.mri" CONTENT "${mri}")
    add_custom_command(OUTPUT "${kit_archive}"
        COMMAND "${CMAKE_COMMAND}" "-DAR=${CMAKE_AR}" "-DMRI=${CMAKE_BINARY_DIR}/kit/kit.mri" "-DOUT=${kit_archive}"
                -P "${CMAKE_SOURCE_DIR}/tools/player-kit/merge.cmake"
        DEPENDS ${QILIN_KIT_LIBRARIES} "${CMAKE_BINARY_DIR}/kit/kit.mri"
        COMMENT "Merging the player kit" VERBATIM)
    add_custom_target(msr_kit ALL DEPENDS "${kit_archive}")
    string(JOIN " " lifted_text ${QILIN_KIT_CXXFLAGS})
    if(WIN32)
        # The system libraries SDL3 and the runtime need (MinGW names).
        set(kit_system_libraries -lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32
            -lsetupapi -lshell32 -ldinput8 -lwinspool -lcomdlg32)
        set(kit_ldflags "-static -O2 -Wl,--stack,8388608")
        set(kit_icon " $kit/lib/icon.o")
        set(kit_native "")  # the launcher's default, -march=native
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # SDL3 loads X11, Wayland, the sound servers and Vulkan at run time:
        # the game links only glibc's own libraries (and zig's libc++).
        set(kit_system_libraries -lm -ldl -lpthread -lrt)
        set(kit_ldflags "-O2")
        set(kit_icon "")
        # For the player's processor: x86-64's -march, ARM's -mcpu.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
            set(kit_native "native = -mcpu=native\n")
        else()
            set(kit_native "native = -march=native\n")
        endif()
    else()
        message(FATAL_ERROR "no player kit for ${CMAKE_SYSTEM_NAME} yet")
    endif()
    string(JOIN " " kit_system_text ${kit_system_libraries})
    # Read by the launcher (tools/player-kit/launcher/build.cpp): `name = value`
    # lines, $kit the kit's folder.
    file(WRITE "${CMAKE_BINARY_DIR}/kit/build-flags.txt"
        "# Written by CMake for the player kit (QILIN_PLAYER_KIT).\n"
        "cxxflags = -O2 -DNDEBUG -std=c++23 -w ${lifted_text} -I$kit/include\n"
        "${kit_native}"
        "ldflags = ${kit_ldflags}\n"
        "libs = $kit/lib/libmsr_kit.a${kit_icon} ${kit_system_text}\n")
    # The kit's archive linked with stand-ins for the lifted code: it's complete.
    add_executable(msr_kit_link_check tools/player-kit/link_check.cpp)
    target_include_directories(msr_kit_link_check PRIVATE recomp/runtime)
    target_compile_options(msr_kit_link_check PRIVATE ${QILIN_KIT_CXXFLAGS})
    target_link_libraries(msr_kit_link_check PRIVATE "${kit_archive}" ${kit_system_libraries})
    separate_arguments(kit_ldflags_list UNIX_COMMAND "${kit_ldflags}")  # as the launcher splits them
    target_link_options(msr_kit_link_check PRIVATE ${kit_ldflags_list})
    add_dependencies(msr_kit_link_check msr_kit)
    install(TARGETS qilin RUNTIME DESTINATION kit COMPONENT msr)
    install(FILES "${kit_archive}" DESTINATION kit/lib COMPONENT msr)
    if(WIN32)
        # The game's icon (the launcher's), linked into it on the player's PC.
        add_dependencies(msr_kit qilin_game_icon)
        install(FILES $<TARGET_OBJECTS:qilin_game_icon> DESTINATION kit/lib RENAME icon.o COMPONENT msr)
    endif()
    install(FILES "${CMAKE_BINARY_DIR}/kit/build-flags.txt" DESTINATION kit COMPONENT msr)
    file(GLOB kit_headers recomp/runtime/*.hpp)
    install(FILES ${kit_headers} DESTINATION kit/include COMPONENT msr)
endfunction()

if(QILIN_HOST)
    find_package(Threads REQUIRED)
    # An object library: the programs that link it get its objects, so the
    # layers it calls and that call back into it (dreamcast_hw's calls into
    # game.cpp) link in any order. msr_game_archive is the same objects as an
    # archive, for the player kit's.
    add_library(msr_game OBJECT msr/src/main.cpp msr/src/game.cpp msr/src/sound_cpu.cpp msr/src/movie.cpp
        recomp/runtime/dispatch.cpp recomp/runtime/system.cpp recomp/runtime/bus.cpp)
    target_link_libraries(msr_game PUBLIC msr_port dreamcast_hw qilin_pl_mpeg qilin_disc qilin_runtime qilin_platform
                                          SDL3::SDL3-static Threads::Threads PRIVATE qilin_warnings)
    if(QILIN_BRINGUP)
        target_compile_definitions(msr_game PUBLIC QILIN_BRINGUP)
    endif()
    if(MSVC)
        target_link_options(msr_game INTERFACE /STACK:8388608)  # guest recursion runs on the host stack
    elseif(WIN32)
        target_link_options(msr_game INTERFACE -Wl,--stack,8388608)
    endif()
    add_library(msr_game_archive STATIC $<TARGET_OBJECTS:msr_game>)
    add_executable(msr_link_check tools/player-kit/link_check.cpp)
    target_link_libraries(msr_link_check PRIVATE msr_game)
    if(QILIN_PLAYER_KIT)
        qilin_player_kit()
    endif()
elseif(QILIN_PLAYER_KIT)
    message(FATAL_ERROR "the player kit needs the host layer (QILIN_HOST)")
endif()

# ---- Lifted code (optional): the output of `qilin lift`, which is derived
# from the game and so lives outside the repository ----
#   qilin lift disc/MSR.BIN --out gen && cmake -B build -DQILIN_GEN_DIR=$PWD/gen
# qilin_difftest runs it against the original code (tools/difftest), with
# memory accesses checked.
set(QILIN_GEN_DIR "" CACHE PATH "qilin lift output directory")
if(QILIN_GEN_DIR)
    file(GLOB QILIN_GEN_SOURCES CONFIGURE_DEPENDS "${QILIN_GEN_DIR}/*.cpp")
    add_library(qilin_gen_checked STATIC ${QILIN_GEN_SOURCES} recomp/runtime/dispatch.cpp recomp/runtime/system.cpp)
    target_include_directories(qilin_gen_checked PUBLIC "${QILIN_GEN_DIR}")
    # Checked memory; and QEMU's two differences from the hardware, so the rest
    # can be compared with it (recomp/runtime/guest.hpp). Never in a game build.
    target_compile_definitions(qilin_gen_checked PUBLIC QILIN_CHECKED_MEMORY QILIN_REFERENCE_QEMU)
    target_link_libraries(qilin_gen_checked PUBLIC qilin_runtime qilin_platform PRIVATE qilin_warnings)
    add_executable(qilin_difftest tools/difftest/harness.cpp)
    target_link_libraries(qilin_difftest PRIVATE qilin_gen_checked qilin_disc qilin_warnings)
    qilin_tool(qilin_difftest)
    if(MSVC)
        # The checked runtime stops guest recursion at 4 MiB of host stack; Windows gives 1 MiB by default.
        target_link_options(qilin_difftest PRIVATE /STACK:16777216)
    endif()

    # Development: QILIN_WATCH=<from>-<to> logs the instructions storing into that range (guest.hpp).
    option(QILIN_WATCH "Let QILIN_WATCH log stores into a range of guest memory (slower; development only)" OFF)
    if(QILIN_WATCH)
        if(NOT QILIN_BRINGUP)
            message(FATAL_ERROR "QILIN_WATCH needs QILIN_BRINGUP (the instruction of each access)")
        endif()
        target_compile_definitions(qilin_runtime INTERFACE QILIN_WATCH QILIN_BRINGUP)
    endif()
    if(NOT QILIN_HOST)
        message(FATAL_ERROR "msr needs the host layer (QILIN_HOST)")
    endif()
    # The sound CPU's driver, lifted too (qilin armlift: docs/sound.md).
    if(NOT EXISTS "${QILIN_GEN_DIR}/arm_table.cpp")
        message(FATAL_ERROR "${QILIN_GEN_DIR} has no lifted sound driver: run qilin armlift <disc>/SOUND/MANATEE.DRV --out ${QILIN_GEN_DIR}")
    endif()
    # The game: the lifted code built for speed (RAM as the fast path, the rest
    # through the bus), linked with the program players run (msr_game).
    add_executable(msr ${QILIN_GEN_SOURCES})
    target_include_directories(msr PRIVATE "${QILIN_GEN_DIR}")
    target_link_libraries(msr PRIVATE msr_game qilin_warnings)
    if(MSVC)
        # C4702 (unreachable code): lifted code calls guest functions that never return (they
        # stop, or loop for ever), and the code after such a call is still emitted.
        target_compile_options(msr PRIVATE /wd4702)
    endif()
endif()

# ---- Tests (no game files needed) ----
add_executable(qilin_tests recomp/tests/tests.cpp dreamcast/src/clock.cpp recomp/runtime/dispatch.cpp recomp/runtime/system.cpp
    recomp/runtime/bus.cpp)
target_link_libraries(qilin_tests PRIVATE qilin_core qilin_runtime qilin_platform msr_port qilin_warnings)
target_include_directories(qilin_tests PRIVATE dreamcast/src)
target_compile_definitions(qilin_tests PRIVATE QILIN_TEST_FRAME_RATE)
qilin_tool(qilin_tests)
add_test(NAME qilin_tests COMMAND qilin_tests)
