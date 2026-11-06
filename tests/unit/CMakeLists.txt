# tests/unit/CMakeLists.txt — unit tests running without game data (public CI)

# VBL pacer timing test
add_executable(m2_vbl_pacer_test test_vbl_pacer.cpp)
target_link_libraries(m2_vbl_pacer_test PRIVATE m2_host_core)
add_test(NAME m2_vbl_pacer_test COMMAND m2_vbl_pacer_test)

# Filesystem shim tests (directory open/read/close, root volume mounting)
add_executable(m2_fs_shim_test test_filesystem_shim.cpp)
target_include_directories(m2_fs_shim_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/include
)
target_link_libraries(m2_fs_shim_test PRIVATE m2_host_core)
add_test(NAME m2_fs_shim_test COMMAND m2_fs_shim_test)

# UTF-8 path handling for runtime-owned file opens (wide _wfopen on Windows)
add_executable(m2_utf8_path_test test_utf8_path.cpp)
target_include_directories(m2_utf8_path_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/include)
target_link_libraries(m2_utf8_path_test PRIVATE
    m2_runtime
    m2_host_platform
)
add_test(NAME m2_utf8_path COMMAND m2_utf8_path_test)

# Platform helpers (executable directory, runtime icon discovery)
add_executable(m2_platform_test test_platform.cpp)
target_include_directories(m2_platform_test PRIVATE
    ${CMAKE_SOURCE_DIR}/host/core
    ${CMAKE_SOURCE_DIR}/tests/include
)
target_link_libraries(m2_platform_test PRIVATE m2_host_platform)
add_test(NAME m2_platform COMMAND m2_platform_test)

# Guest kernel task completion, restore rehydration, and continuation
add_executable(m2_guest_kernel_task_ex_test test_guest_kernel_task_ex.c)
target_link_libraries(m2_guest_kernel_task_ex_test PRIVATE m2_runtime)
add_test(NAME m2_guest_kernel_task_ex COMMAND m2_guest_kernel_task_ex_test)

# Driving game argv: empty unless M2_GUEST_ARGS is set
add_executable(m2_guest_boot_args_test test_guest_boot_args.c)
target_include_directories(m2_guest_boot_args_test PRIVATE "${CMAKE_SOURCE_DIR}/runtime/include")
add_test(NAME m2_guest_boot_args COMMAND m2_guest_boot_args_test)

add_executable(m2_guest_kernel_wait_idle_test test_guest_kernel_wait_idle.c)
target_link_libraries(m2_guest_kernel_wait_idle_test PRIVATE m2_runtime)
add_test(NAME m2_guest_kernel_wait_idle COMMAND m2_guest_kernel_wait_idle_test)
set_tests_properties(m2_guest_kernel_wait_idle PROPERTIES TIMEOUT 30)

# Guest fused multiply-add rounding against exact integer arithmetic
add_executable(m2_ppc_fma_test test_ppc_fma.c)
target_link_libraries(m2_ppc_fma_test PRIVATE m2_runtime)
add_test(NAME m2_ppc_fma COMMAND m2_ppc_fma_test)

# DSPP FDMA serviceable-channel mask and priority
add_executable(m2_dspp_dma_test test_dspp_dma.c)
target_include_directories(m2_dspp_dma_test PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/src
)
target_link_libraries(m2_dspp_dma_test PRIVATE m2_runtime)
add_test(NAME m2_dspp_dma COMMAND m2_dspp_dma_test)

# DSPP interpreter golden execution (operand decoding and cyclic program)
add_executable(m2_dspp_exec_test test_dspp_exec.c)
target_include_directories(m2_dspp_exec_test PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/src
)
target_link_libraries(m2_dspp_exec_test PRIVATE m2_runtime)
add_test(NAME m2_dspp_exec COMMAND m2_dspp_exec_test)

# Diagnostic suspension gate across guest threads
add_executable(m2_diagnostic_gate_test test_diagnostic_gate.c)
target_link_libraries(m2_diagnostic_gate_test PRIVATE m2_runtime)
add_test(NAME m2_diagnostic_gate COMMAND m2_diagnostic_gate_test)
set_tests_properties(m2_diagnostic_gate PROPERTIES TIMEOUT 30)

