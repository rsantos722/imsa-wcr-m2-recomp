# host/CMakeLists.txt — native host core and platform substrate

# Host platform layer (OS paths, UTF-8 conversion, timers, saves)
add_library(m2_host_platform STATIC
    core/platform.cpp
)
target_include_directories(m2_host_platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)
if(WIN32)
    target_link_libraries(m2_host_platform PUBLIC shell32 winmm)
elseif(APPLE)
    target_link_libraries(m2_host_platform PUBLIC "-framework AppKit")
endif()

# Host diagnostics layer (NDJSON tracing, performance profiling, input
# recorder, control socket). Always built; each stays off at runtime unless
# requested.
add_library(m2_host_diagnostics STATIC
    diagnostics/trace.cpp
    diagnostics/diagnostic_perf.cpp
    diagnostics/input_recorder.cpp
    diagnostics/diagnostic_control.cpp
)
target_include_directories(m2_host_diagnostics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/shims
)
target_link_libraries(m2_host_diagnostics PUBLIC
    m2_host_platform
    m2_runtime
)
if(IMSA_WITH_GAME)
    target_compile_definitions(m2_host_diagnostics PRIVATE M2_HAVE_RECOMP=1)
endif()

# Host core layer (folio dispatch, kernel/fs shims, savestates, event queue, VBL pacer)
add_library(m2_host_core STATIC
    core/config.cpp
    core/vbl_pacer.cpp
    core/event_broker.cpp
    core/folio_loader.cpp
    core/selector_probe.cpp
    core/kernel_shim.cpp
    core/small_folio_shims.cpp
    core/filesystem_shim.cpp
    core/savestate.cpp
)
target_include_directories(m2_host_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/shims
)
target_link_libraries(m2_host_core PUBLIC
    m2_host_diagnostics
    m2_host_platform
    m2_runtime
    SDL3::SDL3
)

