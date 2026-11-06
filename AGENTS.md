# Agents guide

Instructions for coding agents working in this repository, with a focus on
debugging the recompiled game. Humans may find it useful too. Start with
[README.md](README.md) for what the project is and [docs/README.md](docs/README.md)
for the reference material.

## Ground rules

- **No game data in the repository.** Never commit anything from the disc or
  derived from it: extracted files, generated guest code (`build*/generated/`),
  captures, screenshots, traces, guest-memory dumps, savestates, or decoded
  assets. `.gitignore` covers the usual paths; check `git status` before
  committing anyway.
- **Do not edit generated guest code.** `game_text_*.c` and friends are
  rebuilt from the disc on every configure. Fix the cause instead: the
  recompiler (`recompiler/transpile.py`), the PPC helpers
  (`runtime/include/ppc_ctx.h`, `recomp_helpers.h`), an OS shim (`runtime/`,
  `host/core/`, `shims/`), or the graphics/audio interpreters (`host/`).
- **The guest owns the game.** Simulation, AI, collision, race rules, menus,
  camera and loading run as recompiled guest code. Host code implements the
  platform underneath. Do not replace guest behavior with host logic or make a
  failing request report success. Optional fixes and restorations go behind
  an environment variable and are documented (see
  [unfinished features](docs/game/unfinished-features.md)).
- **Keep gameplay timing intact.** The guest runs on a 60 Hz VBL tick; see the
  [timing model](docs/recompilation/timing-model.md) before touching pacing.

## Building

Two configurations exist (details in [BUILDING.md](BUILDING.md)):

```sh
# No game data: host with a stub guest, launcher, unit tests. What CI runs.
cmake -S . -B build
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure

# With the user's disc: generates guest code at configure time.
cmake -S . -B build-game -DIMSA_DATA_DIR=/path/to/game-data   # or -DIMSA_DISC=/path/to.iso
cmake --build build-game --parallel 4
```

Be considerate of the machine:

- Keep the job count modest (`--parallel 4`) and run one build at a time. A few
  translation units (`runtime/src/recomp_runtime.c`, `host/graphics/guest_clt.cpp`,
  the generated `game_text_*.c`) need a lot of memory to compile.
- Prefer the system wxWidgets package for the launcher. Building it from source
  (`-DIMSA_LAUNCHER_FETCH_WXWIDGETS=ON`) is slow and memory-hungry; only do it
  when asked.
- Game-data configure, the parity harness, and long headless runs can use far
  more memory than a typical laptop has. Run them only when the task needs
  them, and bound them (`--frames N`).

Builds without game data still compile the guest runtime and the host's
recomp-only paths as compile-only checks (`m2_runtime_compile_check`,
`m2_host_recomp_compile_check`), so a stub build catches most compile errors.

## Debugging workflow

Work from the cheapest observation to the most invasive. Use an optimized
host, bounded runs (`--frames N`), and a throwaway save directory
(`M2_MICROCARD_ROOT=/tmp/m2-card`); never test against real saves.
Diagnostics stay off unless explicitly requested, with caps per trace stream.

1. Reproduce headless with a bounded capture or recorded input. Read console
   subsystem tags for the first missing file, refused selector, or fault.
