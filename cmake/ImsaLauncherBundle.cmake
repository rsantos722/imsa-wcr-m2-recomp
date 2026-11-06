# ImsaLauncherBundle.cmake - macOS launcher app-bundle staging helpers.
#
# m2host and m2launcher are separate targets. Copying m2host from a POST_BUILD
# command on m2launcher is not dependency-correct: an incremental build that
# relinks m2host without relinking m2launcher never runs the copy and leaves
# Contents/MacOS/m2host stale. Stage the host from an ALL target that depends
# on m2host instead, and let the launcher target depend on the staging target
# so explicit launcher-only builds refresh the bundle too.

include_guard(GLOBAL)

# imsa_add_launcher_bundle_host(
#     TARGET <custom-target-name>
#     HOST_TARGET <executable-target>
#     BUNDLE_DIR <path-to.app>
#     [DEST_NAME <file-name>])
#
# TARGET is added to ALL and depends on HOST_TARGET, so a host-only rebuild
# refreshes the bundle. Callers can also add_dependencies() their launcher
# target on TARGET to cover explicit launcher-only builds. The staging script
# compares contents and skips the copy when the bundle is already current, so
# the always-run target stays cheap.
#
# BUNDLE_DIR may contain generator expressions such as $<CONFIG>; they are
# evaluated for the active configuration by supported multi-config generators.
# DEST_NAME defaults to the host target name plus the platform executable
# suffix, matching the name produced by the compiler.
function(imsa_add_launcher_bundle_host)
    cmake_parse_arguments(ARG "" "TARGET;HOST_TARGET;BUNDLE_DIR;DEST_NAME" "" ${ARGN})

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "imsa_add_launcher_bundle_host: TARGET is required")
    endif()
    if(NOT ARG_HOST_TARGET)
        message(FATAL_ERROR "imsa_add_launcher_bundle_host: HOST_TARGET is required")
    endif()
    if(NOT ARG_BUNDLE_DIR)
        message(FATAL_ERROR "imsa_add_launcher_bundle_host: BUNDLE_DIR is required")
    endif()
    if(NOT TARGET "${ARG_HOST_TARGET}")
        message(FATAL_ERROR
            "imsa_add_launcher_bundle_host: HOST_TARGET '${ARG_HOST_TARGET}' "
            "is not a CMake target")
    endif()
    if(NOT ARG_DEST_NAME)
        set(ARG_DEST_NAME "${ARG_HOST_TARGET}${CMAKE_EXECUTABLE_SUFFIX}")
    endif()

    set(_stage_script
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ImsaStageBundleExecutable.cmake")
    set(_staged_host
        "${ARG_BUNDLE_DIR}/Contents/MacOS/${ARG_DEST_NAME}")

    add_custom_target(${ARG_TARGET} ALL
        COMMAND "${CMAKE_COMMAND}"
                "-DIMSA_BUNDLE_SOURCE=$<TARGET_FILE:${ARG_HOST_TARGET}>"
                "-DIMSA_BUNDLE_DESTINATION=${_staged_host}"
                "-DIMSA_BUNDLE_LABEL=${ARG_HOST_TARGET}"
                -P "${_stage_script}"
        DEPENDS "${_stage_script}"
        COMMENT "Staging ${ARG_HOST_TARGET} into the macOS launcher app bundle"
        VERBATIM)
    add_dependencies(${ARG_TARGET} ${ARG_HOST_TARGET})
endfunction()
