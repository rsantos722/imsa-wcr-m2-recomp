# ImsaStageBundleExecutable.cmake - stage one executable into a macOS app bundle.
#
# Invoked at build time by ImsaLauncherBundle.cmake and directly by
# tests/tools/test_bundle_staging.py. The destination is compared by content
# first so a no-op build does not rewrite the bundle, and the executable bit is
# restored on Unix in case the destination was created by an archive-like copy.
#
# Required -D arguments:
#   IMSA_BUNDLE_SOURCE       built executable to stage
#   IMSA_BUNDLE_DESTINATION  destination file (not a directory)
#
# Optional:
#   IMSA_BUNDLE_LABEL        label used in status messages

if(NOT DEFINED IMSA_BUNDLE_SOURCE OR IMSA_BUNDLE_SOURCE STREQUAL "")
    message(FATAL_ERROR "IMSA_BUNDLE_SOURCE is required")
endif()
if(NOT DEFINED IMSA_BUNDLE_DESTINATION OR IMSA_BUNDLE_DESTINATION STREQUAL "")
    message(FATAL_ERROR "IMSA_BUNDLE_DESTINATION is required")
endif()

get_filename_component(_imsa_source "${IMSA_BUNDLE_SOURCE}" ABSOLUTE)
get_filename_component(_imsa_destination "${IMSA_BUNDLE_DESTINATION}" ABSOLUTE)
get_filename_component(_imsa_destination_dir "${_imsa_destination}" DIRECTORY)

if(NOT EXISTS "${_imsa_source}" OR IS_DIRECTORY "${_imsa_source}")
    message(FATAL_ERROR
        "Cannot stage '${_imsa_source}': the source executable does not exist")
endif()

set(_imsa_label "${IMSA_BUNDLE_LABEL}")
if(_imsa_label STREQUAL "")
    get_filename_component(_imsa_label "${_imsa_source}" NAME)
endif()

set(_imsa_copy_needed TRUE)
if(EXISTS "${_imsa_destination}" AND NOT IS_DIRECTORY "${_imsa_destination}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${_imsa_source}" "${_imsa_destination}"
        RESULT_VARIABLE _imsa_compare)
    if(_imsa_compare EQUAL 0)
        set(_imsa_copy_needed FALSE)
    endif()
endif()

if(_imsa_copy_needed)
    file(MAKE_DIRECTORY "${_imsa_destination_dir}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_imsa_source}" "${_imsa_destination}"
        RESULT_VARIABLE _imsa_copy)
    if(NOT _imsa_copy EQUAL 0)
        message(FATAL_ERROR
            "Failed to stage '${_imsa_source}' at '${_imsa_destination}'")
    endif()
    message(STATUS "Staged ${_imsa_label} at ${_imsa_destination}")
else()
    message(STATUS "Up to date: ${_imsa_label} at ${_imsa_destination}")
endif()

if(UNIX)
    file(CHMOD "${_imsa_destination}"
         PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
endif()