2. Choose a focused trace, perf profile, or subsystem flag using
   [Diagnostics](#diagnostics) and the
   [environment catalog](docs/recompilation/environment.md). Read the actual
   source reader; use `_FROM` / `_MAX` when that stream supports them.
3. For a paused macOS/Linux guest, start with `status`, keep `advance` / `wait`
   bounded, and inspect input receipts. Never blindly retry input after an
   unconfirmed reply. The complete session/command/receipt reference is in
   [Tools](tools/README.md#diagnostic-control-client).
4. Map guest addresses using [translation](docs/recompilation/translation.md) and
   `tools/guest_ea.py`. Separate game behavior, platform contracts, and host
   implementation before changing a layer. Compare software/GL/null rendering
   or captured audio as appropriate.

The [unattended-race recipe](#reaching-a-race-unattended)
uses the guest AI and five confirm presses. Check every receipt and a newly
completed frame before trusting a capture. A Debug host can be too slow to
judge; AI runs do not verify manual controls or driving feel.

## Making a change

- Add or update a test that exercises the real code path: `tests/unit/` for
  anything that runs without game data (registered in CI), `tests/guest/` for
  tests that need generated guest code, `tests/tools/` for the Python tools.
  Tests must not write into the repository.
- Run the affected tests, then the full `ctest` for the stub build. Report
  what you ran, what was skipped, and anything (game-data runs, Windows, Linux)
  you could not check.
- Adding, renaming or removing an `M2_*`/`IMSA_*` variable: see
  [Maintaining the flag catalog](#maintaining-the-flag-catalog).
- Findings about how the game or the M2 works go in `docs/game/` or `docs/m2/`.
  Docs are reference material: describe how things work, not the history of a
  change.
- Do not split or reformat the large source files in the same change as a
  behavior fix. Restructuring needs its own change and a parity check
  ([verification](tests/README.md)).


## Diagnostics

### Start with the interface that answers the question

| Need                                                                    | Interface / reference                                                                                                                      |
| ----------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| Inspect a paused guest, step, publish buttons, capture presented pixels | [`m2ctl.py` session guide](tools/README.md#diagnostic-control-client) on macOS/Linux                                                       |
| Save a bounded headless capture                                         | `--headless --frames N --capture DIR`                                                                                                      |
| Trace events or measure host phases                                     | `--trace-file PATH`, trace filters, and `--perf-file PATH`                                                                                 |
| Compare two guest-enabled builds                                        | [Verification and testing](tests/README.md)                                                                                                |
| Resolve an address or inspect relocated static data                     | [Translation](docs/recompilation/translation.md) and `tools/guest_ea.py`                                                                   |
| Identify texture replacement keys                                       | [Custom texture replacements](docs/recompilation/rendering.md#custom-texture-replacements)                                                 |
| Investigate checkpoint behavior                                         | [Checkpoint limitations](docs/recompilation/limitations.md#savestate-m2sv--m2ex-continuation-scope) and the actual continuation mappings |

For `m2ctl` control sessions (macOS/Linux only), see the [session guide](tools/README.md#diagnostic-control-client). Use a Release game-enabled host and a throwaway `M2_MICROCARD_ROOT`.

### Reaching a Race Unattended

Use the original game's own AI for the player car (`M2_PLAYER_AI=1`) and five confirm presses through the menus to reach a race on the default track (Testona / 300 World Speedway; state `3/17` in the [state machine](docs/game/state-machine.md)):

```sh
s=/tmp/m2-race   # must not exist yet; keep the path short (socket path limit)
python3 tools/m2ctl.py --session $s start --binary build-game/host/m2host \
    --game-data /path/to/game-data --backend clean \
    --env M2_PLAYER_AI=1 --env M2_MICROCARD_ROOT=$s/card
python3 tools/m2ctl.py --session $s advance 30
for i in 1 2 3 4 5; do
    python3 tools/m2ctl.py --session $s tap a
    python3 tools/m2ctl.py --session $s advance 30
done
python3 tools/m2ctl.py --session $s wait primary eq 3
python3 tools/m2ctl.py --session $s wait secondary eq 17
python3 tools/m2ctl.py --session $s capture race-start.ppm
python3 tools/m2ctl.py --session $s advance 600 --timeout 120
python3 tools/m2ctl.py --session $s capture race-after.ppm
python3 tools/m2ctl.py --session $s quit
```

Verify that each `tap` reports a completed receipt before relying on the capture. For a headless run without a control session, `M2_PREVIEW_MODE=advance` with `M2_PLAYER_AI=1` scripts the same menu route (`M2_PREVIEW_TRACK_PULSES` selects another track).

Two common pitfalls when diagnosing captures:

- **Debug builds are too slow for timing budgets.** `start` refuses unoptimized builds unless `--allow-debug` is passed; a Debug host can fall behind the guest and hit `pause_timeout`. Check `M2_FRAME_TIMING=1` before treating a timeout as a guest hang.
- **Capturing mid-frame.** A `capture` reply's `present_wait` reports whether the image followed a newly completed game frame (`completed`) or a pacing deadline (`expired`). For a settled frame, `wait completed_frames ge N+1` (`N` from `status`) and advance a few ticks first.

### Supported observation settings

| Setting                                                | Effect / scope                                                                                                                                                                                                                                                                                                  |
| ------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `M2_FRAME_TIMING=1`                                    | Host/guest cadence statistics. Interpret with [timing modes](docs/recompilation/timing-model.md).                                                                                                                                                                                                               |
| `M2_AUDIO_MUTE=1`                                      | Zero playback gain while retaining device timing; useful for silent interactive reproduction.                                                                                                                                                                                                                   |
| `M2_CEL_DUMP_SIZE=WxH[,WxH...]`                        | Dump matching raster uploads from the software capture backend. Off when unset.                                                                                                                                                                                                                                 |
| `M2_CEL_DUMP_DIR=DIR`                                  | Raster-dump directory; default `build/scratch/cel_dump` relative to the host's working directory.                                                                                                                                                                                                               |
| `M2_FORCE_TRANSITION=rotate`                           | Force the random screen transition to one effect (`rotate`, `rotate-reverse`, `scroll`, `scroll2`..`scroll4`, `wipe`, `wipe2`..`wipe4`, `stripes`, `grid`, `square1`, `square2`, or a `dotrans` type 0..16). Only the type-2 random request is replaced; explicit transitions and ordinary swaps are unchanged. |
| `--record-input`, `--replay-input`, broker equivalents | Record or replay deterministic controller input.                                                                                                                                                                                                                                                                |

Check [`host/core/main.cpp`](host/core/main.cpp) and [`docs/recompilation/environment.md`](docs/recompilation/environment.md) for the exact values each flag accepts.

### Maintaining the flag catalog

[`launcher/flags_catalog.h`](launcher/flags_catalog.h) lists every `M2_*`/`IMSA_*` environment variable read by the host, runtime, or launcher, and [`docs/recompilation/environment.md`](docs/recompilation/environment.md) is generated from it. When adding, renaming, or removing a variable, update the catalog and run `python3 tools/gen_env_reference.py`; `tests/tools/test_env_reference.py` verifies that the codebase, catalog, and generated markdown stay in sync.

The launcher UI exports only the user-facing flags listed in `kLauncherFlagNames` in [`launcher/run_flags.h`](launcher/run_flags.h) (including its default-on list); unrecognized keys in saved settings are preserved on disk but not exported to the host environment. When adding a user-facing option, update its reader, `run_flags.h`, `flags_catalog.h`, and the launcher settings tests together.

### Investigating a Stalled or Faulting Guest

Classify whether the task is running, blocked, returning, or faulting using console tags (`[recomp]`, `[guest]`, `[gfx]`, `[audio-*]`, `[clt-*]`, `[font]`), trace events, and the published state/frame record. Use a bounded capture:

```sh
M2_MICROCARD_ROOT=/tmp/m2-diagnostic-card build-game/host/m2host \
    --game-data /path/to/game-data --headless --frames 300 \
    --trace-file /tmp/m2-trace.ndjson --trace-no-enter-exit --trace-frames 300 \
    --capture /tmp/m2-capture
```

Trace kinds are `input`, `guest_enter`, `guest_exit`, `state_transition`, `selector_call`, `selector_return`, `asset_load`, `graphics_request`, `audio_request`, `memory_fault`, `indirect_target`, `unknown_behavior`, `frame_complete`, `tracepoint`, `provenance`, and `shutdown`. Select only the needed kinds with `--trace-filter`; `--perf-file` profiles host phases and `M2_OOB_COUNT=1` prints the out-of-bounds access count at exit.

For an out-of-bounds guest EA, check segment placement, relocation, endianness, and stack state against [translation](docs/recompilation/translation.md) and `tools/guest_ea.py`. For graphics issues, compare the software capture backend against OpenGL or set `M2_GL_BACKEND=null` to isolate CLT interpretation from GL submission. Audio output can be recorded with `M2_AUDIO_CAPTURE` or `M2_DSPP_CAPTURE`, and frame cadence with `M2_FRAME_TIMING=1`.