# Guest input frame determinism and big-endian guest layout
add_executable(m2_analog_steering_test test_analog_steering.c)
target_link_libraries(m2_analog_steering_test PRIVATE m2_runtime)
add_test(NAME m2_analog_steering COMMAND m2_analog_steering_test)

add_executable(m2_guest_input_frame_test test_guest_input_frame.c)
target_link_libraries(m2_guest_input_frame_test PRIVATE m2_runtime)
add_test(NAME m2_guest_input_frame COMMAND m2_guest_input_frame_test)

# Exterior HUD tachometer range fix (game-data-free scale math)
add_executable(m2_hud_tach_fix_test test_hud_tach_fix.c)
target_link_libraries(m2_hud_tach_fix_test PRIVATE m2_runtime)
add_test(NAME m2_hud_tach_fix COMMAND m2_hud_tach_fix_test)

# Opt-in highest-detail car LOD override
add_executable(m2_car_lod_test test_car_lod.c)
target_link_libraries(m2_car_lod_test PRIVATE m2_runtime)
add_test(NAME m2_car_lod COMMAND m2_car_lod_test)

# Forced transition effect diagnostic (M2_FORCE_TRANSITION)
add_executable(m2_force_transition_test test_force_transition.c)
target_link_libraries(m2_force_transition_test PRIVATE m2_runtime)
add_test(NAME m2_force_transition COMMAND m2_force_transition_test)

# Custom control adapter: digital-pad protocol selection, direction bits, and
# analog/digital steering combine around the guest ramp. Stubs the generated
# guest entry points, so it belongs to the game-data-free unit build.
if(NOT IMSA_WITH_GAME)
    add_executable(m2_control_modes_test test_control_modes.c)
    target_link_libraries(m2_control_modes_test PRIVATE m2_runtime)
    add_test(NAME m2_control_modes COMMAND m2_control_modes_test)
endif()

# Host audio voice interface (controlled sink, no device). The guest audio
# contracts come from the recomp runtime, so this runs in game-data builds.
if(IMSA_WITH_GAME)
    add_executable(m2_audio_shim_test test_audio_shim.cpp)
    target_link_libraries(m2_audio_shim_test PRIVATE m2_host_audio SDL3::SDL3)
    add_test(NAME m2_audio_shim COMMAND m2_audio_shim_test)

    add_executable(m2_audio_dspp_fifo_test test_audio_dspp_fifo.cpp)
    target_link_libraries(m2_audio_dspp_fifo_test PRIVATE m2_host_audio SDL3::SDL3)
    add_test(NAME m2_audio_dspp_fifo COMMAND m2_audio_dspp_fifo_test)
endif()

# MPEG still packet recovery
add_executable(m2_mpeg_still_packet_test test_mpeg_still_packet.cpp)
target_include_directories(m2_mpeg_still_packet_test PRIVATE
    ${CMAKE_SOURCE_DIR}/host/audio)
add_test(NAME m2_mpeg_still_packet COMMAND m2_mpeg_still_packet_test)

# Filesystem-root to game-data-root inference
add_executable(m2_data_paths_test test_data_paths.cpp)
target_include_directories(m2_data_paths_test PRIVATE
    ${CMAKE_SOURCE_DIR}/host/core
    ${CMAKE_SOURCE_DIR}/tests/include
)
add_test(NAME m2_data_paths COMMAND m2_data_paths_test)

# CLT register/field table test (project-owned command-list facts)
add_executable(m2_clt_registers_test test_clt_registers.cpp)
target_include_directories(m2_clt_registers_test PRIVATE ${CMAKE_SOURCE_DIR}/shims)
add_test(NAME m2_clt_registers COMMAND m2_clt_registers_test)

# Diagnostic trace limit test
add_executable(m2_trace_limit_test test_trace_limit.cpp)
target_link_libraries(m2_trace_limit_test PRIVATE
    m2_host_diagnostics
    m2_host_platform
)
add_test(NAME m2_trace_limit COMMAND m2_trace_limit_test)

# Input/broker recorder initialization and UTF-8 round trip.
add_executable(m2_input_recorder_test test_input_recorder.cpp)
target_link_libraries(m2_input_recorder_test PRIVATE
    m2_host_diagnostics
    m2_host_platform
)
add_test(NAME m2_input_recorder COMMAND m2_input_recorder_test)

