# ImsaPackaging.cmake - install rules and CPack configuration.
#
# One package per platform, all named imsa-wcr-m2-recomp-<version>-<os>-<arch>:
#   Windows  portable ZIP. Only the launcher stub and README.txt sit at the top;
#            the executables and DLLs are in bin/ and the notices in licenses/.
#            Saves, settings and game files all go in userdata/, so nothing is
#            written outside the package folder and an update only has to
#            carry that one folder across.
#   macOS    DMG holding one app bundle, with m2host inside Contents/MacOS and
#            dependent dylibs in Contents/Frameworks (ad-hoc signed).
#   Linux    portable tar.gz. Only a run script and README.txt sit at the top,
#            beside bin, lib, share, licenses and userdata, which holds saves,
#            settings and game files. Its portable files are left out of cmake --install,
#            which gives a /usr-style tree; packaging/appimage/ assembles the
#            AppImage from that tree.
#
# Include after every add_subdirectory(); it needs the m2host and m2launcher
# targets. Without the launcher (no wxWidgets) there is nothing to package.

include_guard(GLOBAL)

if(NOT TARGET m2host OR NOT TARGET m2launcher)
    message(STATUS "Packaging disabled: needs both m2host and m2launcher")
    return()
endif()

include(GNUInstallDirs)

string(TOLOWER "${CMAKE_SYSTEM_NAME}" _imsa_os)
if(APPLE)
    set(_imsa_os "macos")
endif()
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _imsa_arch)
if(_imsa_arch STREQUAL "amd64")
    set(_imsa_arch "x86_64")
elseif(_imsa_arch STREQUAL "aarch64")
    set(_imsa_arch "arm64")
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(GET CMAKE_OSX_ARCHITECTURES 0 _imsa_arch)
endif()
# On Windows CMake takes the processor from PROCESSOR_ARCHITECTURE, which some
# non-interactive MSYS2 shells don't pass through. Stop rather than name the
# package "...-windows-.zip".
if(_imsa_arch STREQUAL "")
    message(FATAL_ERROR "CMAKE_SYSTEM_PROCESSOR is empty, so the package name "
        "has no architecture. On Windows, export PROCESSOR_ARCHITECTURE (e.g. "
        "AMD64) and configure a fresh build directory.")
endif()

# Distribution FFmpeg builds enable GPL components, which would make a bundled
# package GPL. Packages must use the LGPL build from packaging/ffmpeg, passed
# as -DIMSA_FFMPEG_ROOT.
if(IMSA_HAVE_FFMPEG AND NOT IMSA_FFMPEG_ROOT)
    install(CODE "message(WARNING \"This package bundles a system FFmpeg, which \
may be GPL-licensed. Release packages must configure with \
-DIMSA_FFMPEG_ROOT=<prefix from packaging/ffmpeg/build-ffmpeg.sh>.\")")
endif()

set(_imsa_notices
    "${CMAKE_SOURCE_DIR}/LICENSE"
    "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES"
    "${CMAKE_SOURCE_DIR}/ORIGINAL_CREDITS.md")

# -- Windows: portable folder with one program at the top ----------------------

if(WIN32)
    # Dependency DLLs come from the MinGW prefix that holds the compiler. The
    # LGPL FFmpeg folder is searched first: MSYS2's own FFmpeg package uses the
    # same DLL names and would otherwise bundle its GPL build.
    get_filename_component(_imsa_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    set(_imsa_dll_dirs "${_imsa_compiler_dir}")
    if(IMSA_FFMPEG_ROOT)
        list(PREPEND _imsa_dll_dirs "${IMSA_FFMPEG_ROOT}/bin")
    endif()
    install(TARGETS m2host m2launcher
        RUNTIME_DEPENDENCIES
            PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
            POST_EXCLUDE_REGEXES ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*"
            DIRECTORIES ${_imsa_dll_dirs}
        RUNTIME DESTINATION bin)
    install(FILES "${IMSA_LAUNCHER_ICON_PNG}" DESTINATION bin)
    install(TARGETS m2launcher_stub RUNTIME DESTINATION .)
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/windows/README.txt" DESTINATION .)
    # The file keeps the otherwise empty folder in the ZIP.
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/portable/userdata-README.txt"
        DESTINATION userdata RENAME README.txt)
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/portable/portable.txt" DESTINATION bin)
    if(IMSA_FFMPEG_ROOT)
        install(FILES "${IMSA_FFMPEG_ROOT}/FFMPEG_SOURCE.txt" DESTINATION licenses)
    endif()
    install(FILES ${_imsa_notices} DESTINATION licenses)
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/third_party/licenses/"
        DESTINATION licenses FILES_MATCHING PATTERN "*.txt")
    set(CPACK_GENERATOR ZIP)

# -- macOS: one app bundle in a DMG -------------------------------------------

