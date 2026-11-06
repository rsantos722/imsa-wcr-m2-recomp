# Runs the recompiler on the verified guest image and exposes the result.
#
# Requires IMSA_GAME_ELF (see ImsaGameData.cmake). Generated code goes to
# ${CMAKE_BINARY_DIR}/generated and is never committed. Generation runs at
# configure time because the set of emitted files is only known afterwards.
#
# The freshness key covers the source image, both companion images, the
# transpiler, both CMake modules, and every generation flag. The stamp and
# output manifest are written only after all requested images generate
# successfully. Reconfiguration validates both key freshness and output
# completeness against the manifest, restoring any missing files.
#
# Sets IMSA_GENERATED_DIR, IMSA_GENERATED_SOURCES, and IMSA_RECOMPILER_PYTHON
# (an interpreter with capstone, for build steps that load guest images) in
# the caller's scope.

set(IMSA_GENERATE_MODULE_FILE "${CMAKE_CURRENT_LIST_FILE}")
file(SHA256 "${IMSA_GENERATE_MODULE_FILE}" IMSA_GENERATE_MODULE_SHA256)
set(IMSA_GAMEDATA_MODULE_FILE "${CMAKE_CURRENT_LIST_DIR}/ImsaGameData.cmake")
if(EXISTS "${IMSA_GAMEDATA_MODULE_FILE}")
    file(SHA256 "${IMSA_GAMEDATA_MODULE_FILE}" IMSA_GAMEDATA_MODULE_SHA256)
else()
    set(IMSA_GAMEDATA_MODULE_SHA256 "missing")
endif()

if(NOT DEFINED IMSA_GAME_ELF_SHA256)
    set(IMSA_GAME_ELF_SHA256 "bb91fcd900424e0894755153b850892b39a704aa4493947b2aa632abaf290865")
endif()
if(NOT DEFINED IMSA_SHOOTER2D_ELF_SHA256)
    set(IMSA_SHOOTER2D_ELF_SHA256 "9955160c2279e40c303c9dc7997f900ef9eaf46bda3799d5f09550cbdb623d24")
endif()
if(NOT DEFINED IMSA_LAUNCHME_ELF_SHA256)
    set(IMSA_LAUNCHME_ELF_SHA256 "f1062d63ab9adb7db3b18b69e5170b799bcf6023e88800342208ce09937ecddf")
endif()

function(_imsa_require_generated path description)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Guest code generation produced no ${description}: ${path}")
    endif()
endfunction()

