# Tools

Python utilities for inspecting and extracting the user's game data, plus the host diagnostic control client.

| Tool | Purpose |
| --- | --- |
| `m2ctl.py` | Control a suspended `m2host` diagnostic session over its local socket |
| `operafs.py` | List or extract an OperaFS ISO |
| `m2_lzss.py` | Decompress M2/3DO loader LZSS sections |
| `decode_cbd2.py` | Decode CBD2 AIFF-C audio |
| `decode_imsasound.py` | Decode `.imsasound` samples |
| `decode_rgb.py` | Decode raw RGB stills |
| `decode_txtr.py` | Decode TXTR/FONT textures |
| `decode_message_cel.py` | Decode packed-cel images |
| `decode_kernel_opera.py` | Inspect the `kernel.opera` image |
| `extract_symbols.py` | Build function/source maps from a guest ELF |
| `guest_ea.py` | Resolve guest effective addresses and inspect the relocated `.data` image |
| `tire_sweep.py` | Measure the original tire-force response with a built guest probe (see [Tire Bench](../docs/recompilation/tire-bench.md)) |
| `gen_env_reference.py` | Generate/check the [environment catalog](../docs/recompilation/environment.md) from launcher flags |
| `gen_selector_names.py` | Build the host's selector name tables from the guest images (run by CMake) |

`decode_txtr.py` and `decode_rgb.py` require Pillow for PNG output. `decode_cbd2.py` requires `numpy` and writes Ogg through `soundfile` when available (or an `ffmpeg` binary with a Vorbis encoder otherwise). `decode_imsasound.py` uses the same Ogg path for `--all`:

```sh
python3 -m pip install -r tools/requirements.txt
```

Most decoders take a positional input path (`decode_rgb.py FILE`, `operafs.py extract ISO DIR`, ...); `decode_kernel_opera.py` uses `--input`, and `--root` / `IMSA_DATA_DIR` select an extracted tree for `--all` batch modes.

## Diagnostic control client

`m2ctl.py` drives the host's `--control-socket` channel. Each client command is one tab-separated line on a Unix-domain socket, and the host answers with one JSON line:

```text
<request-id>\t<verb>[\t<argument>...]\n
{"schema":1,"id":"...","pid":123,"ok":true,"error":null,"state":{...},"result":{...}}\n
```

The client uses only the Python standard library (Python 3.9+). Build `m2host` with game data first (see [BUILDING.md](../BUILDING.md)); a stub-only build has no guest image and refuses to open the control socket.

### Starting or Attaching to a Session

`start` creates a new session directory, launches an already-built `m2host`, and waits for its paused status handshake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DIMSA_BUILD_LAUNCHER=OFF \
    -DIMSA_DISC=/path/to/imsa.iso
cmake --build build --target m2host --parallel 4
python3 tools/m2ctl.py --session /tmp/m2-session start --binary build/host/m2host \
    --game-data build/game-data --env M2_MICROCARD_ROOT=/tmp/m2-session/card
python3 tools/m2ctl.py --session /tmp/m2-session status
python3 tools/m2ctl.py --session /tmp/m2-session wait secondary eq 4
python3 tools/m2ctl.py --session /tmp/m2-session tap a
python3 tools/m2ctl.py --session /tmp/m2-session capture screen.ppm
python3 tools/m2ctl.py --session /tmp/m2-session quit
```

For an existing extraction, configure with `-DIMSA_DATA_DIR=/path/to/game-data` instead and pass that directory to `start --game-data`. Diagnostic control uses `AF_UNIX` sockets on macOS and Linux (see the [Diagnostics Guide](../AGENTS.md#diagnostics) for workflow details).

Key session options:
- The session directory must not exist before `start`, and its path must stay short enough for `AF_UNIX` socket binding (103 characters max on macOS).
- Software rendering is the default and runs headless; `--backend clean` or `--backend cel` opens a windowed GL context.
- `--width` and `--height` must be specified together, and `--perf-file` must stay inside the session directory.
- Inherited `M2_*` variables are cleared on launch; pass explicit overrides with `--env M2_NAME=value`.
- If the adjacent CMake cache reports a `Debug` build, `start` requires `--allow-debug` so tick and pause budgets are not skewed by unoptimized builds.
- `--startup-timeout` bounds the initial paused handshake (default 30 seconds).

You can also attach `m2ctl.py` to a manually launched host pointing at `SESSION/control.sock`:

```sh
mkdir -p /tmp/m2-session
M2_MICROCARD_ROOT=/tmp/m2-session/card \
    build/host/m2host --control-socket /tmp/m2-session/control.sock --headless \
    --game-data /path/to/game-data
