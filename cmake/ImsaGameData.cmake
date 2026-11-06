# Resolves the user's game data into IMSA_GAME_DIR (a folder containing
# driving/game) and verifies it.
#
#   -DIMSA_DISC=<iso>        hash the ISO, extract it into the build tree
#   -DIMSA_DATA_DIR=<dir>    use an already extracted folder, hash driving/game
#
# Sets IMSA_GAME_DIR and IMSA_GAME_ELF in the caller's scope. Does nothing
# when neither option is given.

set(IMSA_ISO_SHA256 "b34e30635435b13c080e945c6343aa8f6920afc278eef2669970a7632c066835")
set(IMSA_ISO_SIZE 671399936)
if(NOT DEFINED IMSA_GAME_ELF_SHA256)
    set(IMSA_GAME_ELF_SHA256 "bb91fcd900424e0894755153b850892b39a704aa4493947b2aa632abaf290865")
endif()
if(NOT DEFINED IMSA_SHOOTER2D_ELF_SHA256)
    set(IMSA_SHOOTER2D_ELF_SHA256 "9955160c2279e40c303c9dc7997f900ef9eaf46bda3799d5f09550cbdb623d24")
endif()
if(NOT DEFINED IMSA_LAUNCHME_ELF_SHA256)
    set(IMSA_LAUNCHME_ELF_SHA256 "f1062d63ab9adb7db3b18b69e5170b799bcf6023e88800342208ce09937ecddf")
endif()
set(IMSA_DISC_EXTRACT_VERSION 2)

function(imsa_verify_companion_elf elf expected description)
    if(NOT EXISTS "${elf}")
        message(FATAL_ERROR "Game data is missing ${description}: ${elf}")
    endif()
    file(SHA256 "${elf}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "${description} does not match the supported disc (SHA-256 ${actual}).\n"
            "Use an unmodified copy of the supported IMSA presents World Championship "
            "Racing disc.")
    endif()
endfunction()

function(imsa_verify_game_elf elf)
    imsa_verify_companion_elf("${elf}" "${IMSA_GAME_ELF_SHA256}" "driving/game")
endfunction()

function(imsa_resolve_game_data)
    if(IMSA_DISC AND IMSA_DATA_DIR)
        message(FATAL_ERROR "Set only one of IMSA_DISC and IMSA_DATA_DIR.")
    endif()

    if(IMSA_DISC)
        if(NOT EXISTS "${IMSA_DISC}")
            message(FATAL_ERROR "IMSA_DISC does not exist: ${IMSA_DISC}")
        endif()
        file(SIZE "${IMSA_DISC}" iso_size)
        if(NOT iso_size EQUAL IMSA_ISO_SIZE)
            message(FATAL_ERROR
                "IMSA_DISC is not the supported disc (expected ${IMSA_ISO_SIZE} bytes, "
                "got ${iso_size}).")
        endif()

        # Hash the current image every configure. The marker only avoids
        # re-extraction; it is not a substitute for verifying the input.
        message(STATUS "Verifying ${IMSA_DISC}")
        file(SHA256 "${IMSA_DISC}" iso_hash)
        if(NOT iso_hash STREQUAL IMSA_ISO_SHA256)
            message(FATAL_ERROR
                "IMSA_DISC does not match the supported disc (SHA-256 ${iso_hash}).\n"
                "Use an unmodified copy of the supported disc image.")
        endif()

        set(extractor "${CMAKE_SOURCE_DIR}/tools/operafs.py")
        if(NOT EXISTS "${extractor}")
            message(FATAL_ERROR "Disc extractor is missing: ${extractor}")
        endif()
        file(SHA256 "${extractor}" extractor_hash)

        set(dest "${CMAKE_BINARY_DIR}/game-data")
        set(marker "${dest}/.imsa-disc")
        set(want_marker
            "${iso_hash}:${extractor_hash}:${IMSA_DISC_EXTRACT_VERSION}")

        set(extracted FALSE)
        if(EXISTS "${marker}")
            file(READ "${marker}" marked)
            string(STRIP "${marked}" marked)
            if(marked STREQUAL want_marker)
                # The marker records the disc and extractor, not the state of
                # the tree. A partially cleaned build/game-data still carries
                # it, so require the files the rest of this function verifies.
                if(EXISTS "${dest}/driving/game" AND
                   EXISTS "${dest}/shooter2d/shooter2d" AND
                   EXISTS "${dest}/LaunchMe.m2")
                    set(extracted TRUE)
                else()
                    message(STATUS
                        "Existing extraction at ${dest} is incomplete; extracting again")
                endif()
            endif()
        endif()

        if(extracted)
            message(STATUS "Using existing extraction at ${dest}")
        else()
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            message(STATUS "Extracting disc to ${dest}")
            file(REMOVE_RECURSE "${dest}")
            execute_process(
                COMMAND "${Python3_EXECUTABLE}" "${extractor}"
                        extract "${IMSA_DISC}" "${dest}"
                RESULT_VARIABLE rc
                ERROR_VARIABLE extract_err)
            if(NOT rc EQUAL 0)
                file(REMOVE_RECURSE "${dest}")
                message(FATAL_ERROR "Disc extraction failed.\n${extract_err}")
            endif()
            file(WRITE "${marker}" "${want_marker}\n")
        endif()

        imsa_verify_game_elf("${dest}/driving/game")
        imsa_verify_companion_elf("${dest}/shooter2d/shooter2d" "${IMSA_SHOOTER2D_ELF_SHA256}" "shooter2d/shooter2d")
        imsa_verify_companion_elf("${dest}/LaunchMe.m2" "${IMSA_LAUNCHME_ELF_SHA256}" "LaunchMe.m2")
        set(game_dir "${dest}")
    elseif(IMSA_DATA_DIR)
        set(game_dir "${IMSA_DATA_DIR}")
        imsa_verify_game_elf("${game_dir}/driving/game")
        imsa_verify_companion_elf("${game_dir}/shooter2d/shooter2d" "${IMSA_SHOOTER2D_ELF_SHA256}" "shooter2d/shooter2d")
        imsa_verify_companion_elf("${game_dir}/LaunchMe.m2" "${IMSA_LAUNCHME_ELF_SHA256}" "LaunchMe.m2")
    else()
        return()
    endif()

    set(IMSA_GAME_DIR "${game_dir}" PARENT_SCOPE)
    set(IMSA_GAME_ELF "${game_dir}/driving/game" PARENT_SCOPE)
endfunction()
