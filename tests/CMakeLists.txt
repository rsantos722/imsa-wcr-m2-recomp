# Unit tests run without game data and are what public CI executes.
add_subdirectory(unit)

find_package(Python3 COMPONENTS Interpreter)

if(WIN32 AND TARGET m2launcher AND Python3_Interpreter_FOUND)
    add_test(NAME m2_launcher_manifest
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_launcher_manifest.py"
                     "$<TARGET_FILE:m2launcher>")
endif()

# Host CLI regressions for input/broker recording and replay and explicit
# screenshot/capture output.  They drive the stub guest, so they are registered
# only in a game-data-free build; the unit tests above cover the same
# components in any configuration.
if(NOT IMSA_WITH_GAME AND TARGET m2host AND Python3_Interpreter_FOUND)
    add_test(NAME m2_host_cli
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/unit/test_host_cli.py"
                     --host "$<TARGET_FILE:m2host>")
    set_tests_properties(m2_host_cli PROPERTIES
        TIMEOUT 300
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

if(Python3_Interpreter_FOUND)
    find_program(PYTEST_EXECUTABLE NAMES pytest pytest3 pytest-3)
    if(PYTEST_EXECUTABLE)
        add_test(NAME m2_tools_operafs
                 COMMAND "${PYTEST_EXECUTABLE}" -q
                         "${CMAKE_SOURCE_DIR}/tests/tools/test_operafs.py")
        set_tests_properties(m2_tools_operafs PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            TIMEOUT 300
            ENVIRONMENT "PYTEST_DISABLE_PLUGIN_AUTOLOAD=1")

        add_test(NAME m2_tools_decoders
                 COMMAND "${PYTEST_EXECUTABLE}" -q
                         "${CMAKE_SOURCE_DIR}/tests/tools/test_decoders.py")
        set_tests_properties(m2_tools_decoders PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            TIMEOUT 300
            ENVIRONMENT "PYTEST_DISABLE_PLUGIN_AUTOLOAD=1")

        add_test(NAME m2_tools_companion_verification
                 COMMAND "${PYTEST_EXECUTABLE}" -q
                         "${CMAKE_SOURCE_DIR}/tests/tools/test_companion_verification.py")
        set_tests_properties(m2_tools_companion_verification PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT "PYTEST_DISABLE_PLUGIN_AUTOLOAD=1")

        add_test(NAME m2_tools_selector_names
                 COMMAND "${PYTEST_EXECUTABLE}" -q
                         "${CMAKE_SOURCE_DIR}/tests/tools/test_selector_names.py")
        set_tests_properties(m2_tools_selector_names PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT "PYTEST_DISABLE_PLUGIN_AUTOLOAD=1")

        add_test(NAME m2_tools_parity
                 COMMAND "${PYTEST_EXECUTABLE}" -q
                         "${CMAKE_SOURCE_DIR}/tests/tools/test_parity.py")
        set_tests_properties(m2_tools_parity PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT "PYTEST_DISABLE_PLUGIN_AUTOLOAD=1")

        add_test(NAME m2_tools_generate_cache
                 COMMAND "${PYTEST_EXECUTABLE}" -q
                         "${CMAKE_SOURCE_DIR}/tests/tools/test_generate_cache.py")
        set_tests_properties(m2_tools_generate_cache PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT "PYTEST_DISABLE_PLUGIN_AUTOLOAD=1")

        add_test(NAME m2_tools_m2ctl
                 COMMAND "${PYTEST_EXECUTABLE}" -q
                         "${CMAKE_SOURCE_DIR}/tests/tools/test_m2ctl.py")
        set_tests_properties(m2_tools_m2ctl PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT "PYTEST_DISABLE_PLUGIN_AUTOLOAD=1")

        add_test(NAME m2_tools_env_reference
                 COMMAND "${PYTEST_EXECUTABLE}" -q
                         "${CMAKE_SOURCE_DIR}/tests/tools/test_env_reference.py")
        set_tests_properties(m2_tools_env_reference PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT "PYTEST_DISABLE_PLUGIN_AUTOLOAD=1")

        add_test(NAME m2_tools_bundle_staging
                 COMMAND "${PYTEST_EXECUTABLE}" -q
                         "${CMAKE_SOURCE_DIR}/tests/tools/test_bundle_staging.py")
        set_tests_properties(m2_tools_bundle_staging PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT "PYTEST_DISABLE_PLUGIN_AUTOLOAD=1")

        add_test(NAME m2_tools_tire_sweep
                 COMMAND "${PYTEST_EXECUTABLE}" -q
                         "${CMAKE_SOURCE_DIR}/tests/tools/test_tire_sweep.py")
        set_tests_properties(m2_tools_tire_sweep PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT "PYTEST_DISABLE_PLUGIN_AUTOLOAD=1")
    else()
        message(STATUS "pytest not found: tools tests are not registered")
    endif()
endif()

# The parity harness needs two host binaries built with the user's game data.
# Point these at the reference and candidate builds to register it with CTest.
set(IMSA_PARITY_REFERENCE "" CACHE FILEPATH
    "Reference m2host binary for tests/guest/parity.py")
set(IMSA_PARITY_CANDIDATE "" CACHE FILEPATH
    "Candidate m2host binary for tests/guest/parity.py")

if(Python3_Interpreter_FOUND AND IMSA_PARITY_REFERENCE AND IMSA_PARITY_CANDIDATE)
    add_test(NAME m2_parity
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_SOURCE_DIR}/tests/guest/parity.py"
                     --reference "${IMSA_PARITY_REFERENCE}"
                     --candidate "${IMSA_PARITY_CANDIDATE}"
                     --root "${IMSA_GAME_DIR}")
    set_tests_properties(m2_parity PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 600
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
else()
    message(STATUS "parity harness not registered: set IMSA_PARITY_REFERENCE and IMSA_PARITY_CANDIDATE")
endif()

if(IMSA_WITH_GAME AND TARGET m2_runtime_smoke)
    add_test(NAME m2_runtime_smoke COMMAND m2_runtime_smoke)
    set_tests_properties(m2_runtime_smoke PROPERTIES TIMEOUT 120)
endif()

# Generated-code tests: they call lifted guest functions or the recomp entry,
# so they exist only in game-data builds.
if(IMSA_WITH_GAME)
    # Zero-size libm symbols are real leaf functions in the image.
    add_executable(m2_libm_zero_size_test guest/test_libm_zero_size.c)
    target_link_libraries(m2_libm_zero_size_test PRIVATE m2_runtime)
    add_test(NAME m2_libm_zero_size COMMAND m2_libm_zero_size_test)

    # Kernel shutdown and restore: stale handles do not cross epochs.
    add_executable(m2_reset_reopen_lifecycle_test guest/test_reset_reopen_lifecycle.c)
    target_link_libraries(m2_reset_reopen_lifecycle_test PRIVATE m2_runtime)
    add_test(NAME m2_reset_reopen_lifecycle COMMAND m2_reset_reopen_lifecycle_test)

    # Frozen-state tire experiment: original setup and force functions, no boot.
    add_executable(m2_tire_probe guest/tire_probe.c)
    target_link_libraries(m2_tire_probe PRIVATE m2_runtime)
    add_test(NAME m2_tire_probe COMMAND m2_tire_probe
             --game-data "${IMSA_GAME_DIR}" --self-test)
    set_tests_properties(m2_tire_probe PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

    add_executable(m2_steering_vehicle_test guest/test_steering_vehicle.c)
    target_link_libraries(m2_steering_vehicle_test PRIVATE m2_runtime)
    add_test(NAME m2_steering_vehicle COMMAND m2_steering_vehicle_test "${IMSA_GAME_DIR}")
    set_tests_properties(m2_steering_vehicle PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)

    # Audio folio items, connections, and DSPP graph on the disc's own templates.
    add_executable(m2_audio_items_test guest/test_audio_items.c)
    target_link_libraries(m2_audio_items_test PRIVATE m2_runtime)
    target_include_directories(m2_audio_items_test PRIVATE
        ${CMAKE_SOURCE_DIR}/runtime/src
        ${CMAKE_SOURCE_DIR}/tests/include
        ${CMAKE_BINARY_DIR})
    target_compile_definitions(m2_audio_items_test PRIVATE
        RECOMP_BUILD=1
        IMSA_TEST_GAME_DIR="${IMSA_GAME_DIR}")
    add_test(NAME m2_audio_items COMMAND m2_audio_items_test)
    set_tests_properties(m2_audio_items PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
endif()

# Headless input adapter/map/broker self-test. No game data required.
add_executable(m2_input_selftest
    ${CMAKE_SOURCE_DIR}/host/input/input_selftest.cpp)
target_link_libraries(m2_input_selftest PRIVATE
    m2_host_input
    m2_host_core
    m2_host_diagnostics
    m2_runtime
    SDL3::SDL3
)
add_test(NAME m2_input_selftest COMMAND m2_input_selftest)

# MPEG audio boundary test. Needs the user's Intro.mov and FFmpeg; exits 3
# when either is absent.
if(IMSA_WITH_GAME AND IMSA_HAVE_FFMPEG)
    add_executable(m2_mpeg_audio_test guest/test_mpeg_audio.cpp)
    target_link_libraries(m2_mpeg_audio_test PRIVATE m2_host_audio)
    target_compile_definitions(m2_mpeg_audio_test PRIVATE
        IMSA_TEST_MOVIE_PATH="${IMSA_GAME_DIR}/driving/data/movies/Intro.mov")
    add_test(NAME m2_mpeg_audio COMMAND m2_mpeg_audio_test)
    set_tests_properties(m2_mpeg_audio PROPERTIES
        SKIP_RETURN_CODE 3
        TIMEOUT 120)
endif()
