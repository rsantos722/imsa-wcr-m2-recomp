# Building `imsa-wcr-m2-recomp`

This repository contains no game data and no pre-generated guest code. To build
the playable game, supply a copy of the *IMSA presents World Championship Racing*
disc image (`.iso`) at CMake configure time. Configuration verifies the disc,
extracts its filesystem, verifies the executables, and runs the static
recompiler (`recompiler/transpile.py`). The build then compiles the host
(`m2host`) and builds the desktop launcher (`m2launcher`). These steps live in
[`cmake/ImsaGameData.cmake`](cmake/ImsaGameData.cmake) and
[`cmake/ImsaGenerate.cmake`](cmake/ImsaGenerate.cmake).

You can also build without game data (for CI, development, or unit testing).
In that configuration `m2host` links a stub guest (`IMSA_WITH_GAME=OFF`).

## 1. Prerequisites


| Dependency                      | Version                                 | Purpose                                                                                                                               |
| ------------------------------- | --------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| **C / C++ compiler**            | C11 / C++20                             | Clang on macOS, GCC or Clang on Linux, MinGW-w64 UCRT64 GCC or Clang on Windows. MSVC is not supported; configure rejects it.         |
| **CMake**                       | 3.24+                                   | Build configuration and the code-generation step                                                                                      |
| **Python 3**                    | —                                       | Runs `tools/operafs.py` (ISO extraction) and `recompiler/transpile.py` (PowerPC lifting)                                              |
| **`capstone`** (Python package) | Pinned in `recompiler/requirements.txt` | PowerPC disassembly during recompilation; needed only when building with game data                                                    |
| **SDL3**                        | 3.4.0+                                  | Window, OpenGL context, audio output, and input. Earlier SDL3 releases lack `SDL_LoadPNG`, which the replacement-texture loader uses. |
| **OpenGL** *(optional)*         | 3.3 Core (4.1 Core context on macOS)    | Hardware renderer alongside the software capture backend                                                                              |
| **FFmpeg** *(optional)*         | `libavcodec`, `libavutil`               | MPEG-1 still backgrounds and FMV playback. Without it the decoders report unavailable and fail closed.                                |
| **wxWidgets** *(optional)*      | 3.2+ (`core`, `base`)                   | Desktop launcher (`m2launcher`)                                                                                                       |


### macOS (Homebrew)

```sh
brew install cmake ninja sdl3 ffmpeg wxwidgets python3
python3 -m venv .venv && source .venv/bin/activate
python -m pip install -r recompiler/requirements.txt
```

### Linux (Debian / Ubuntu)

```sh
sudo apt install build-essential cmake ninja-build libsdl3-dev libgl-dev \
    libavcodec-dev libavutil-dev libwxgtk3.2-dev python3 python3-venv python3-pip
python3 -m venv .venv && source .venv/bin/activate
python -m pip install -r recompiler/requirements.txt
```

SDL3 3.4.0 or newer is required. If your distribution's `libsdl3-dev` is
older or missing, build SDL3 from source and install it where CMake can find
it.

Distributions that mark the system Python as externally managed (PEP 668)
reject a bare `pip install`. Activate the virtual environment before
configuring so CMake's `find_package(Python3)` and the recompiler use it.

### Windows (MSYS2 UCRT64)

Build inside an MSYS2 **UCRT64** shell with MinGW-w64:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-sdl3 \
    mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-wxwidgets3.2 \
    mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-pip
python -m venv .venv && source .venv/Scripts/activate
python -m pip install -r recompiler/requirements.txt
```

Use `mingw-w64-ucrt-x86_64-clang` instead of `-gcc` for Clang. Make sure
`cmake`, `ninja`, `python` and SDL3 (plus FFmpeg and wxWidgets if wanted) come
from the UCRT64 prefix before configuring.

## 2. Supplying the Game Data

Options A and B supply the disc; Option C builds without it. The
[disc manifest](launcher/disc-manifest.json) lists every file and its hash.

### Option A: the ISO (`-DIMSA_DISC`)

Pass your 2048-byte/sector OperaFS image (`671,399,936` bytes, SHA-256
`b34e30635435b13c080e945c6343aa8f6920afc278eef2669970a7632c066835`):

```sh
cmake -S . -B build -DIMSA_DISC=/path/to/imsa.iso
cmake --build build --parallel 4
```

During configuration, CMake:

1. Verifies the ISO size and SHA-256 digest.
2. Extracts the OperaFS filesystem into `build/game-data/` with `tools/operafs.py`.
3. Verifies `driving/game` (SHA-256 `bb91fcd900424e0894755153b850892b39a704aa4493947b2aa632abaf290865`),
 `shooter2d/shooter2d` (SHA-256 `9955160c2279e40c303c9dc7997f900ef9eaf46bda3799d5f09550cbdb623d24`),
 and `LaunchMe.m2` (SHA-256 `f1062d63ab9adb7db3b18b69e5170b799bcf6023e88800342208ce09937ecddf`).
4. Runs `recompiler/transpile.py` to generate C sources and relocation tables
 for all three executables under `build/generated/`. Generation happens at
 configure time, not at compile time.

### Option B: an extracted folder (`-DIMSA_DATA_DIR`)

To extract the ISO once and reuse it across builds:

```sh
python3 tools/operafs.py extract /path/to/imsa.iso /path/to/game-data
cmake -S . -B build -DIMSA_DATA_DIR=/path/to/game-data
cmake --build build --parallel 4
```

The folder must contain `driving/game`, `shooter2d/shooter2d` and `LaunchMe.m2`.

### Option C: no game data (stub guest)

Without `IMSA_DISC` or `IMSA_DATA_DIR`, CMake builds `m2host` with a stub
entry point, the launcher when wxWidgets is available, and every unit test that
needs no game data. This is what public CI runs:

```sh
cmake -S . -B build
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Importing a disc in the launcher does not turn a stub host into a playable
build; configure with `-DIMSA_DISC` or `-DIMSA_DATA_DIR` to generate and link
the guest code.