function(_imsa_validate_cached_outputs out_dir manifest_path shooter2d_elf launchme_elf out_valid_var)
    set(${out_valid_var} FALSE PARENT_SCOPE)

    if(NOT EXISTS "${manifest_path}")
        return()
    endif()

    file(STRINGS "${manifest_path}" _cached_files)
    if(NOT _cached_files)
        return()
    endif()

    foreach(_f IN LISTS _cached_files)
        string(STRIP "${_f}" _f)
        if(_f AND NOT EXISTS "${out_dir}/${_f}")
            return()
        endif()
    endforeach()

    foreach(_required IN ITEMS
            "game_data.c"
            "game_dispatch.c"
            "game_recomp.h"
            "recomp_report.json")
        if(NOT EXISTS "${out_dir}/${_required}")
            return()
        endif()
    endforeach()

    file(READ "${out_dir}/recomp_report.json" _report_json)
    string(JSON _chunks_len ERROR_VARIABLE _json_err LENGTH "${_report_json}" "chunks")
    if(_json_err OR _chunks_len EQUAL 0)
        return()
    endif()
    math(EXPR _max_idx "${_chunks_len} - 1")
    foreach(_idx RANGE 0 ${_max_idx})
        string(JSON _chunk_name GET "${_report_json}" "chunks" ${_idx})
        if(NOT EXISTS "${out_dir}/${_chunk_name}")
            return()
        endif()
    endforeach()

    if(EXISTS "${shooter2d_elf}")
        foreach(_required IN ITEMS
                "shooter2d_data.c"
                "shooter2d_dispatch.c"
                "shooter2d_recomp.h"
                "shooter2d_report.json")
            if(NOT EXISTS "${out_dir}/${_required}")
                return()
            endif()
        endforeach()

        file(READ "${out_dir}/shooter2d_report.json" _s2_report_json)
        string(JSON _s2_len ERROR_VARIABLE _s2_err LENGTH "${_s2_report_json}" "chunks")
        if(_s2_err OR _s2_len EQUAL 0)
            return()
        endif()
        math(EXPR _s2_max "${_s2_len} - 1")
        foreach(_idx RANGE 0 ${_s2_max})
            string(JSON _chunk_name GET "${_s2_report_json}" "chunks" ${_idx})
            if(NOT EXISTS "${out_dir}/${_chunk_name}")
                return()
            endif()
        endforeach()
    endif()

    if(EXISTS "${launchme_elf}")
        foreach(_required IN ITEMS
                "launchme_data.c"
                "launchme_dispatch.c"
                "launchme_recomp.h"
                "launchme_report.json")
            if(NOT EXISTS "${out_dir}/${_required}")
                return()
            endif()
        endforeach()

        file(READ "${out_dir}/launchme_report.json" _lm_report_json)
        string(JSON _lm_len ERROR_VARIABLE _lm_err LENGTH "${_lm_report_json}" "chunks")
        if(_lm_err OR _lm_len EQUAL 0)
            return()
        endif()
        math(EXPR _lm_max "${_lm_len} - 1")
        foreach(_idx RANGE 0 ${_lm_max})
            string(JSON _chunk_name GET "${_lm_report_json}" "chunks" ${_idx})
            if(NOT EXISTS "${out_dir}/${_chunk_name}")
                return()
            endif()
        endforeach()
    endif()

    file(GLOB _c_sources "${out_dir}/*.c")
    if(NOT _c_sources)
        return()
    endif()

    set(${out_valid_var} TRUE PARENT_SCOPE)
endfunction()

