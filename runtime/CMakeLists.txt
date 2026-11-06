# runtime/CMakeLists.txt — guest runtime library

find_package(Threads REQUIRED)

set(RUNTIME_SOURCES
    src/analog_steering.c
    src/adaptive_driver.c
    src/car_lod.c
    src/control_modes.c
    src/device_command_contract.c
    src/diagnostic_gate.c
    src/dsp_template_loader.c
    src/font_folio.c
    src/guest_input_frame.c
    src/guest_kernel.c
    src/guest_module.c
    src/guest_provenance.c
    src/guest_tracepoints.c
    src/hud_tach_fix.c
    src/sha256.c
    src/shim_stub.c
    src/te_command_contract.c
    src/trace_stubs.c
    src/utf8_path.c
)

if(IMSA_WITH_GAME)
    list(APPEND RUNTIME_SOURCES
        src/recomp_entry.c
        src/recomp_runtime.c
        ${IMSA_GENERATED_SOURCES}
    )
endif()

add_library(m2_runtime STATIC ${RUNTIME_SOURCES})
target_link_libraries(m2_runtime PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# libm is a separate library on Linux/glibc (and other non-Apple Unixes).
# The runtime calls powf and the public float helpers use fmaf, so every
# consumer -- including m2_runtime_smoke with generated guest code -- must
# link it. Apple folds math into libSystem; MSVC and MinGW use the CRT.
if(UNIX AND NOT APPLE)
    target_link_libraries(m2_runtime PUBLIC m)
endif()

target_include_directories(m2_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/shims
)

if(IMSA_WITH_GAME)
    target_include_directories(m2_runtime PUBLIC
        ${IMSA_GENERATED_DIR}
    )
endif()

if(MSVC)
    target_compile_options(m2_runtime PRIVATE /fp:precise)
else()
    target_compile_options(m2_runtime PRIVATE
        -ffp-contract=off
        -fno-fast-math
        -fno-unsafe-math-optimizations
        -Wall -Wextra -Wno-unused-parameter -Wno-unused-label -Wno-unused-function
    )
endif()

target_compile_definitions(m2_runtime PRIVATE RECOMP_BUILD=1)
if(IMSA_WITH_GAME)
    target_compile_definitions(m2_runtime PRIVATE IMSA_DEFAULT_DATA_DIR="${IMSA_GAME_DIR}")
endif()

# Without game data the guest runtime proper is not linked into anything, but
# it is still compiled so builds without the disc (including public CI) catch
# compiler and platform errors in it. Nothing links these objects.
if(NOT IMSA_WITH_GAME)
    add_library(m2_runtime_compile_check OBJECT
        src/recomp_entry.c
        src/recomp_runtime.c
    )
    target_include_directories(m2_runtime_compile_check PRIVATE
        $<TARGET_PROPERTY:m2_runtime,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_options(m2_runtime_compile_check PRIVATE
        $<TARGET_PROPERTY:m2_runtime,COMPILE_OPTIONS>)
    target_compile_definitions(m2_runtime_compile_check PRIVATE RECOMP_BUILD=1)
endif()

# Standalone smoke test when built with game data
if(IMSA_WITH_GAME)
    add_executable(m2_runtime_smoke src/recomp_smoke_main.c)
    target_compile_definitions(m2_runtime_smoke PRIVATE RECOMP_BUILD=1)
    target_link_libraries(m2_runtime_smoke PRIVATE m2_runtime)
endif()
