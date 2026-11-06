"""Tests for ImsaGenerate.cmake cache validation and missing chunk restoration."""
import json
import os
import subprocess
from pathlib import Path
import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
CMAKE_DIR = REPO_ROOT / "cmake"


def run_cmake_script(script_text: str, tmp_path: Path) -> subprocess.CompletedProcess:
    script_file = tmp_path / "run_test.cmake"
    script_file.write_text(script_text)
    return subprocess.run(
        ["cmake", "-P", str(script_file)],
        capture_output=True,
        text=True,
    )


def test_validate_cached_outputs_complete(tmp_path):
    """When all manifest outputs and chunks exist, validation succeeds."""
    out_dir = tmp_path / "generated"
    out_dir.mkdir()
    manifest_path = out_dir / ".manifest"

    chunks = ["game_text_00.c", "game_text_01.c"]
    files = [
        "game_data.c",
        "game_dispatch.c",
        "game_recomp.h",
        "recomp_report.json",
    ] + chunks

    for f in files:
        (out_dir / f).write_text("/* dummy */\n")

    report_content = {
        "chunks": chunks,
        "funcs": 240,
    }
    (out_dir / "recomp_report.json").write_text(json.dumps(report_content))

    manifest_path.write_text("\n".join(files) + "\n")

    script = f"""
    cmake_minimum_required(VERSION 3.24)
    list(APPEND CMAKE_MODULE_PATH "{CMAKE_DIR.as_posix()}")
    include(ImsaGenerate)

    _imsa_validate_cached_outputs("{out_dir.as_posix()}" "{manifest_path.as_posix()}" "" "" is_valid)
    if(NOT is_valid)
        message(FATAL_ERROR "Expected cache to be valid")
    endif()
    message(STATUS "CACHE_VALID=TRUE")
    """
    res = run_cmake_script(script, tmp_path)
    assert res.returncode == 0, res.stderr
    assert "CACHE_VALID=TRUE" in res.stdout


def test_validate_cached_outputs_rejects_missing_chunk(tmp_path):
    """Deleting a generated function chunk causes cache validation to fail."""
    out_dir = tmp_path / "generated"
    out_dir.mkdir()
    manifest_path = out_dir / ".manifest"

    chunks = ["game_text_00.c", "game_text_01.c"]
    files = [
        "game_data.c",
        "game_dispatch.c",
        "game_recomp.h",
        "recomp_report.json",
    ] + chunks

    for f in files:
        (out_dir / f).write_text("/* dummy */\n")

    report_content = {
        "chunks": chunks,
        "funcs": 240,
    }
    (out_dir / "recomp_report.json").write_text(json.dumps(report_content))
    manifest_path.write_text("\n".join(files) + "\n")

    # Delete one chunk
    (out_dir / "game_text_01.c").unlink()

    script = f"""
    cmake_minimum_required(VERSION 3.24)
    list(APPEND CMAKE_MODULE_PATH "{CMAKE_DIR.as_posix()}")
    include(ImsaGenerate)

    _imsa_validate_cached_outputs("{out_dir.as_posix()}" "{manifest_path.as_posix()}" "" "" is_valid)
    if(is_valid)
        message(FATAL_ERROR "Expected cache to be invalid after deleting chunk")
    endif()
    message(STATUS "CACHE_INVALID_DETECTED=TRUE")
    """
    res = run_cmake_script(script, tmp_path)
    assert res.returncode == 0, res.stderr
    assert "CACHE_INVALID_DETECTED=TRUE" in res.stdout


def test_validate_cached_outputs_rejects_missing_manifest(tmp_path):
    """If the manifest is missing, cache validation fails."""
    out_dir = tmp_path / "generated"
    out_dir.mkdir()
    manifest_path = out_dir / ".manifest"

    chunks = ["game_text_00.c"]
    files = [
        "game_data.c",
        "game_dispatch.c",
        "game_recomp.h",
        "recomp_report.json",
    ] + chunks

    for f in files:
        (out_dir / f).write_text("/* dummy */\n")

    report_content = {"chunks": chunks}
    (out_dir / "recomp_report.json").write_text(json.dumps(report_content))
    # Do NOT create manifest_path

    script = f"""
    cmake_minimum_required(VERSION 3.24)
    list(APPEND CMAKE_MODULE_PATH "{CMAKE_DIR.as_posix()}")
    include(ImsaGenerate)

    _imsa_validate_cached_outputs("{out_dir.as_posix()}" "{manifest_path.as_posix()}" "" "" is_valid)
    if(is_valid)
        message(FATAL_ERROR "Expected cache to be invalid when manifest is absent")
    endif()
    message(STATUS "MANIFEST_ABSENT_DETECTED=TRUE")
    """
    res = run_cmake_script(script, tmp_path)
    assert res.returncode == 0, res.stderr
    assert "MANIFEST_ABSENT_DETECTED=TRUE" in res.stdout