# Selector name tables for the folio loader. The imported selectors and call
# counts come from the user's guest images; without game data every named
# selector is listed with no counts.
set(M2_SELECTOR_NAMES_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(M2_SELECTOR_NAMES_HEADER "${M2_SELECTOR_NAMES_DIR}/selector_names.gen.h")
set(M2_SELECTOR_NAMES_ARGS --out "${M2_SELECTOR_NAMES_HEADER}")
set(M2_SELECTOR_NAMES_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/gen_selector_names.py"
    "${CMAKE_SOURCE_DIR}/shims/abi/selector-names.json")
if(IMSA_WITH_GAME)
    set(M2_SELECTOR_NAMES_PYTHON "${IMSA_RECOMPILER_PYTHON}")
    list(APPEND M2_SELECTOR_NAMES_ARGS --game-data "${IMSA_GAME_DIR}")
    list(APPEND M2_SELECTOR_NAMES_DEPENDS
        "${CMAKE_SOURCE_DIR}/recompiler/transpile.py"
        "${IMSA_GAME_DIR}/driving/game"
        "${IMSA_GAME_DIR}/shooter2d/shooter2d")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(M2_SELECTOR_NAMES_PYTHON "${Python3_EXECUTABLE}")
endif()
add_custom_command(
    OUTPUT "${M2_SELECTOR_NAMES_HEADER}"
    COMMAND "${M2_SELECTOR_NAMES_PYTHON}"
            "${CMAKE_SOURCE_DIR}/tools/gen_selector_names.py"
            ${M2_SELECTOR_NAMES_ARGS}
    DEPENDS ${M2_SELECTOR_NAMES_DEPENDS}
    COMMENT "Generating selector name tables"
    VERBATIM)
target_sources(m2_host_core PRIVATE "${M2_SELECTOR_NAMES_HEADER}")
target_include_directories(m2_host_core PRIVATE "${M2_SELECTOR_NAMES_DIR}")

if(MSVC)
    target_compile_options(m2_host_core PRIVATE /fp:precise /W4)
    target_compile_options(m2_host_diagnostics PRIVATE /fp:precise /W4)
else()
    target_compile_options(m2_host_core PRIVATE
        -ffp-contract=off
        -fno-fast-math
        -Wall -Wextra -Wno-unused-parameter
    )
    target_compile_options(m2_host_diagnostics PRIVATE
        -ffp-contract=off
        -fno-fast-math
        -Wall -Wextra -Wno-unused-parameter
    )
endif()

if(IMSA_WITH_GAME)
    target_compile_definitions(m2_host_core PUBLIC M2_HAVE_RECOMP=1)
endif()

# Host input layer (SDL3 adapter, input map, host controls, broker bridge)
add_library(m2_host_input STATIC
    input/actions.cpp
    input/broker_bridge.cpp
    input/host_controls.cpp
    input/input_map.cpp
    input/sdl_adapter.cpp
)
target_include_directories(m2_host_input PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/input
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/shims
)
target_link_libraries(m2_host_input PUBLIC
    SDL3::SDL3
    m2_host_core
)
if(MSVC)
    target_compile_options(m2_host_input PRIVATE /fp:precise /W4)
else()
    target_compile_options(m2_host_input PRIVATE
        -ffp-contract=off
        -fno-fast-math
        -Wall -Wextra -Wno-unused-parameter
    )
endif()

# Host audio layer (SDL3 mixer, Ogg/Vorbis streaming, MPEG decoder)
add_library(m2_host_audio STATIC
    audio/audio_shim.cpp
    audio/audio_ogg.cpp
    audio/ogg_stream.cpp
    audio/mpeg_decoder.cpp
    ${CMAKE_SOURCE_DIR}/third_party/stb_vorbis.c
)
target_include_directories(m2_host_audio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/shims
    ${CMAKE_SOURCE_DIR}/third_party
)
target_link_libraries(m2_host_audio PUBLIC
    SDL3::SDL3
    m2_host_core
)
set(IMSA_HAVE_FFMPEG FALSE)
if(IMSA_WITH_FFMPEG)
    if(IMSA_FFMPEG_ROOT)
        # An explicit FFmpeg (the LGPL build release packages bundle) must be
        # the one used; never fall back to a system copy.
        find_path(M2_AVCODEC_INCLUDE libavcodec/avcodec.h
                  HINTS "${IMSA_FFMPEG_ROOT}/include" NO_DEFAULT_PATH)
        find_library(M2_AVCODEC_LIB avcodec
                     HINTS "${IMSA_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
        find_library(M2_AVUTIL_LIB avutil
                     HINTS "${IMSA_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
        if(NOT (M2_AVCODEC_INCLUDE AND M2_AVCODEC_LIB AND M2_AVUTIL_LIB))
            message(FATAL_ERROR
                "IMSA_FFMPEG_ROOT=${IMSA_FFMPEG_ROOT} does not contain "
                "libavcodec and libavutil with headers")
        endif()
    else()
        find_path(M2_AVCODEC_INCLUDE libavcodec/avcodec.h)
        find_library(M2_AVCODEC_LIB avcodec)
        find_library(M2_AVUTIL_LIB avutil)
    endif()
    if(M2_AVCODEC_INCLUDE AND M2_AVCODEC_LIB AND M2_AVUTIL_LIB)
        set(IMSA_HAVE_FFMPEG TRUE)
        target_compile_definitions(m2_host_audio PRIVATE M2_HAVE_FFMPEG=1)
        target_include_directories(m2_host_audio PRIVATE ${M2_AVCODEC_INCLUDE})
        target_link_libraries(m2_host_audio PRIVATE
            ${M2_AVCODEC_LIB} ${M2_AVUTIL_LIB})
        message(STATUS "m2_host_audio: FFmpeg found, MPEG decoder enabled")
    else()
        message(STATUS "m2_host_audio: FFmpeg not found, MPEG decoder stubs only")
    endif()
endif()
set(IMSA_HAVE_FFMPEG "${IMSA_HAVE_FFMPEG}" PARENT_SCOPE)
if(MSVC)
    target_compile_options(m2_host_audio PRIVATE /fp:precise /W4)
else()
    target_compile_options(m2_host_audio PRIVATE
        -ffp-contract=off
        -fno-fast-math
        -Wall -Wextra -Wno-unused-parameter
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(m2_host_audio PRIVATE
            -Wno-unused-function
            -Wno-unused-private-field
        )
    endif()
endif()
if(MSVC)
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/third_party/stb_vorbis.c
        PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/third_party/stb_vorbis.c
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# Host graphics layer (CLT interpreter, software clean renderer, OpenGL backend)
set(GRAPHICS_SOURCES
    graphics/guest_clt.cpp
    graphics/clean_renderer.cpp
)
if(IMSA_HAVE_GL)
    list(APPEND GRAPHICS_SOURCES graphics/gl_backend.cpp)
endif()

add_library(m2_host_graphics STATIC
    ${GRAPHICS_SOURCES}
)
target_include_directories(m2_host_graphics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/shims
)
target_link_libraries(m2_host_graphics PUBLIC
    SDL3::SDL3
    m2_host_diagnostics
)
if(IMSA_HAVE_GL)
    target_compile_definitions(m2_host_graphics PUBLIC M2_HAVE_GL=1)
    target_link_libraries(m2_host_graphics PUBLIC ${OPENGL_LIBRARIES})
endif()

if(MSVC)
    target_compile_options(m2_host_graphics PRIVATE /fp:precise /W4)
else()
    target_compile_options(m2_host_graphics PRIVATE
        -ffp-contract=off
        -fno-fast-math
        -Wall -Wextra -Wno-unused-parameter
    )
endif()

# Native host executable. Built with the recompiled guest if game data was
# supplied; built with the stub guest entry when no game data is present.
set(M2HOST_SOURCES core/main.cpp)
if(NOT IMSA_WITH_GAME)
    list(APPEND M2HOST_SOURCES core/game_entry_stub.cpp)
endif()

add_executable(m2host
    ${M2HOST_SOURCES}
)
target_include_directories(m2host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics
    ${CMAKE_CURRENT_SOURCE_DIR}/graphics
    ${CMAKE_CURRENT_SOURCE_DIR}/input
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/shims
)
target_link_libraries(m2host PRIVATE
    SDL3::SDL3
    m2_host_graphics
    m2_host_audio
    m2_host_input
    m2_host_core
    m2_host_diagnostics
    m2_host_platform
    m2_runtime
)
if(IMSA_WITH_GAME)
    target_compile_definitions(m2host PRIVATE M2_HAVE_RECOMP=1 IMSA_DEFAULT_DATA_DIR="${IMSA_GAME_DIR}")
endif()
if(IMSA_HAVE_FFMPEG)
    target_link_libraries(m2host PRIVATE
        ${M2_AVCODEC_LIB} ${M2_AVUTIL_LIB})
endif()
if(MSVC)
    target_compile_options(m2host PRIVATE /fp:precise /W4)
else()
    target_compile_options(m2host PRIVATE
        -ffp-contract=off
        -fno-fast-math
        -Wall -Wextra -Wno-unused-parameter
    )
endif()

# Without game data the M2_HAVE_RECOMP paths in these files are compiled out.
# Compile them once more with it set so builds without the disc (including
# public CI) still catch errors there. Nothing links these objects.
if(NOT IMSA_WITH_GAME)
    set(M2_RECOMP_CHECK_SOURCES
        core/main.cpp
        core/kernel_shim.cpp
        core/savestate.cpp
        core/selector_probe.cpp
        input/input_map.cpp
        diagnostics/diagnostic_control.cpp
        diagnostics/trace.cpp
    )
    add_library(m2_host_recomp_compile_check OBJECT ${M2_RECOMP_CHECK_SOURCES})
    target_include_directories(m2_host_recomp_compile_check PRIVATE
        $<TARGET_PROPERTY:m2host,INCLUDE_DIRECTORIES>)
    target_link_libraries(m2_host_recomp_compile_check PRIVATE
        SDL3::SDL3
        m2_host_graphics
        m2_host_audio
        m2_host_input
        m2_host_core
        m2_host_diagnostics
        m2_host_platform
        m2_runtime
    )
    target_compile_definitions(m2_host_recomp_compile_check PRIVATE M2_HAVE_RECOMP=1)
    if(NOT MSVC)
        target_compile_options(m2_host_recomp_compile_check PRIVATE
            -ffp-contract=off
            -fno-fast-math
            -Wall -Wextra -Wno-unused-parameter
        )
    endif()
endif()