python3 tools/m2ctl.py --session /tmp/m2-session status
```

### Commands

| Command | Contract |
| --- | --- |
| `status` | Suspended state, task/game states, clocks, input receipts, and graphics counters. |
| `tap a` | Publish a press, release at its copy-completion boundary, and wait for the release copy. |
| `hold a`, `release` | Explicit level input; waits for one copied sample. A hold persists across advancing commands. |
| `advance 5` | Service five platform ticks, then suspend. Each tick services video, input, and 735 stereo audio sample frames. |
| `wait secondary eq 27` | Service bounded ticks until the predicate holds, then suspend. Fields: `primary`, `secondary`, `guest_frame`, `completed_frames`, `audio_frames`, `ticks`, or big-endian `u8:EA`, `u16:EA`, `u32:EA`. Comparisons: `eq`, `ne`, `ge`. |
| `read 0x6bf3f0 64` | Read a bounded guest-memory range as hex. |
| `watch 0x6bf3f0 0x3ae8 --label footer` | Enable the bounded store/copy provenance collector at the current location. |
| `provenance writes.json` | Export collected store/copy provenance records to JSON. |
| `trace events.ndjson --kinds input,frame_complete,provenance` | Enable or reconfigure filtered runtime tracing without restarting the game. |
| `graphics` | Nonzero TE register cells and current/cached texture metadata. |
| `capture screen.ppm` | Capture the renderer's presented pixels (`result.present_wait` reports `completed`, `expired`, `guest_ended`, or `unpaced`). Refuses to overwrite existing files. |
| `pause` | Cancel an outstanding command, neutralize input, and leave the host suspended. |
| `quit` | Cancel outstanding work and close the host while retaining session logs. |

Buttons accept pad names (`a b c start l r x lookback up down left right none`) or a numeric ControlPort mask. Directions also set the stick axis, and `--x`/`--y` set analog bytes `0..255` (neutral `128`). Advancing commands default to 1,800 ticks and 30 seconds (120 seconds for `wait`), up to a maximum of 12,000 ticks and 120 seconds. When a budget expires, the host returns the suspended state with `ok:false` and neutralizes input so you can continue inspecting the session.

Outputs (`capture`, `provenance`, `trace`) are written inside the session directory alongside `launch.json`, `session.json`, `host.pid`, `supervisor.pid`, `host.log` (8 MiB cap), `commands.ndjson` (1 MiB cap), and `exit.json`.

### Exit Status and Constraints

- Exit status is `0` for a successful host reply, `1` for a host-reported `ok:false`, and `2` for a client, transport, startup, or protocol failure.
- A lost reply is reported with `"outcome":"unconfirmed"`. Reuse the original `request-id` with identical arguments to reconcile a lost reply rather than blindly retrying an input command.
- Diagnostic control requires a fresh guest boot and exclusive input: it cannot be combined with replay, savestates, memory pokes, or `M2_PREVIEW_MODE`. Cooperative pause suspends between guest boundaries and cannot preempt a blocking native call.
- `ticks`, input-copy receipts, `completed_frames`, and `capture`'s `present_wait` each track a different boundary. Inspect `status` first, step with a bounded `wait` or `advance`, and then capture or read memory.
- `pause_timeout` indicates the cooperative barrier was not reached within five seconds; guest-memory snapshots are withheld and only `status` and `quit` remain available.