## 3. CMake Options


| Option                                           | Default | Description                                                                                                                                                                                                       |
| ------------------------------------------------ | ------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `IMSA_DISC`                                      | `""`    | Path to the OperaFS disc image (`.iso`). Extracted into `<build>/game-data`.                                                                                                                                      |
| `IMSA_DATA_DIR`                                  | `""`    | Path to an extracted disc folder containing `driving/game`, `shooter2d/shooter2d` and `LaunchMe.m2`.                                                                                                              |
| `IMSA_FFMPEG_ROOT`                               | `""`    | Use the FFmpeg installed under this prefix (`include/`, `lib/`) and fail if it is incomplete, instead of searching the system. Release packages set it to the LGPL build from `packaging/ffmpeg/build-ffmpeg.sh`. |
| `IMSA_BUILD_TESTS`                               | `ON`    | Build the CTest unit and guest test targets.                                                                                                                                                                      |
| `IMSA_WITH_GL`                                   | `ON`    | Enable the OpenGL rendering backend (`M2_HAVE_GL=1`).                                                                                                                                                             |
| `IMSA_WITH_FFMPEG`                               | `ON`    | Enable FFmpeg MPEG-1 still and video decoding when the libraries are found.                                                                                                                                       |
| `IMSA_BUILD_LAUNCHER`                            | `ON`    | Build the `IMSA WCR M2 Recomp Launcher` UI (`m2launcher`) when wxWidgets is available. The disc-import and save libraries always build for the tests.                                                             |
| `IMSA_LAUNCHER_FETCH_WXWIDGETS`                  | `OFF`   | Download and build a static, minimal wxWidgets 3.2.8 with `FetchContent` instead of using an installed one. Slow and memory-hungry.                                                                               |
| `IMSA_PARITY_REFERENCE`, `IMSA_PARITY_CANDIDATE` | `""`    | Two game-enabled `m2host` binaries; when both are set, CTest registers the parity harness. See [tests/README.md](tests/README.md).                                                                               |


## 4. Build Outputs

- **Host:** `build/host/m2host` (`m2host.exe` on Windows)
- **Launcher:** `build/launcher/m2launcher` (`m2launcher.exe` on Windows)
- **macOS app bundle:** `build/launcher/IMSA WCR M2 Recomp Launcher.app`, with
the launcher at `Contents/MacOS/m2launcher` and a copy of `m2host` beside it

A host built with game data finds its data from the build. To point it at
another extraction:

```sh
./build/host/m2host --game-data /path/to/game-data
```

Use `build/game-data` for a build configured with `IMSA_DISC`, or the folder
passed as `IMSA_DATA_DIR`.

## 5. Running the Tests

```sh
# C/C++ unit tests, and guest tests in a game-data build
ctest --test-dir build --output-on-failure

```

See [tests/README.md](tests/README.md) for the Python tool tests, stub versus
game-enabled suites, and headless parity checks.

## 6. Packaging

`cmake/ImsaPackaging.cmake` defines the install rules and CPack settings.
Distribution FFmpeg builds enable GPL components, and bundling one would make
the package GPL, so build the LGPL FFmpeg first and point the build at it:

```sh
packaging/ffmpeg/build-ffmpeg.sh "$PWD/ffmpeg-lgpl"
cmake -S . -B build -DIMSA_FFMPEG_ROOT="$PWD/ffmpeg-lgpl"
cmake --build build --parallel 4
cpack --config build/CPackConfig.cmake -C Release -B dist
```


| Platform        | Output                                                                                                                                                                        |
| --------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Windows (MSYS2) | Portable `.zip` with `IMSA WCR M2 Recomp.exe` and `README.txt` at the root, plus `bin/`, `licenses/`, and `userdata/`. Bundles toolchain and FFmpeg DLLs into `bin/`, along with `bin/portable.txt` so saves, settings, and extracted game files stay in `userdata/`. |
| macOS           | `.dmg` containing a single ad-hoc signed `.app` with bundled dylibs.                                                                                                          |
| Linux           | Portable `.tar.gz` with `imsa-wcr-m2-recomp` and `README.txt` at the root, plus `bin/`, `lib/`, `share/`, `licenses/`, and `userdata/`. `cmake --install` omits the portable files to produce a `bin`/`lib`/`share` tree; `packaging/appimage/build-appimage.sh <build-dir> <out-dir>` packages that tree into a non-portable AppImage (requires `linuxdeploy` and its GTK plugin). |
