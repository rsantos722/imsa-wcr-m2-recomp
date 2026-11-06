"""Regression tests for macOS launcher app-bundle host staging.

The bundled m2host must be refreshed whenever the m2host target is rebuilt,
not only when m2launcher relinks: a rebuild that changes only the host does not
relink the launcher, so a launcher-only POST_BUILD hook leaves an old host
inside the .app.

The fixture projects below include the real cmake/ImsaLauncherBundle.cmake
module and run its real staging script, so the host-only rebuild case exercises
the production dependency wiring rather than a Python reimplementation.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
CMAKE_DIR = REPO_ROOT / "cmake"
STAGE_SCRIPT = CMAKE_DIR / "ImsaStageBundleExecutable.cmake"

pytestmark = pytest.mark.skipif(
    sys.platform.startswith("win"),
    reason="fixture asserts Unix host executable names without .exe",
)


@pytest.fixture
def work_dir():
    """Unique work directory, safe for concurrent pytest processes."""
    path = Path(tempfile.mkdtemp(prefix="imsa-bundle-staging-"))
    try:
        yield path
    finally:
        shutil.rmtree(path, ignore_errors=True)


def run_cmake(args, *, cwd=None):
    return subprocess.run(
        ["cmake", *(str(arg) for arg in args)],
        cwd=cwd,
        capture_output=True,
        text=True,
    )


def cmake_path(path):
    """Render a path as a CMake string literal body."""
    return str(path).replace("\\", "/").replace('"', '\\"')


def stage_executable(source, destination, label="m2host"):
    return run_cmake([
        f"-DIMSA_BUNDLE_SOURCE={source}",
        f"-DIMSA_BUNDLE_DESTINATION={destination}",
        f"-DIMSA_BUNDLE_LABEL={label}",
        "-P", STAGE_SCRIPT,
    ])


def write_fixture(work_dir):
    fixture = work_dir / "fixture"
    fixture.mkdir()
    (fixture / "host.c").write_text(
        'const char *host_build_marker = "HOST_VERSION_ONE";\n'
        "int main(void) { return host_build_marker[0] == 'H' ? 0 : 1; }\n"
    )
    (fixture / "launcher.c").write_text(
        'const char *launcher_build_marker = "LAUNCHER_VERSION";\n'
        "int main(void) { return launcher_build_marker[0] == 'L' ? 0 : 1; }\n"
    )
    (fixture / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.24)
project(imsa_bundle_staging_fixture C)

list(APPEND CMAKE_MODULE_PATH "{cmake_path(CMAKE_DIR)}")
include(ImsaLauncherBundle)

add_executable(fakehost host.c)
add_executable(fakelauncher launcher.c)

if(CMAKE_CONFIGURATION_TYPES)
    set(FAKE_BUNDLE_DIR "${{CMAKE_BINARY_DIR}}/$<CONFIG>/Fake Launcher.app")
else()
    set(FAKE_BUNDLE_DIR "${{CMAKE_BINARY_DIR}}/Fake Launcher.app")
endif()

imsa_add_launcher_bundle_host(
    TARGET fake_bundle_host
    HOST_TARGET fakehost
    BUNDLE_DIR "${{FAKE_BUNDLE_DIR}}")
add_dependencies(fakelauncher fake_bundle_host)
""")
    return fixture


def bundle_path(build_dir, multi_config, config="Release"):
    if multi_config:
        return build_dir / config / "Fake Launcher.app"
    return build_dir / "Fake Launcher.app"


def staged_host_path(build_dir, multi_config, config="Release"):
    return bundle_path(build_dir, multi_config, config) / "Contents" / "MacOS" / "fakehost"


def build_tree(build_dir, *extra):
    return run_cmake(["--build", build_dir, "--parallel", *extra])


def rewrite_host_source(fixture, marker):
    """Change the fixture host source after a deterministic timestamp gap.

    Some make implementations compare timestamps in whole seconds. Sleeping
    before the edit guarantees both that the source is newer than the existing
    object and that the rebuilt object lands in a later second than the old
    executable, so the host is genuinely relinked.
    """
    time.sleep(1.2)
    path = fixture / "host.c"
    path.write_text(
        f'const char *host_build_marker = "{marker}";\n'
        "int main(void) { return host_build_marker[0] == 'H' ? 0 : 1; }\n"
    )


def configure_fixture(fixture, build_dir, *, multi_config):
    args = ["-S", fixture, "-B", build_dir]
    if multi_config:
        args += ["-G", "Ninja Multi-Config"]
    result = run_cmake(args)
    assert result.returncode == 0, result.stdout + result.stderr
    return result


