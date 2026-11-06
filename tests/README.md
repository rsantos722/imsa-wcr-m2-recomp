# Tests

Three suites, split by what they need:

| Directory | Needs | Runs in public CI |
| --- | --- | --- |
| `unit/` | Nothing beyond the build; launcher tests in `unit/launcher/` may use `IMSA_DATA_DIR` when present and skip otherwise | Yes |
| `tools/` | Python with the packages in `tools/requirements.txt` and `tests/tools/requirements.txt`; Pillow-dependent decoders skip when it is absent | Yes |
| `guest/` | The user's extracted game data and a built host; skip cleanly (exit 77 / CTest skip) when absent | No |

```sh
# C++ and launcher unit tests (registered with CTest)
ctest --test-dir build --output-on-failure

# Python tool tests
python3 -m pip install -r tools/requirements.txt -r tests/tools/requirements.txt
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python3 -m pytest -q tests/tools

# Guest parity check (two host builds and disc data)
python3 tests/guest/parity.py \
    --reference /path/to/old/m2host --candidate build-game/host/m2host \
    --root /path/to/game-data --frames 60 --runs 2
```

## Guest and Steering Test Targets

| Target / Script | Needs Game Data | Coverage |
| --- | --- | --- |
| `guest/parity.py` | Yes | Runs two `m2host` binaries headlessly and compares frame hashes and NDJSON trace records across repeated runs to verify behavioral parity after refactors. |
| `guest/test_mpeg_audio.cpp` | Yes (`IMSA_TEST_MOVIE_PATH`) | Verifies MPEG-1 Layer II audio stream decoding against a disc `.mov` file. |
| `guest/test_audio_items.c` | Yes (the disc's DSP templates) | Audio folio items, connections, voice routing, generated mixers, patches, and the asynchronous DSPP worker on the disc's own templates. |
| `guest/test_libm_zero_size.c`, `guest/test_reset_reopen_lifecycle.c` | Yes | Exercises generated guest code and runtime reset/reopen lifecycles. |
| `m2_tire_probe` (`guest/tire_probe.c`) | Yes | No-boot harness that calls the guest's tire setup, force, and control routines (`ProcessInput`, `do_controls`, `patchforce`) across tire brands, loads, temperatures, wear, and slip angles. Used by CTest and [`tools/tire_sweep.py`](../docs/recompilation/tire-bench.md). |
| `m2_analog_steering`, `m2_control_modes` | No | Unit tests for the live analog steering controller ([`docs/recompilation/analog-steering.md`](../docs/recompilation/analog-steering.md)), control profiles, and digital D-pad handoff. |
| `m2_steering_vehicle` | Yes | Full-car flat-plane simulation harness exercising all four cars and tire brands through the guest suspension, tire, aero, and Newton integration callbacks across held turns, taps, releases, and reversals. |

CI installs the same Python packages, so decoder tests that write PNGs or need numpy run instead of skipping.

Tests never write into the repository; temporary directories are created under `std::filesystem::temp_directory_path()` or `tempfile.mkdtemp()`.
