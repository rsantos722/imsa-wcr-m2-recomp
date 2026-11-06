# Launcher unit tests. The libraries and sources under test live in launcher/.
set(IMSA_LAUNCHER_DIR "${CMAKE_SOURCE_DIR}/launcher")

add_executable(m2_launcher_disc_test disc_import_test.cpp)
target_include_directories(m2_launcher_disc_test PRIVATE "${IMSA_LAUNCHER_DIR}")
target_link_libraries(m2_launcher_disc_test PRIVATE m2_launcher_disc)
add_test(NAME m2_launcher_disc COMMAND m2_launcher_disc_test)

add_executable(m2_launcher_saves_test save_data_test.cpp)
target_link_libraries(m2_launcher_saves_test PRIVATE m2_launcher_saves)
add_test(NAME m2_launcher_saves COMMAND m2_launcher_saves_test)

add_executable(m2_launcher_settings_test run_flags_test.cpp)
target_include_directories(m2_launcher_settings_test PRIVATE
    "${IMSA_LAUNCHER_DIR}"
    "${CMAKE_SOURCE_DIR}/host/input"
    "${CMAKE_SOURCE_DIR}/host/core"
    "${CMAKE_SOURCE_DIR}/runtime/include"
    "${CMAKE_SOURCE_DIR}/shims"
)
target_link_libraries(m2_launcher_settings_test PRIVATE SDL3::SDL3)
add_test(NAME m2_launcher_settings COMMAND m2_launcher_settings_test)

# Synthetic asset fixture always runs; the checks against the user's game data
# run only when IMSA_DATA_DIR is set (a game-enabled build sets it).
add_executable(m2_launcher_assets_test
    "${IMSA_LAUNCHER_DIR}/asset_raster.cpp"
    "${IMSA_LAUNCHER_DIR}/asset_texture.cpp"
    "${IMSA_LAUNCHER_DIR}/asset_model.cpp"
    asset_model_test.cpp
)
target_include_directories(m2_launcher_assets_test PRIVATE
    "${IMSA_LAUNCHER_DIR}"
    "${CMAKE_SOURCE_DIR}/tests/include"
)
add_test(NAME m2_launcher_assets COMMAND m2_launcher_assets_test)
set_tests_properties(m2_launcher_assets PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    SKIP_RETURN_CODE 3)
if(IMSA_WITH_GAME)
    set_property(TEST m2_launcher_assets APPEND PROPERTY
        ENVIRONMENT "IMSA_DATA_DIR=${IMSA_GAME_DIR}")
endif()