def test_stage_executable_copies_updates_and_skips_unchanged(work_dir):
    source = work_dir / "build" / "host" / "m2host"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"host version one")
    source.chmod(0o755)
    destination = work_dir / "app" / "Contents" / "MacOS" / "m2host"

    result = stage_executable(source, destination)
    assert result.returncode == 0, result.stdout + result.stderr
    assert destination.read_bytes() == source.read_bytes()
    assert os.access(destination, os.X_OK)

    # A no-op stage must leave the bundled file untouched; that keeps
    # incremental builds and any future code signing from being invalidated.
    # It must still repair the executable bit if the copied file lost it.
    os.chmod(destination, 0o644)
    sentinel_time = 1_000_000_000
    os.utime(destination, (sentinel_time, sentinel_time))
    result = stage_executable(source, destination)
    assert result.returncode == 0, result.stdout + result.stderr
    assert destination.stat().st_mtime == sentinel_time
    assert "Up to date" in result.stdout
    assert os.access(destination, os.X_OK)

    source.write_bytes(b"host version two")
    result = stage_executable(source, destination)
    assert result.returncode == 0, result.stdout + result.stderr
    assert destination.read_bytes() == b"host version two"
    assert destination.stat().st_mtime != sentinel_time
    assert os.access(destination, os.X_OK)


def test_stage_executable_rejects_missing_source(work_dir):
    destination = work_dir / "app" / "Contents" / "MacOS" / "m2host"
    result = stage_executable(work_dir / "missing" / "m2host", destination)
    assert result.returncode != 0
    output = result.stdout + result.stderr
    assert "does not exist" in output
    assert not destination.exists()


def test_host_only_rebuild_refreshes_bundle_and_restores_deleted_host(work_dir):
    fixture = write_fixture(work_dir)
    build_dir = work_dir / "build"
    configure_fixture(fixture, build_dir, multi_config=False)

    result = build_tree(build_dir)
    assert result.returncode == 0, result.stdout + result.stderr

    host = build_dir / "fakehost"
    launcher = build_dir / "fakelauncher"
    staged = staged_host_path(build_dir, multi_config=False)

    assert host.is_file()
    assert launcher.is_file()
    assert staged.is_file()
    assert staged.read_bytes() == host.read_bytes()

    launcher_mtime = launcher.stat().st_mtime_ns
    first_host_bytes = host.read_bytes()

    # Change only the host source. The launcher target is already up to date
    # and must not relink; the bundle host still has to refresh.
    rewrite_host_source(fixture, "HOST_VERSION_TWO")
    result = build_tree(build_dir)
    assert result.returncode == 0, result.stdout + result.stderr

    updated_host_bytes = host.read_bytes()
    assert updated_host_bytes != first_host_bytes
    assert staged.read_bytes() == updated_host_bytes
    assert launcher.stat().st_mtime_ns == launcher_mtime, (
        "launcher relinked during a host-only rebuild; the test would not "
        "prove that staging is independent of the launcher target"
    )

    # Explicit launcher builds must also recover a missing bundled host, and
    # must build the host first when both the host and the bundle copy are gone.
    staged.unlink()
    result = build_tree(build_dir, "--target", "fakelauncher")
    assert result.returncode == 0, result.stdout + result.stderr
    assert staged.is_file()
    assert staged.read_bytes() == host.read_bytes()

    host.unlink()
    staged.unlink()
    result = build_tree(build_dir, "--target", "fakelauncher")
    assert result.returncode == 0, result.stdout + result.stderr
    assert host.is_file()
    assert staged.is_file()
    assert staged.read_bytes() == host.read_bytes()


@pytest.mark.skipif(shutil.which("ninja") is None, reason="ninja is not installed")
def test_multi_config_staging_is_per_configuration(work_dir):
    fixture = write_fixture(work_dir)
    build_dir = work_dir / "build"
    configure_fixture(fixture, build_dir, multi_config=True)

    result = build_tree(build_dir, "--config", "Release")
    assert result.returncode == 0, result.stdout + result.stderr

    release_host = build_dir / "Release" / "fakehost"
    release_staged = staged_host_path(build_dir, multi_config=True, config="Release")
    assert release_staged.read_bytes() == release_host.read_bytes()
    release_mtime = release_staged.stat().st_mtime_ns

    # Build the other configuration. Its bundle must be staged separately and
    # must not overwrite the Release bundle.
    result = build_tree(build_dir, "--config", "Debug")
    assert result.returncode == 0, result.stdout + result.stderr
    debug_host = build_dir / "Debug" / "fakehost"
    debug_staged = staged_host_path(build_dir, multi_config=True, config="Debug")
    assert debug_staged.read_bytes() == debug_host.read_bytes()
    assert release_staged.stat().st_mtime_ns == release_mtime

    # A host-only change in Debug refreshes only the Debug bundle.
    rewrite_host_source(fixture, "HOST_VERSION_THREE")
    result = build_tree(build_dir, "--config", "Debug")
    assert result.returncode == 0, result.stdout + result.stderr
    assert debug_staged.read_bytes() == debug_host.read_bytes()
    assert release_staged.read_bytes() == release_host.read_bytes()