def test_validate_cached_outputs_rejects_missing_header(tmp_path):
    """If a generated header is missing, cache validation fails."""
    out_dir = tmp_path / "generated"
    out_dir.mkdir()
    manifest_path = out_dir / ".manifest"

    chunks = ["game_text_00.c"]
    files = [
        "game_data.c",
        "game_dispatch.c",
        "game_recomp.h",
        "recomp_report.json",
    ] + chunks

    for f in files:
        (out_dir / f).write_text("/* dummy */\n")

    report_content = {"chunks": chunks}
    (out_dir / "recomp_report.json").write_text(json.dumps(report_content))
    manifest_path.write_text("\n".join(files) + "\n")

    # Delete master header
    (out_dir / "game_recomp.h").unlink()

    script = f"""
    cmake_minimum_required(VERSION 3.24)
    list(APPEND CMAKE_MODULE_PATH "{CMAKE_DIR.as_posix()}")
    include(ImsaGenerate)

    _imsa_validate_cached_outputs("{out_dir.as_posix()}" "{manifest_path.as_posix()}" "" "" is_valid)
    if(is_valid)
        message(FATAL_ERROR "Expected cache to be invalid when game_recomp.h is absent")
    endif()
    message(STATUS "HEADER_ABSENT_DETECTED=TRUE")
    """
    res = run_cmake_script(script, tmp_path)
    assert res.returncode == 0, res.stderr
    assert "HEADER_ABSENT_DETECTED=TRUE" in res.stdout


def test_validate_cached_outputs_rejects_missing_companion_chunk(tmp_path):
    """If companion image is present and its chunk is missing, cache validation fails."""
    out_dir = tmp_path / "generated"
    out_dir.mkdir()
    manifest_path = out_dir / ".manifest"

    game_chunks = ["game_text_00.c"]
    s2_chunks = ["shooter2d_text_00.c"]
    files = [
        "game_data.c",
        "game_dispatch.c",
        "game_recomp.h",
        "recomp_report.json",
        "shooter2d_data.c",
        "shooter2d_dispatch.c",
        "shooter2d_recomp.h",
        "shooter2d_report.json",
    ] + game_chunks + s2_chunks

    for f in files:
        (out_dir / f).write_text("/* dummy */\n")

    (out_dir / "recomp_report.json").write_text(json.dumps({"chunks": game_chunks}))
    (out_dir / "shooter2d_report.json").write_text(json.dumps({"chunks": s2_chunks}))
    manifest_path.write_text("\n".join(files) + "\n")

    # Delete companion chunk
    (out_dir / "shooter2d_text_00.c").unlink()

    fake_s2_elf = tmp_path / "shooter2d"
    fake_s2_elf.write_bytes(b"s2")

    script = f"""
    cmake_minimum_required(VERSION 3.24)
    list(APPEND CMAKE_MODULE_PATH "{CMAKE_DIR.as_posix()}")
    include(ImsaGenerate)

    _imsa_validate_cached_outputs("{out_dir.as_posix()}" "{manifest_path.as_posix()}" "{fake_s2_elf.as_posix()}" "" is_valid)
    if(is_valid)
        message(FATAL_ERROR "Expected cache to be invalid when companion chunk is missing")
    endif()
    message(STATUS "COMPANION_CHUNK_ABSENT_DETECTED=TRUE")
    """
    res = run_cmake_script(script, tmp_path)
    assert res.returncode == 0, res.stderr
    assert "COMPANION_CHUNK_ABSENT_DETECTED=TRUE" in res.stdout