elseif(APPLE)
    # The build already stages <launcher>.app with m2host in Contents/MacOS
    # (launcher/CMakeLists.txt); install that tree, then make it relocatable.
    set(_imsa_app_name "IMSA WCR M2 Recomp Launcher.app")
    install(DIRECTORY "${IMSA_LAUNCHER_APP_DIR}"
        DESTINATION .
        USE_SOURCE_PERMISSIONS)
    install(FILES ${_imsa_notices}
        DESTINATION "${_imsa_app_name}/Contents/Resources")
    if(IMSA_FFMPEG_ROOT)
        install(FILES "${IMSA_FFMPEG_ROOT}/FFMPEG_SOURCE.txt"
            DESTINATION "${_imsa_app_name}/Contents/Resources")
    endif()
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/third_party/licenses"
        DESTINATION "${_imsa_app_name}/Contents/Resources"
        FILES_MATCHING PATTERN "*.txt")
    set(_imsa_dylib_dirs "")
    if(IMSA_FFMPEG_ROOT)
        list(APPEND _imsa_dylib_dirs "${IMSA_FFMPEG_ROOT}/lib")
    endif()
    foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND _imsa_dylib_dirs "${_prefix}/lib")
    endforeach()
    install(CODE "
        set(_app \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_imsa_app_name}\")
        include(BundleUtilities)
        fixup_bundle(\"\${_app}\"
            \"\${_app}/Contents/MacOS/m2host\"
            \"${_imsa_dylib_dirs}\")
        # install_name_tool invalidates signatures, and arm64 refuses unsigned
        # code. An ad-hoc signature keeps the bundle launchable; without a
        # Developer ID, Gatekeeper still asks the user to approve it once.
        execute_process(
            COMMAND codesign --force --deep --sign - \"\${_app}\"
            RESULT_VARIABLE _sign)
        if(NOT _sign EQUAL 0)
            message(FATAL_ERROR \"codesign failed for \${_app}\")
        endif()
    ")
    set(CPACK_GENERATOR DragNDrop)
    set(CPACK_DMG_VOLUME_NAME "IMSA WCR M2 Recomp ${PROJECT_VERSION}")

# -- Linux and other Unix: portable folder with one script at the top ----------

else()
    set(_imsa_rpath "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    set_target_properties(m2host m2launcher PROPERTIES
        INSTALL_RPATH "${_imsa_rpath}")
    install(TARGETS m2host m2launcher RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

    # SDL3 3.4 is not packaged by most distributions, so ship it beside the
    # executables. The other libraries come from the system (the AppImage
    # bundles those as well).
    if(TARGET SDL3::SDL3-shared)
        install(IMPORTED_RUNTIME_ARTIFACTS SDL3::SDL3-shared
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    endif()

    # FFmpeg is the LGPL build, so it travels as shared libraries the user
    # can replace. Symlinks (libavcodec.so -> .so.N) are preserved.
    if(IMSA_FFMPEG_ROOT)
        install(DIRECTORY "${IMSA_FFMPEG_ROOT}/lib/"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            FILES_MATCHING PATTERN "libav*.so*" PATTERN "libsw*.so*")
        set(_imsa_ffmpeg_source "${IMSA_FFMPEG_ROOT}/FFMPEG_SOURCE.txt")
    else()
        set(_imsa_ffmpeg_source "")
    endif()

    install(FILES "${CMAKE_SOURCE_DIR}/packaging/imsa-wcr-m2-recomp.desktop"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/packaging/icons/hicolor"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons")

    # cmake --install (system installs and the AppImage): notices in share/doc.
    install(FILES ${_imsa_notices} ${_imsa_ffmpeg_source}
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/imsa-wcr-m2-recomp"
        COMPONENT system_docs)
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/third_party/licenses"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/imsa-wcr-m2-recomp"
        COMPONENT system_docs
        FILES_MATCHING PATTERN "*.txt")

    # tar.gz only: the portable layout, including the marker that turns on
    # portable storage. A system install must never get these.
    install(FILES ${_imsa_notices} ${_imsa_ffmpeg_source}
        DESTINATION licenses
        COMPONENT portable EXCLUDE_FROM_ALL)
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/third_party/licenses/"
        DESTINATION licenses
        COMPONENT portable EXCLUDE_FROM_ALL
        FILES_MATCHING PATTERN "*.txt")
    install(PROGRAMS "${CMAKE_SOURCE_DIR}/packaging/linux/imsa-wcr-m2-recomp"
        DESTINATION .
        COMPONENT portable EXCLUDE_FROM_ALL)
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/README.txt"
        DESTINATION .
        COMPONENT portable EXCLUDE_FROM_ALL)
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/portable/userdata-README.txt"
        DESTINATION userdata RENAME README.txt
        COMPONENT portable EXCLUDE_FROM_ALL)
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/portable/portable.txt"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT portable EXCLUDE_FROM_ALL)

    set(CPACK_INSTALL_CMAKE_PROJECTS
        "${CMAKE_BINARY_DIR};${PROJECT_NAME};Unspecified;/"
        "${CMAKE_BINARY_DIR};${PROJECT_NAME};portable;/")
    set(CPACK_GENERATOR TGZ)
endif()

# -- CPack --------------------------------------------------------------------

set(CPACK_PACKAGE_NAME "imsa-wcr-m2-recomp")
set(CPACK_PACKAGE_VENDOR "imsa-wcr-m2-recomp")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_FILE_NAME
    "imsa-wcr-m2-recomp-${PROJECT_VERSION}-${_imsa_os}-${_imsa_arch}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
# A top folder keeps an extracted archive together. In a DMG it would hide the
# app inside a folder, next to CPack's temporary image.
if(APPLE)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
else()
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
endif()
set(CPACK_ARCHIVE_THREADS 0)

include(CPack)
