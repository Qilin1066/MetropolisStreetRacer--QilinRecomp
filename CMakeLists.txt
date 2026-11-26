cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on; so runtime code avoids std::format,
# which Apple's libc++ only has from 13.3. The developer tools (qilin, the
# tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(MetropolisStreetRacerQilinRecomp VERSION 0.9.3 LANGUAGES C CXX)  # C: SDL3 and volk

# ---- Language: C++23 everywhere (recompiler, runtime, generated code) ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
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

# ---- Disc image, IP.BIN and boot-file loaders: runtime code too (no std::format) ----
add_library(qilin_disc STATIC recomp/src/gdrom.cpp recomp/src/bootfile.cpp recomp/src/sha256.cpp)
target_include_directories(qilin_disc PUBLIC recomp/src)
target_link_libraries(qilin_disc PRIVATE qilin_warnings)

# ---- Recompiler core ----
add_library(qilin_core STATIC
    recomp/src/report.cpp
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
    target_link_libraries(qilin_platform PRIVATE onecore)  # VirtualAlloc2, MapViewOfFile3 (Windows 10 1803+)
endif()

enable_testing()
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

# Compile the movie and sound-CPU integration in CI without requiring game-derived lifts.
# These objects are not linked into the game; its executable compiles them normally.
if(QILIN_HOST)
    add_library(msr_media_compile OBJECT msr/src/movie.cpp msr/src/sound_cpu.cpp)
    target_link_libraries(msr_media_compile PRIVATE msr_port qilin_pl_mpeg dreamcast_hw qilin_warnings)
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

    # The game: the lifted code built for speed (RAM as the fast path, the rest
    # through the bus), and the program players run (msr/src).
    add_library(qilin_gen STATIC ${QILIN_GEN_SOURCES} recomp/runtime/dispatch.cpp recomp/runtime/system.cpp
        recomp/runtime/bus.cpp)
    target_include_directories(qilin_gen PUBLIC "${QILIN_GEN_DIR}")
    target_link_libraries(qilin_gen PUBLIC qilin_runtime qilin_platform PRIVATE qilin_warnings)
    if(MSVC)
        # C4702 (unreachable code): lifted code calls guest functions that never return (they
        # stop, or loop for ever), and the code after such a call is still emitted.
        target_compile_options(qilin_gen PRIVATE /wd4702)
    endif()
    # During the bring-up: each access notes its instruction, so a stop names it (docs/runtime.md).
    option(QILIN_BRINGUP "Note the guest instruction of each access in the game build" ON)
    if(QILIN_BRINGUP)
        target_compile_definitions(qilin_gen PUBLIC QILIN_BRINGUP)
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
    add_executable(msr msr/src/main.cpp msr/src/game.cpp msr/src/sound_cpu.cpp msr/src/movie.cpp)
    target_link_libraries(msr PRIVATE msr_port dreamcast_hw qilin_pl_mpeg qilin_gen qilin_disc SDL3::SDL3-static qilin_warnings)
    if(MSVC)
        target_link_options(msr PRIVATE /STACK:8388608)  # guest recursion runs on the host stack
    endif()
endif()

# ---- Tests (no game files needed) ----
add_executable(qilin_tests recomp/tests/tests.cpp recomp/runtime/dispatch.cpp recomp/runtime/system.cpp
    recomp/runtime/bus.cpp)
target_link_libraries(qilin_tests PRIVATE qilin_core qilin_runtime qilin_platform msr_port qilin_warnings)
qilin_tool(qilin_tests)
add_test(NAME qilin_tests COMMAND qilin_tests)
