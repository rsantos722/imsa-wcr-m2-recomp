# Native launcher: disc import, save management, settings, and optional wx UI.

option(IMSA_BUILD_LAUNCHER
       "Build the wxWidgets launcher UI when available (disc/save libraries always build for tests)" ON)
option(IMSA_LAUNCHER_FETCH_WXWIDGETS
       "Download and compile wxWidgets from source instead of using precompiled package" OFF)

# The generated manifest keeps installed launchers independent of Python.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(IMSA_LAUNCHER_MANIFEST
    "${CMAKE_CURRENT_BINARY_DIR}/launcher_disc_manifest.inc")
add_custom_command(
    OUTPUT "${IMSA_LAUNCHER_MANIFEST}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/launcher_disc_manifest.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/disc-manifest.json"
            "${IMSA_LAUNCHER_MANIFEST}"
    DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/launcher_disc_manifest.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/disc-manifest.json"
    VERBATIM)

add_library(m2_launcher_disc STATIC
    disc_import.cpp
    "${IMSA_LAUNCHER_MANIFEST}"
)
target_include_directories(m2_launcher_disc PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
)

add_library(m2_launcher_saves STATIC
    save_data.cpp
)
target_include_directories(m2_launcher_saves PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/host/core"
)
target_link_libraries(m2_launcher_saves PUBLIC m2_host_platform)

if(MSVC)
    target_compile_options(m2_launcher_disc PRIVATE /W4)
    target_compile_options(m2_launcher_saves PRIVATE /W4)
else()
    target_compile_options(m2_launcher_disc PRIVATE -Wall -Wextra)
    target_compile_options(m2_launcher_saves PRIVATE -Wall -Wextra)
endif()

