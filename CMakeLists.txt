# mf-android — native build of Metroid Fusion (decomp) for PC/Android.
#
#   cmake -B build -G Ninja -DMF_HOST=headless   # or sdl
#   cmake --build build
#
# The GBA ROM build is still the upstream Makefile; this file only drives the
# native port. No ROM is needed to build: game data is read from the player's
# own ROM when the game starts.
cmake_minimum_required(VERSION 3.22)
project(mf_android C)

set(MF_REGION "us" CACHE STRING "Game region: us, eu")
set(MF_HOST "sdl" CACHE STRING "Frontend: sdl or headless")
option(MF_BUGFIX "Build with the decomp's BUGFIX fixes" OFF)
# The decomp is written for agbcc and is very noisy under modern compilers.
set(MF_GAME_WARNINGS "-w" CACHE STRING "Warning flags for the game code")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_POSITION_INDEPENDENT_CODE ON) # Android links everything into libmain.so
set(MF_SDL3_TAG "release-3.4.18" CACHE STRING "SDL3 version fetched when no SDL3 is installed")
set(MF_IMGUI_TAG "v1.92.9b" CACHE STRING "Dear ImGui version (options menu)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

string(TOUPPER "${MF_REGION}" MF_REGION_UPPER)

# ---------------------------------------------------------------------------
# Game code (the decomp), compiled natively.
# ---------------------------------------------------------------------------
# Files with INCBIN data (src/data/** and a few others) are never compiled with
# real ROM contents: tools/port/gen_assets.py writes a copy of each where the
# data is zero-filled and registered for loading from the player's ROM.
file(GLOB_RECURSE MF_ALL_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.c")
set(MF_GAME_SOURCES)
set(MF_DATA_SOURCES)
# src/status_screen.c is a partial C version that the GBA build doesn't link
# (it uses asm/disasm_0x0807e678.s); port/decomp/status_easy_sleep.c has the
# complete one.
list(FILTER MF_ALL_SOURCES EXCLUDE REGEX "/src/status_screen\\.c$")
foreach(src IN LISTS MF_ALL_SOURCES)
    file(STRINGS "${src}" has_incbin REGEX "INCBIN_" LIMIT_COUNT 1)
    if(src MATCHES "/src/data/" OR has_incbin)
        list(APPEND MF_DATA_SOURCES "${src}")
    else()
        list(APPEND MF_GAME_SOURCES "${src}")
    endif()
endforeach()
# C translations of the functions the decomp still has in assembly (asm/*.s).
option(MF_PORT_DECOMP "Compile port/decomp (development switch)" ON)
set(MF_PORT_DECOMP_SOURCES)
if(MF_PORT_DECOMP)
    file(GLOB_RECURSE MF_PORT_DECOMP_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/port/decomp/*.c")
endif()
file(GLOB_RECURSE MF_PORT_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/port/game/*.c")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/database.json" "${CMAKE_SOURCE_DIR}/tools/port/gen_assets.py" ${MF_DATA_SOURCES})
set(MF_GEN_DIR "${CMAKE_BINARY_DIR}/gen_assets")
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/port/gen_assets.py"
            --root "${CMAKE_SOURCE_DIR}" --out "${MF_GEN_DIR}" --region ${MF_REGION}
            --cc "${CMAKE_C_COMPILER}"
    OUTPUT_VARIABLE MF_DATA_WRAPPERS
    RESULT_VARIABLE gen_result
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT gen_result EQUAL 0)
    message(FATAL_ERROR "tools/port/gen_assets.py failed")
endif()
string(REPLACE "\n" ";" MF_DATA_WRAPPERS "${MF_DATA_WRAPPERS}")

add_library(mf_game STATIC ${MF_GAME_SOURCES}
    ${MF_PORT_DECOMP_SOURCES} ${MF_PORT_GAME_SOURCES})
# DMA macro arguments: src/ follows agbcc (see include/gba/dma.h), the code
# written for the port follows C.
set_source_files_properties(${MF_PORT_DECOMP_SOURCES} ${MF_PORT_GAME_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS PORT_DMA_C_ARITHMETIC)
target_include_directories(mf_game PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/port/decomp")
target_compile_definitions(mf_game PRIVATE PLATFORM_PC REGION_${MF_REGION_UPPER})
if(MF_BUGFIX)
    target_compile_definitions(mf_game PRIVATE BUGFIX)
endif()
# The generated data files are already preprocessed, so they get no forced
# include. Their arrays must be writable to receive the ROM contents (they are
# mostly zero-initialized, so they land in .bss and cost nothing).
add_library(mf_data STATIC ${MF_DATA_WRAPPERS})
# Data and code refer to each other (sprite AI tables, cutscene tables...):
# CMake repeats the two archives on the link line.
target_link_libraries(mf_data PUBLIC mf_game)
target_link_libraries(mf_game PUBLIC mf_data)
target_compile_definitions(mf_data PRIVATE "const=")
set(MF_GAME_FLAGS
    -std=gnu11
    -nostdinc                 # the game defines its own NULL/types
    -fno-strict-aliasing      # GBA code type-puns freely
    -fwrapv                   # ...and relies on two's-complement overflow
    -fcommon
    -funsigned-char
    ${MF_GAME_WARNINGS})
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    # A few functions `return;` without a value (callers ignore the result).
    # Implicit function declarations stay errors: see include/port/prototypes.h.
    list(APPEND MF_GAME_FLAGS -Wno-error=return-type -Werror=implicit-function-declaration
        -Werror=int-conversion -Werror=incompatible-pointer-types)
endif()
target_compile_options(mf_data PRIVATE ${MF_GAME_FLAGS})
target_compile_options(mf_game PRIVATE
    -include port/prototypes.h # no implicit declarations on 64-bit hosts
    ${MF_GAME_FLAGS})

# ---------------------------------------------------------------------------
# Port layer.
# ---------------------------------------------------------------------------
add_library(mf_port STATIC
    port/src/assets.c
    port/src/sha1.c
    port/src/gba_mem.c
    port/src/dma.c
    port/src/bios.c
    port/src/apu.c
    port/src/frame.c
    port/src/main.c
    port/src/savestate.c
    port/src/warp.c
    port/src/widescreen.c
    port/src/extras.c
    port/src/second_screen.c
    port/src/pacing.c
    port/ppu/src/virtuappu.c
    port/ppu/src/mode1.c)
target_include_directories(mf_port PUBLIC port/include PRIVATE port/ppu/include)
file(READ "${CMAKE_SOURCE_DIR}/mf_${MF_REGION}.sha1" MF_SHA1_LINE)
string(REGEX MATCH "^[0-9a-fA-F]+" MF_ROM_SHA1 "${MF_SHA1_LINE}")
string(TOLOWER "${MF_ROM_SHA1}" MF_ROM_SHA1)
target_compile_definitions(mf_port PRIVATE
    MF_ROM_SHA1="${MF_ROM_SHA1}" MF_ROM_REGION_NAME="${MF_REGION_UPPER}")
target_compile_options(mf_port PRIVATE -std=gnu11 -Wall -Wextra -Wno-unused-parameter)
# The PPU renders scanlines in parallel with OpenMP when available. Not used on
# Android (240x160 takes ~1 ms on one core, and it avoids shipping libomp).
if(NOT ANDROID)
    find_package(OpenMP COMPONENTS C)
    if(OpenMP_C_FOUND)
        target_link_libraries(mf_port PRIVATE OpenMP::OpenMP_C)
    endif()
endif()
target_link_libraries(mf_port PUBLIC m)
if(ANDROID)
    target_link_libraries(mf_port PUBLIC log)
endif()

# ---------------------------------------------------------------------------
# Executables.
# ---------------------------------------------------------------------------
if(MF_HOST STREQUAL "headless")
    add_executable(mf_headless port/src/host_headless.c)
    target_compile_options(mf_headless PRIVATE -std=gnu11 -Wall -Wextra)
    target_link_libraries(mf_headless PRIVATE mf_port mf_game mf_data mf_port)
    # Frame pacing / sound rate control test (no ROM needed).
    add_executable(mf_pacing_test tools/port/pacing_test.c port/src/pacing.c)
    target_include_directories(mf_pacing_test PRIVATE port/include)
    target_compile_options(mf_pacing_test PRIVATE -std=gnu11 -Wall -Wextra)
    target_link_libraries(mf_pacing_test PRIVATE m)
elseif(MF_HOST STREQUAL "sdl")
    if(NOT ANDROID)
        find_package(SDL3 CONFIG QUIET)
    endif()
    if(NOT TARGET SDL3::SDL3)
        # No system SDL3 (always the case on Android): build it from source.
        include(FetchContent)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG ${MF_SDL3_TAG}
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SDL3)
    endif()
    # Options menu: Dear ImGui with its SDL3 + SDL_Renderer backends.
    enable_language(CXX)
    include(FetchContent)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG ${MF_IMGUI_TAG}
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(imgui)
    add_library(mf_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(mf_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_compile_definitions(mf_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS
                                               IMGUI_DISABLE_DEBUG_TOOLS)
    target_link_libraries(mf_imgui PUBLIC SDL3::SDL3)

    set(MF_SDL_SOURCES port/src/host_sdl.c port/src/config.c port/src/menu.cpp)
    if(ANDROID)
        # SDLActivity loads libmain.so and calls SDL_main.
        add_library(main SHARED ${MF_SDL_SOURCES})
        set(MF_EXE main)
    else()
        add_executable(mf ${MF_SDL_SOURCES})
        set(MF_EXE mf)
    endif()
    target_compile_options(${MF_EXE} PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11> -Wall -Wextra)
    target_compile_features(${MF_EXE} PRIVATE cxx_std_17)
    target_link_libraries(${MF_EXE} PRIVATE mf_port mf_game mf_data mf_port mf_imgui SDL3::SDL3)
else()
    message(FATAL_ERROR "MF_HOST must be sdl or headless")
endif()