function(imsa_generate_guest)
    find_package(Python3 COMPONENTS Interpreter)
    set(_py_candidates "")
    if(Python3_EXECUTABLE)
        list(APPEND _py_candidates "${Python3_EXECUTABLE}")
    endif()
    file(TO_CMAKE_PATH "$ENV{PATH}" _path_dirs)
    foreach(_d IN LISTS _path_dirs)
        if(EXISTS "${_d}/python3")
            list(APPEND _py_candidates "${_d}/python3")
        elseif(EXISTS "${_d}/python3.exe")
            list(APPEND _py_candidates "${_d}/python3.exe")
        endif()
    endforeach()

    set(Python3_WITH_CAPSTONE "")
    foreach(_cand IN LISTS _py_candidates)
        execute_process(
            COMMAND "${_cand}" -c "import capstone"
            RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
        if(rc EQUAL 0)
            set(Python3_WITH_CAPSTONE "${_cand}")
            break()
        endif()
    endforeach()

    if(NOT Python3_WITH_CAPSTONE)
        message(FATAL_ERROR
            "The recompiler needs capstone: python3 -m pip install capstone")
    endif()
    set(Python3_EXECUTABLE "${Python3_WITH_CAPSTONE}")

    if(NOT EXISTS "${IMSA_GAME_ELF}")
        message(FATAL_ERROR "Game image is missing: ${IMSA_GAME_ELF}")
    endif()

    set(out "${CMAKE_BINARY_DIR}/generated")
    set(stamp "${out}/.stamp")
    set(manifest "${out}/.manifest")
    set(transpiler "${CMAKE_SOURCE_DIR}/recompiler/transpile.py")
    file(SHA256 "${transpiler}" transpiler_hash)

    set(game_text_base "0x100000")
    set(shooter2d_text_base "0x2c0000")
    set(launchme_text_base "0x2e0000")
    # Each image keeps its own import-slot window so the three modules can
    # share guest RAM without aliasing different (folio, ordinal) pairs.
    set(game_import_table_base "0x3f1000")
    set(shooter2d_import_table_base "0x3f2000")
    set(launchme_import_table_base "0x3f4000")
    set(chunk_size "120")
    set(shooter2d_elf "${IMSA_GAME_DIR}/shooter2d/shooter2d")
    set(launchme_elf "${IMSA_GAME_DIR}/LaunchMe.m2")

    # The runtime entry links both companion images into the static guest
    # build. A supported disc always carries them.
    if(NOT EXISTS "${shooter2d_elf}")
        message(FATAL_ERROR
            "Game data is missing the shooter2d image: ${shooter2d_elf}")
    endif()
    if(NOT EXISTS "${launchme_elf}")
        message(FATAL_ERROR
            "Game data is missing the LaunchMe image: ${launchme_elf}")
    endif()

    file(SHA256 "${IMSA_GAME_ELF}" game_hash)
    if(NOT game_hash STREQUAL IMSA_GAME_ELF_SHA256)
        message(FATAL_ERROR
            "driving/game does not match the supported disc (SHA-256 ${game_hash}).\n"
            "Use an unmodified copy of the supported IMSA presents World Championship "
            "Racing disc.")
    endif()

    file(SHA256 "${shooter2d_elf}" shooter2d_hash)
    if(NOT shooter2d_hash STREQUAL IMSA_SHOOTER2D_ELF_SHA256)
        message(FATAL_ERROR
            "shooter2d/shooter2d does not match the supported disc (SHA-256 ${shooter2d_hash}).\n"
            "Use an unmodified copy of the supported IMSA presents World Championship "
            "Racing disc.")
    endif()

    file(SHA256 "${launchme_elf}" launchme_hash)
    if(NOT launchme_hash STREQUAL IMSA_LAUNCHME_ELF_SHA256)
        message(FATAL_ERROR
            "LaunchMe.m2 does not match the supported disc (SHA-256 ${launchme_hash}).\n"
            "Use an unmodified copy of the supported IMSA presents World Championship "
            "Racing disc.")
    endif()

    set(game_args
        --elf "${IMSA_GAME_ELF}"
        --name game
        --text-base "${game_text_base}"
        --import-table-base "${game_import_table_base}"
        --profile driving-game
        --chunk "${chunk_size}"
        --out "${out}")
    set(shooter2d_args
        --elf "${shooter2d_elf}"
        --name shooter2d
        --text-base "${shooter2d_text_base}"
        --import-table-base "${shooter2d_import_table_base}"
        --profile generic
        --chunk "${chunk_size}"
        --out "${out}")
    set(launchme_args
        --elf "${launchme_elf}"
        --name launchme
        --text-base "${launchme_text_base}"
        --import-table-base "${launchme_import_table_base}"
        --profile generic
        --chunk "${chunk_size}"
        --out "${out}")

    set(_key "")
    string(APPEND _key "format=3\n")
    string(APPEND _key "game=${game_hash}\n")
    string(APPEND _key "shooter2d=${shooter2d_hash}\n")
    string(APPEND _key "launchme=${launchme_hash}\n")
    string(APPEND _key "transpiler=${transpiler_hash}\n")
    string(APPEND _key "generate_module=${IMSA_GENERATE_MODULE_SHA256}\n")
    string(APPEND _key "gamedata_module=${IMSA_GAMEDATA_MODULE_SHA256}\n")
    string(REPLACE ";" " " _game_args_text "${game_args}")
    string(REPLACE ";" " " _shooter2d_args_text "${shooter2d_args}")
    string(REPLACE ";" " " _launchme_args_text "${launchme_args}")
    string(APPEND _key "game_args=${_game_args_text}\n")
    string(APPEND _key "shooter2d_args=${_shooter2d_args_text}\n")
    string(APPEND _key "launchme_args=${_launchme_args_text}\n")
    string(STRIP "${_key}" want)

    set(have "")
    if(EXISTS "${stamp}")
        file(READ "${stamp}" have)
        string(STRIP "${have}" have)
    endif()

    set(_cache_valid FALSE)
    if(have STREQUAL want)
        _imsa_validate_cached_outputs("${out}" "${manifest}"
                                      "${shooter2d_elf}" "${launchme_elf}"
                                      _cache_valid)
    endif()

    if(NOT _cache_valid)
        message(STATUS "Generating guest code")
        file(REMOVE_RECURSE "${out}")

        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${transpiler}" ${game_args}
            RESULT_VARIABLE rc
            OUTPUT_QUIET
            ERROR_VARIABLE gen_err)
        if(NOT rc EQUAL 0)
            file(REMOVE_RECURSE "${out}")
            message(FATAL_ERROR
                "Guest code generation failed for driving/game.\n${gen_err}")
        endif()
        _imsa_require_generated("${out}/game_data.c" "game data image")
        _imsa_require_generated("${out}/game_dispatch.c" "game dispatch table")
        _imsa_require_generated("${out}/game_recomp.h" "game master header")
        _imsa_require_generated("${out}/recomp_report.json" "game translation report")

        if(EXISTS "${shooter2d_elf}")
            execute_process(
                COMMAND "${Python3_EXECUTABLE}" "${transpiler}" ${shooter2d_args}
                RESULT_VARIABLE rc_s2
                OUTPUT_QUIET
                ERROR_VARIABLE gen_err_s2)
            if(NOT rc_s2 EQUAL 0)
                file(REMOVE_RECURSE "${out}")
                message(FATAL_ERROR
                    "Guest code generation failed for shooter2d.\n${gen_err_s2}")
            endif()
            _imsa_require_generated("${out}/shooter2d_data.c"
                                    "shooter2d data image")
            _imsa_require_generated("${out}/shooter2d_dispatch.c"
                                    "shooter2d dispatch table")
            _imsa_require_generated("${out}/shooter2d_recomp.h"
                                    "shooter2d master header")
            _imsa_require_generated("${out}/shooter2d_report.json"
                                    "shooter2d translation report")
        endif()

        if(EXISTS "${launchme_elf}")
            execute_process(
                COMMAND "${Python3_EXECUTABLE}" "${transpiler}" ${launchme_args}
                RESULT_VARIABLE rc_lm
                OUTPUT_QUIET
                ERROR_VARIABLE gen_err_lm)
            if(NOT rc_lm EQUAL 0)
                file(REMOVE_RECURSE "${out}")
                message(FATAL_ERROR
                    "Guest code generation failed for launchme.\n${gen_err_lm}")
            endif()
            _imsa_require_generated("${out}/launchme_data.c"
                                    "launchme data image")
            _imsa_require_generated("${out}/launchme_dispatch.c"
                                    "launchme dispatch table")
            _imsa_require_generated("${out}/launchme_recomp.h"
                                    "launchme master header")
            _imsa_require_generated("${out}/launchme_report.json"
                                    "launchme translation report")
        endif()

        file(GLOB generated_sources "${out}/*.c")
        if(NOT generated_sources)
            file(REMOVE_RECURSE "${out}")
            message(FATAL_ERROR "Guest code generation produced no C sources.")
        endif()

        file(GLOB_RECURSE _generated_files RELATIVE "${out}" "${out}/*")
        list(SORT _generated_files)
        set(_manifest_content "")
        foreach(_f IN LISTS _generated_files)
            if(NOT _f MATCHES "(^|/)\\.")
                string(APPEND _manifest_content "${_f}\n")
            endif()
        endforeach()
        file(WRITE "${manifest}" "${_manifest_content}")
        file(WRITE "${stamp}" "${want}\n")
    endif()

    set(_deps "${transpiler}" "${IMSA_GENERATE_MODULE_FILE}"
              "${IMSA_GAMEDATA_MODULE_FILE}" "${IMSA_GAME_ELF}")
    foreach(_optional IN ITEMS "${shooter2d_elf}" "${launchme_elf}")
        if(EXISTS "${_optional}")
            list(APPEND _deps "${_optional}")
        endif()
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_deps})

    file(GLOB sources CONFIGURE_DEPENDS "${out}/*.c")
    set(IMSA_GENERATED_DIR "${out}" PARENT_SCOPE)
    set(IMSA_GENERATED_SOURCES "${sources}" PARENT_SCOPE)
    set(IMSA_RECOMPILER_PYTHON "${Python3_EXECUTABLE}" PARENT_SCOPE)
endfunction()