# Software capture output error reporting and valid-output control.
add_executable(m2_capture_output_test test_capture_output.cpp)
target_link_libraries(m2_capture_output_test PRIVATE m2_host_graphics)
add_test(NAME m2_capture_output COMMAND m2_capture_output_test)

# Savestate roundtrip test (runs when guest context is available)
if(IMSA_WITH_GAME)
    add_executable(m2_savestate_test test_savestate.cpp)
    target_link_libraries(m2_savestate_test PRIVATE m2_host_core)
    target_compile_definitions(m2_savestate_test PRIVATE M2_HAVE_RECOMP=1)
    add_test(NAME m2_savestate_roundtrip COMMAND m2_savestate_test)
endif()

# Letterbox 4:3 fit geometry test
add_executable(m2_letterbox_test test_letterbox.cpp)
target_include_directories(m2_letterbox_test PRIVATE ${CMAKE_SOURCE_DIR}/host/graphics)
target_link_libraries(m2_letterbox_test PRIVATE SDL3::SDL3)
add_test(NAME m2_letterbox COMMAND m2_letterbox_test)

# Texture scaling options test
add_executable(m2_texture_scaling_test test_texture_scaling.cpp)
target_include_directories(m2_texture_scaling_test PRIVATE ${CMAKE_SOURCE_DIR}/host/graphics)
add_test(NAME m2_texture_scaling COMMAND m2_texture_scaling_test)

# Retired-texture cache identity, FIFO eviction, and byte budget
add_executable(m2_cel_texture_cache_test test_cel_texture_cache.cpp)
target_include_directories(m2_cel_texture_cache_test PRIVATE ${CMAKE_SOURCE_DIR}/host/graphics)
add_test(NAME m2_cel_texture_cache COMMAND m2_cel_texture_cache_test)

# Guest CLT parser/queue simulation test
add_executable(m2_guest_clt_test test_guest_clt.cpp)
target_include_directories(m2_guest_clt_test PRIVATE
    ${CMAKE_SOURCE_DIR}/host/graphics
    ${CMAKE_SOURCE_DIR}/tests/include
)
target_link_libraries(m2_guest_clt_test PRIVATE
    m2_host_graphics
    m2_host_diagnostics
    m2_host_platform
    m2_runtime
    SDL3::SDL3
)
add_test(NAME m2_guest_clt COMMAND m2_guest_clt_test)

# OpenGL backend parity and target tests
if(IMSA_HAVE_GL)
    add_executable(m2_gl_clt_targets_test test_gl_clt_targets.cpp)
    target_include_directories(m2_gl_clt_targets_test PRIVATE ${CMAKE_SOURCE_DIR}/host/graphics)
    target_link_libraries(m2_gl_clt_targets_test PRIVATE SDL3::SDL3 ${OPENGL_LIBRARIES})
    add_test(NAME m2_gl_clt_targets COMMAND m2_gl_clt_targets_test)
    set_tests_properties(m2_gl_clt_targets PROPERTIES SKIP_RETURN_CODE 3)

    add_executable(m2_gl_backend_parity_test test_gl_backend_parity.cpp)
    target_include_directories(m2_gl_backend_parity_test PRIVATE
        ${CMAKE_SOURCE_DIR}/host/graphics
        ${CMAKE_SOURCE_DIR}/tests/include
    )
    target_link_libraries(m2_gl_backend_parity_test PRIVATE
        m2_host_graphics
        SDL3::SDL3
        ${OPENGL_LIBRARIES}
    )
    target_compile_definitions(m2_gl_backend_parity_test PRIVATE M2_HAVE_GL=1)
    if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
       AND CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "16.2.0")
        # UCRT64 GCC 16.2.0 at -O3 substitutes requested=1 in the MSAA
        # fixture even when run_backend passes 4. Disabling IPA constant
        # propagation preserves the argument; keep the workaround test-local.
        target_compile_options(m2_gl_backend_parity_test PRIVATE -fno-ipa-cp)
    endif()
    add_test(NAME m2_gl_backend_parity COMMAND m2_gl_backend_parity_test)
    set_tests_properties(m2_gl_backend_parity PROPERTIES SKIP_RETURN_CODE 3)
endif()

add_subdirectory(launcher)
