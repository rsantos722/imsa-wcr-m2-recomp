"""Tests that companion executables are verified against supported-disc hashes."""
import hashlib
import os
import subprocess
import tempfile
from pathlib import Path
import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
CMAKE_DIR = REPO_ROOT / "cmake"

EXPECTED_GAME_HASH = "bb91fcd900424e0894755153b850892b39a704aa4493947b2aa632abaf290865"
EXPECTED_SHOOTER2D_HASH = "9955160c2279e40c303c9dc7997f900ef9eaf46bda3799d5f09550cbdb623d24"
EXPECTED_LAUNCHME_HASH = "f1062d63ab9adb7db3b18b69e5170b799bcf6023e88800342208ce09937ecddf"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run_cmake_script(script_text: str, tmp_path: Path) -> subprocess.CompletedProcess:
    script_file = tmp_path / "run_test.cmake"
    script_file.write_text(script_text)
    return subprocess.run(
        ["cmake", "-P", str(script_file)],
        capture_output=True,
        text=True,
    )


def test_default_constants(tmp_path):
    """Verify that CMake files define the exact expected supported-disc hashes."""
    script = f"""
    cmake_minimum_required(VERSION 3.20)
    list(APPEND CMAKE_MODULE_PATH "{CMAKE_DIR.as_posix()}")
    include(ImsaGameData)
    message(STATUS "GAME=${{IMSA_GAME_ELF_SHA256}}")
    message(STATUS "SHOOTER2D=${{IMSA_SHOOTER2D_ELF_SHA256}}")
    message(STATUS "LAUNCHME=${{IMSA_LAUNCHME_ELF_SHA256}}")
    """
    res = run_cmake_script(script, tmp_path)
    assert res.returncode == 0, res.stderr
    out = res.stdout + res.stderr
    assert f"GAME={EXPECTED_GAME_HASH}" in out
    assert f"SHOOTER2D={EXPECTED_SHOOTER2D_HASH}" in out
    assert f"LAUNCHME={EXPECTED_LAUNCHME_HASH}" in out


def test_resolve_game_data_rejects_modified_shooter2d(tmp_path):
    game_dir = tmp_path / "game_data"
    (game_dir / "driving").mkdir(parents=True)
    (game_dir / "shooter2d").mkdir(parents=True)

    game_bytes = b"dummy game elf"
    s2_bytes = b"deliberately modified shooter2d"
    lm_bytes = b"dummy launchme elf"

    (game_dir / "driving" / "game").write_bytes(game_bytes)
    (game_dir / "shooter2d" / "shooter2d").write_bytes(s2_bytes)
    (game_dir / "LaunchMe.m2").write_bytes(lm_bytes)

    good_s2_hash = sha256_bytes(b"original authentic shooter2d")
    script = f"""
    cmake_minimum_required(VERSION 3.20)
    list(APPEND CMAKE_MODULE_PATH "{CMAKE_DIR.as_posix()}")
    set(IMSA_DATA_DIR "{game_dir.as_posix()}")
    set(IMSA_GAME_ELF_SHA256 "{sha256_bytes(game_bytes)}")
    set(IMSA_SHOOTER2D_ELF_SHA256 "{good_s2_hash}")
    set(IMSA_LAUNCHME_ELF_SHA256 "{sha256_bytes(lm_bytes)}")
    include(ImsaGameData)
    imsa_resolve_game_data()
    """
    res = run_cmake_script(script, tmp_path)
    assert res.returncode != 0
    err = res.stderr + res.stdout
    assert "shooter2d/shooter2d does not match the supported disc" in err
    assert sha256_bytes(s2_bytes) in err


def test_resolve_game_data_rejects_modified_launchme(tmp_path):
    game_dir = tmp_path / "game_data"
    (game_dir / "driving").mkdir(parents=True)
    (game_dir / "shooter2d").mkdir(parents=True)

    game_bytes = b"dummy game elf"
    s2_bytes = b"dummy shooter2d"
    lm_bytes = b"deliberately modified launchme"

    (game_dir / "driving" / "game").write_bytes(game_bytes)
    (game_dir / "shooter2d" / "shooter2d").write_bytes(s2_bytes)
    (game_dir / "LaunchMe.m2").write_bytes(lm_bytes)

    good_lm_hash = sha256_bytes(b"original authentic launchme")
    script = f"""
    cmake_minimum_required(VERSION 3.20)
    list(APPEND CMAKE_MODULE_PATH "{CMAKE_DIR.as_posix()}")
    set(IMSA_DATA_DIR "{game_dir.as_posix()}")
    set(IMSA_GAME_ELF_SHA256 "{sha256_bytes(game_bytes)}")
    set(IMSA_SHOOTER2D_ELF_SHA256 "{sha256_bytes(s2_bytes)}")
    set(IMSA_LAUNCHME_ELF_SHA256 "{good_lm_hash}")
    include(ImsaGameData)
    imsa_resolve_game_data()
    """
    res = run_cmake_script(script, tmp_path)
    assert res.returncode != 0
    err = res.stderr + res.stdout
    assert "LaunchMe.m2 does not match the supported disc" in err
    assert sha256_bytes(lm_bytes) in err


