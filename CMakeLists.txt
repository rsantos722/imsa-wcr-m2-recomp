cmake_minimum_required(VERSION 3.24)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" IMSA_VERSION LIMIT_COUNT 1)

project(imsa-wcr-m2-recomp
    VERSION "${IMSA_VERSION}"
    DESCRIPTION "Recompilation of IMSA presents World Championship Racing (Panasonic M2)"
    LANGUAGES C CXX)

# Report the released version in trace/build metadata. The diagnostics reader
# prefers a VERSION file when running from a source tree, but installed builds
# have no file to read.
add_compile_definitions(IMSA_VERSION="${PROJECT_VERSION}")

# The guest runtime uses POSIX threads and GNU C features throughout. MSVC
# (including clang-cl) cannot compile it, so reject the toolchain here instead
# of failing later in a confusing compile or link error.
if(MSVC)
    message(FATAL_ERROR
        "MSVC is not supported: the guest runtime requires POSIX threads "
        "(pthreads) and GNU C language features that cl.exe does not provide.\n"
        "On Windows, build from an MSYS2 UCRT64 shell with MinGW-w64 GCC or Clang\n"
        "(use mingw-w64-ucrt-x86_64-clang instead of -gcc for Clang):\n"
        "  pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake\n"
        "      mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-sdl3\n"
        "  cmake -S . -B build -G Ninja\n"
        "  cmake --build build --parallel\n"
        "See BUILDING.md for the complete dependency list and build options.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The runtime opts into GNU C explicitly (`-std=gnu11`); C++ stays portable.
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The guest's arithmetic is a fixed point of comparison between runs and
# against a reference build. Contraction and fast-math reassociation change
# results across compilers, so they are disabled for every target rather than
# per directory. Launcher previews rasterize the same guest data and inherit
# the same rule.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-ffp-contract=off -fno-fast-math)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Release default; Debug for stepping)" FORCE)
endif()

# Game data. Without either of these, only the host (with the stub guest)
# and the unit tests are built; this is the configuration public CI uses.
set(IMSA_DISC "" CACHE FILEPATH "Path to the user's disc image (ISO)")
set(IMSA_DATA_DIR "" CACHE PATH "Path to an extracted game data folder")

option(IMSA_BUILD_TESTS "Build the unit tests" ON)
option(IMSA_WITH_FFMPEG "Enable the FFmpeg-backed MPEG decoder" ON)
set(IMSA_FFMPEG_ROOT "" CACHE PATH
    "Use this FFmpeg install (include/ and lib/) instead of searching; release packages use the LGPL build from packaging/ffmpeg")

include(CTest)

if(APPLE)
    # Homebrew prefixes: /opt/homebrew (Apple Silicon), /usr/local (Intel).
    list(PREPEND CMAKE_PREFIX_PATH "/opt/homebrew" "/usr/local")
endif()
# host/graphics/texture_replacements.h loads replacement PNGs with
# SDL_LoadPNG, which SDL documents as available since 3.4.0. Reject older
# SDL3 packages at configure time instead of failing later in the compiler.
find_package(SDL3 3.4.0 REQUIRED CONFIG)

# OpenGL detection
option(IMSA_WITH_GL "Enable OpenGL cel-blitter backend" ON)
if(IMSA_WITH_GL)
    if(APPLE)
        find_library(M2_OPENGL_FRAMEWORK OpenGL REQUIRED)
        set(OPENGL_LIBRARIES ${M2_OPENGL_FRAMEWORK})
        set(IMSA_HAVE_GL TRUE)
    else()
        # OpenGL::GL is the portable target: opengl32 on Windows, and either
        # the legacy libGL or GLVND on Unix. OpenGL::OpenGL only exists on
        # GLVND systems, so using it breaks Windows generation.
        find_package(OpenGL QUIET)
        if(TARGET OpenGL::GL)
            set(OPENGL_LIBRARIES OpenGL::GL)
            set(IMSA_HAVE_GL TRUE)
        else()
            message(STATUS "OpenGL not found: building without the OpenGL backend")
        endif()
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(ImsaGameData)
include(ImsaGenerate)

imsa_resolve_game_data()
if(IMSA_GAME_ELF)
    set(IMSA_WITH_GAME ON)
    imsa_generate_guest()
    message(STATUS "Guest code: ${IMSA_GENERATED_DIR}")
else()
    set(IMSA_WITH_GAME OFF)
    message(STATUS "No game data given: building without the guest")
endif()

add_subdirectory(runtime)
add_subdirectory(host)
add_subdirectory(launcher)

if(IMSA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Install rules and CPack. Must follow the targets it installs.
include(ImsaPackaging)