def test_reconfiguration_restores_deleted_chunk(tmp_path):
    """End-to-end: deleting a generated function chunk triggers regeneration and restores the file."""
    work_dir = tmp_path / "work"
    work_dir.mkdir()
    source_dir = work_dir / "src"
    build_dir = work_dir / "build"
    source_dir.mkdir()
    build_dir.mkdir()

    game_dir = work_dir / "game_data"
    (game_dir / "driving").mkdir(parents=True)
    (game_dir / "shooter2d").mkdir(parents=True)

    game_elf = game_dir / "driving" / "game"
    s2_elf = game_dir / "shooter2d" / "shooter2d"
    lm_elf = game_dir / "LaunchMe.m2"
    game_elf.write_bytes(b"mock game binary")
    s2_elf.write_bytes(b"mock shooter2d binary")
    lm_elf.write_bytes(b"mock launchme binary")

    # Create mock transpiler
    recompiler_dir = source_dir / "recompiler"
    recompiler_dir.mkdir()
    mock_transpiler = recompiler_dir / "transpile.py"
    mock_transpiler_code = """import argparse, os, json
parser = argparse.ArgumentParser()
parser.add_argument("--elf", required=True)
parser.add_argument("--name", default="game")
parser.add_argument("--text-base")
parser.add_argument("--import-table-base")
parser.add_argument("--profile")
parser.add_argument("--chunk", type=int, default=120)
parser.add_argument("--out", required=True)
args = parser.parse_args()

os.makedirs(args.out, exist_ok=True)
prefix = args.name
with open(os.path.join(args.out, f"{prefix}_data.c"), "w") as f:
    f.write("/* data */\\n")
with open(os.path.join(args.out, f"{prefix}_dispatch.c"), "w") as f:
    f.write("/* dispatch */\\n")
with open(os.path.join(args.out, f"{prefix}_recomp.h"), "w") as f:
    f.write("/* header */\\n")

chunks = [f"{prefix}_text_00.c", f"{prefix}_text_01.c"]
for c in chunks:
    with open(os.path.join(args.out, c), "w") as f:
        f.write(f"/* {c} */\\n")

report_name = "recomp_report.json" if prefix == "game" else f"{prefix}_report.json"
with open(os.path.join(args.out, report_name), "w") as f:
    json.dump({"chunks": chunks, "funcs": 240}, f)
"""
    mock_transpiler.write_text(mock_transpiler_code)

    import hashlib
    game_hash = hashlib.sha256(b"mock game binary").hexdigest()
    s2_hash = hashlib.sha256(b"mock shooter2d binary").hexdigest()
    lm_hash = hashlib.sha256(b"mock launchme binary").hexdigest()

    cmakelists = f"""cmake_minimum_required(VERSION 3.24)
project(test_generate_cache C)
list(APPEND CMAKE_MODULE_PATH "{CMAKE_DIR.as_posix()}")

set(IMSA_GAME_DIR "{game_dir.as_posix()}")
set(IMSA_GAME_ELF "{game_elf.as_posix()}")
set(IMSA_GAME_ELF_SHA256 "{game_hash}")
set(IMSA_SHOOTER2D_ELF_SHA256 "{s2_hash}")
set(IMSA_LAUNCHME_ELF_SHA256 "{lm_hash}")

include(ImsaGenerate)
imsa_generate_guest()

message(STATUS "GENERATED_SOURCES_COUNT: ${{IMSA_GENERATED_SOURCES}}")
"""
    (source_dir / "CMakeLists.txt").write_text(cmakelists)

    # 1. Initial configure: generates all code
    p1 = subprocess.run(
        ["cmake", "-S", str(source_dir), "-B", str(build_dir)],
        capture_output=True,
        text=True,
    )
    assert p1.returncode == 0, p1.stderr
    assert "Generating guest code" in p1.stdout
    chunk0 = build_dir / "generated" / "game_text_00.c"
    chunk1 = build_dir / "generated" / "game_text_01.c"
    manifest = build_dir / "generated" / ".manifest"
    stamp = build_dir / "generated" / ".stamp"
    assert chunk0.exists()
    assert chunk1.exists()
    assert manifest.exists()
    assert stamp.exists()
    assert "game_text_00.c" in manifest.read_text()
    assert "game_text_01.c" in manifest.read_text()

    # 2. Second configure without changes: cache is valid, generator does not run
    p2 = subprocess.run(
        ["cmake", "-S", str(source_dir), "-B", str(build_dir)],
        capture_output=True,
        text=True,
    )
    assert p2.returncode == 0, p2.stderr
    assert "Generating guest code" not in p2.stdout

    # 3. Delete a generated function chunk
    chunk1.unlink()
    assert not chunk1.exists()

    # 4. Reconfigure: must detect missing chunk, regenerate, and restore chunk1
    p3 = subprocess.run(
        ["cmake", "-S", str(source_dir), "-B", str(build_dir)],
        capture_output=True,
        text=True,
    )
    assert p3.returncode == 0, p3.stderr
    assert "Generating guest code" in p3.stdout
    assert chunk1.exists()
    assert "game_text_01.c" in p3.stdout