def test_generate_guest_rejects_modified_shooter2d(tmp_path):
    game_dir = tmp_path / "game_data"
    (game_dir / "driving").mkdir(parents=True)
    (game_dir / "shooter2d").mkdir(parents=True)

    game_bytes = b"dummy game elf"
    s2_bytes = b"deliberately modified shooter2d"
    lm_bytes = b"dummy launchme elf"

    (game_dir / "driving" / "game").write_bytes(game_bytes)
    (game_dir / "shooter2d" / "shooter2d").write_bytes(s2_bytes)
    (game_dir / "LaunchMe.m2").write_bytes(lm_bytes)

    good_s2_hash = sha256_bytes(b"original authentic shooter2d")
    script = f"""
    cmake_minimum_required(VERSION 3.20)
    list(APPEND CMAKE_MODULE_PATH "{CMAKE_DIR.as_posix()}")
    set(CMAKE_SOURCE_DIR "{REPO_ROOT.as_posix()}")
    set(CMAKE_BINARY_DIR "{tmp_path.as_posix()}")
    set(IMSA_GAME_DIR "{game_dir.as_posix()}")
    set(IMSA_GAME_ELF "{game_dir.as_posix()}/driving/game")
    set(IMSA_GAME_ELF_SHA256 "{sha256_bytes(game_bytes)}")
    set(IMSA_SHOOTER2D_ELF_SHA256 "{good_s2_hash}")
    set(IMSA_LAUNCHME_ELF_SHA256 "{sha256_bytes(lm_bytes)}")
    include(ImsaGenerate)
    imsa_generate_guest()
    """
    res = run_cmake_script(script, tmp_path)
    assert res.returncode != 0
    err = res.stderr + res.stdout
    assert "shooter2d/shooter2d does not match the supported disc" in err
    assert sha256_bytes(s2_bytes) in err


def test_generate_guest_rejects_modified_launchme(tmp_path):
    game_dir = tmp_path / "game_data"
    (game_dir / "driving").mkdir(parents=True)
    (game_dir / "shooter2d").mkdir(parents=True)

    game_bytes = b"dummy game elf"
    s2_bytes = b"dummy shooter2d"
    lm_bytes = b"deliberately modified launchme"

    (game_dir / "driving" / "game").write_bytes(game_bytes)
    (game_dir / "shooter2d" / "shooter2d").write_bytes(s2_bytes)
    (game_dir / "LaunchMe.m2").write_bytes(lm_bytes)

    good_lm_hash = sha256_bytes(b"original authentic launchme")
    script = f"""
    cmake_minimum_required(VERSION 3.20)
    list(APPEND CMAKE_MODULE_PATH "{CMAKE_DIR.as_posix()}")
    set(CMAKE_SOURCE_DIR "{REPO_ROOT.as_posix()}")
    set(CMAKE_BINARY_DIR "{tmp_path.as_posix()}")
    set(IMSA_GAME_DIR "{game_dir.as_posix()}")
    set(IMSA_GAME_ELF "{game_dir.as_posix()}/driving/game")
    set(IMSA_GAME_ELF_SHA256 "{sha256_bytes(game_bytes)}")
    set(IMSA_SHOOTER2D_ELF_SHA256 "{sha256_bytes(s2_bytes)}")
    set(IMSA_LAUNCHME_ELF_SHA256 "{good_lm_hash}")
    include(ImsaGenerate)
    imsa_generate_guest()
    """
    res = run_cmake_script(script, tmp_path)
    assert res.returncode != 0
    err = res.stderr + res.stdout
    assert "LaunchMe.m2 does not match the supported disc" in err
    assert sha256_bytes(lm_bytes) in err


def test_real_hashes_reject_unmodified_fake_files(tmp_path):
    """When using the authentic default hashes, fake files are rejected."""
    game_dir = tmp_path / "game_data"
    (game_dir / "driving").mkdir(parents=True)
    (game_dir / "shooter2d").mkdir(parents=True)

    (game_dir / "driving" / "game").write_bytes(b"fake game")
    (game_dir / "shooter2d" / "shooter2d").write_bytes(b"fake shooter2d")
    (game_dir / "LaunchMe.m2").write_bytes(b"fake launchme")

    script = f"""
    cmake_minimum_required(VERSION 3.20)
    list(APPEND CMAKE_MODULE_PATH "{CMAKE_DIR.as_posix()}")
    set(IMSA_DATA_DIR "{game_dir.as_posix()}")
    include(ImsaGameData)
    imsa_resolve_game_data()
    """
    res = run_cmake_script(script, tmp_path)
    assert res.returncode != 0
    err = res.stderr + res.stdout
    assert "driving/game does not match the supported disc" in err