# Developer tool: dump a memimage car model (no game data in the build).
if(IMSA_BUILD_TESTS)
    add_executable(m2_launcher_assets_dump
        asset_texture.cpp
        asset_model.cpp
        asset_dump.cpp
    )
    target_include_directories(m2_launcher_assets_dump PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()

if(IMSA_BUILD_LAUNCHER)
    set(IMSA_HAVE_WXWIDGETS FALSE)
    if(NOT IMSA_LAUNCHER_FETCH_WXWIDGETS)
        find_package(wxWidgets COMPONENTS core base QUIET)
        if(wxWidgets_FOUND)
            include(${wxWidgets_USE_FILE})
            set(IMSA_LAUNCHER_WX_LIBRARIES ${wxWidgets_LIBRARIES})
            set(IMSA_HAVE_WXWIDGETS TRUE)
            message(STATUS "m2launcher: using system precompiled wxWidgets (${wxWidgets_VERSION_STRING})")
        endif()
    endif()

    if(NOT IMSA_HAVE_WXWIDGETS AND IMSA_LAUNCHER_FETCH_WXWIDGETS)
        include(FetchContent)
        # Build a static, minimal wxWidgets so the release launcher needs no
        # external wxWidgets installation.
        foreach(wx_off
                wxBUILD_SHARED wxBUILD_INSTALL wxBUILD_SAMPLES wxBUILD_TESTS
                wxBUILD_DEMOS wxBUILD_BENCHMARKS wxUSE_WEBVIEW wxUSE_STC
                wxUSE_RICHTEXT wxUSE_AUI wxUSE_PROPGRID wxUSE_RIBBON
                wxUSE_MEDIACTRL wxUSE_OPENGL wxUSE_XRC wxUSE_XML
                wxUSE_WEBREQUEST wxUSE_SECRETSTORE wxUSE_LIBNOTIFY wxUSE_LIBTIFF
                wxUSE_DEBUGREPORT wxUSE_QA wxUSE_SOUND)
            set(${wx_off} OFF CACHE BOOL "" FORCE)
        endforeach()
        set(wxBUILD_LOCALES OFF CACHE STRING "" FORCE)
        set(wxBUILD_PRECOMP OFF CACHE STRING "" FORCE)
        FetchContent_Declare(wxwidgets
            URL https://github.com/wxWidgets/wxWidgets/releases/download/v3.2.8/wxWidgets-3.2.8.tar.bz2
            URL_HASH SHA256=c74784904109d7229e6894c85cfa068f1106a4a07c144afd78af41f373ee0fe6
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(wxwidgets)
        set(IMSA_LAUNCHER_WX_LIBRARIES wx::core wx::base)
        set(IMSA_HAVE_WXWIDGETS TRUE)
    endif()

    if(NOT IMSA_HAVE_WXWIDGETS)
        message(STATUS "m2launcher: wxWidgets not found; skipping UI application.")
        message(STATUS "  To install precompiled wxWidgets without compiling:")
        message(STATUS "    macOS:   brew install wxwidgets")
        message(STATUS "    Linux:   sudo apt install libwxgtk3.2-dev")
        message(STATUS "    Windows: pacman -S mingw-w64-ucrt-x86_64-wxwidgets3.2")
        message(STATUS "  Or configure with -DIMSA_LAUNCHER_FETCH_WXWIDGETS=ON to compile from source.")
    else()
        set(IMSA_LAUNCHER_ICON_DIR "${CMAKE_SOURCE_DIR}/packaging/icons")
        set(IMSA_LAUNCHER_ICON_PNG "${IMSA_LAUNCHER_ICON_DIR}/imsa-wcr-m2-recomp-256.png")
        set(IMSA_LAUNCHER_ICON_ICO "${IMSA_LAUNCHER_ICON_DIR}/imsa-wcr-m2-recomp.ico")
        set(IMSA_LAUNCHER_ICON_ICNS "${IMSA_LAUNCHER_ICON_DIR}/imsa-wcr-m2-recomp.icns")
        # Read by cmake/ImsaPackaging.cmake in the parent scope.
        set(IMSA_LAUNCHER_ICON_PNG "${IMSA_LAUNCHER_ICON_PNG}" PARENT_SCOPE)

        add_executable(m2launcher
            main.cpp
            app_icon.cpp
            asset_model.cpp
            asset_raster.cpp
            asset_texture.cpp
        )
        target_include_directories(m2launcher PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_SOURCE_DIR}/host/core"
            "${CMAKE_SOURCE_DIR}/host/input"
            "${CMAKE_SOURCE_DIR}/host/diagnostics"
            "${CMAKE_SOURCE_DIR}/runtime/include"
            "${CMAKE_SOURCE_DIR}/shims"
        )
        target_link_libraries(m2launcher PRIVATE
            SDL3::SDL3
            ${IMSA_LAUNCHER_WX_LIBRARIES}
            m2_host_input
            m2_launcher_disc
            m2_launcher_saves
        )

        if(IMSA_BUILD_TESTS)
            add_executable(m2_launcher_ui_text_test
                "${CMAKE_SOURCE_DIR}/tests/unit/launcher/ui_text_test.cpp")
            target_include_directories(m2_launcher_ui_text_test PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}")
            target_link_libraries(m2_launcher_ui_text_test PRIVATE
                ${IMSA_LAUNCHER_WX_LIBRARIES})
            add_test(NAME m2_launcher_ui_text COMMAND m2_launcher_ui_text_test)
        endif()

        if(APPLE)
            enable_language(OBJCXX)
            target_sources(m2launcher PRIVATE app_icon_macos.mm)
            set_source_files_properties(app_icon_macos.mm
                PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        elseif(WIN32)
            configure_file(
                "${CMAKE_SOURCE_DIR}/packaging/m2launcher.rc.in"
                "${CMAKE_CURRENT_BINARY_DIR}/m2launcher.rc" @ONLY)
            target_sources(m2launcher PRIVATE
                "${CMAKE_CURRENT_BINARY_DIR}/m2launcher.rc")

            # Windows only finds DLLs beside an executable, so the real
            # launcher lives in bin with them. The package's top folder gets
            # this small DLL-free program that starts it.
            configure_file(
                "${CMAKE_SOURCE_DIR}/packaging/windows/launcher_stub.rc.in"
                "${CMAKE_CURRENT_BINARY_DIR}/launcher_stub.rc" @ONLY)
            add_executable(m2launcher_stub WIN32
                "${CMAKE_SOURCE_DIR}/packaging/windows/launcher_stub.c"
                "${CMAKE_CURRENT_BINARY_DIR}/launcher_stub.rc")
            set_target_properties(m2launcher_stub PROPERTIES
                OUTPUT_NAME "IMSA WCR M2 Recomp")
            if(MINGW)
                target_link_options(m2launcher_stub PRIVATE -static)
            endif()
        endif()

        set_target_properties(m2launcher PROPERTIES WIN32_EXECUTABLE TRUE)
        if(MSVC)
            target_link_options(m2launcher PRIVATE /ENTRY:mainCRTStartup)
            # wx/msw/wx.rc embeds the themed, DPI-aware application manifest.
            # Do not also generate the linker's default manifest.
            target_link_options(m2launcher PRIVATE /MANIFEST:NO)
        endif()

        add_custom_command(TARGET m2launcher POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${IMSA_LAUNCHER_ICON_PNG}"
                    "$<TARGET_FILE_DIR:m2launcher>/imsa-wcr-m2-recomp-256.png"
            COMMENT "Copying launcher icon"
            VERBATIM)
        set_property(TARGET m2launcher APPEND PROPERTY
            LINK_DEPENDS "${IMSA_LAUNCHER_ICON_PNG}")

        if(APPLE)
            # Single-config generators use the
            # build/launcher/IMSA WCR M2 Recomp Launcher.app path. Multi-config
            # generators (Xcode, Ninja Multi-Config) produce one bundle per
            # configuration so Debug and Release hosts cannot overwrite each
            # other.
            if(CMAKE_CONFIGURATION_TYPES)
                set(IMSA_LAUNCHER_APP_DIR
                    "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/IMSA WCR M2 Recomp Launcher.app")
            else()
                set(IMSA_LAUNCHER_APP_DIR
                    "${CMAKE_CURRENT_BINARY_DIR}/IMSA WCR M2 Recomp Launcher.app")
            endif()
            set(IMSA_LAUNCHER_APP_DIR "${IMSA_LAUNCHER_APP_DIR}" PARENT_SCOPE)
            set(IMSA_LAUNCHER_VERSION "${PROJECT_VERSION}")
            configure_file(
                "${CMAKE_SOURCE_DIR}/packaging/Info.plist.in"
                "${CMAKE_CURRENT_BINARY_DIR}/m2launcher-Info.plist" @ONLY)
            add_custom_command(TARGET m2launcher POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E make_directory
                        "${IMSA_LAUNCHER_APP_DIR}/Contents/MacOS"
                        "${IMSA_LAUNCHER_APP_DIR}/Contents/Resources"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:m2launcher>"
                        "${IMSA_LAUNCHER_APP_DIR}/Contents/MacOS/m2launcher"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${IMSA_LAUNCHER_ICON_ICNS}"
                        "${IMSA_LAUNCHER_APP_DIR}/Contents/Resources/imsa-wcr-m2-recomp.icns"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${IMSA_LAUNCHER_ICON_PNG}"
                        "${IMSA_LAUNCHER_APP_DIR}/Contents/Resources/imsa-wcr-m2-recomp-256.png"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${CMAKE_CURRENT_BINARY_DIR}/m2launcher-Info.plist"
                        "${IMSA_LAUNCHER_APP_DIR}/Contents/Info.plist"
                COMMENT "Staging macOS launcher app bundle"
                VERBATIM)
            # m2host is a different target, so a POST_BUILD copy here would
            # only run when the launcher relinks. Stage it from an ALL target
            # that depends on m2host instead, and make m2launcher depend on
            # that staging target.
            include(ImsaLauncherBundle)
            if(TARGET m2host)
                imsa_add_launcher_bundle_host(
                    TARGET m2launcher_bundle_host
                    HOST_TARGET m2host
                    BUNDLE_DIR "${IMSA_LAUNCHER_APP_DIR}")
                add_dependencies(m2launcher m2launcher_bundle_host)
            endif()
        endif()
    endif()
endif()
