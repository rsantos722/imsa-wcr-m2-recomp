# Timing Model and Frame Rate Architecture

The host models display fields, physics timers, the PowerPC Time Base, and audio samples as four independent clock domains. Guest code advances at its own synchronization boundaries rather than rescaling vehicle physics or game logic to the host monitor's refresh rate.

## Clock Domains

| Clock Domain | Guest Interface | Frequency / Period | Host Implementation |
| :--- | :--- | :--- | :--- |
| **Display Fields** | VBL waits in `StateDriver` and GState | `60,000 / 1001` Hz (~59.94 Hz NTSC) | Integer rational period tracking in [`VblPacer`](../../host/core/vbl_pacer.cpp). |
| **Physics Metronome** | `CarSimThread`, `TIMERCMD_METRONOME_USEC` | 16,666 µs (~60.0024 Hz) | Monotonic microsecond timer in [`guest_kernel.c`](../../runtime/src/guest_kernel.c), running asynchronously from rendering. |
| **PPC Time Base** | `SampleSystemTimeTT` (21), `ConvertTimerTicksToTimeVal` (27) | 8,333,333 Hz | Scaled monotonic host clock. |
| **Audio Clock** | `GetAudioTime` (35), `GetAudioFrameCount` (37), `SignalAtAudioTime` (58) | 44,100 Hz sample rate | Driven by the audio playback stream; 184 sample frames per `AudioTime` tick. |

## Display Deadlines and Presentation

[`VblPacer::due()`](../../host/core/vbl_pacer.cpp) tracks target field deadlines using integer rational nanoseconds:

$$\text{Period} = \frac{1001 \times 1,000,000,000}{60,000} \text{ ns} \approx 16,683,333 \text{ ns}$$

A late wakeup grants at most one field permit and schedules the next deadline in the future rather than bursting missed VBL pulses. If a host stall exceeds 500 ms, the pacer resets its phase.

Swap pacing flags:

- `--vbl-paced-swap` (default, `M2_VBL_PACED_SWAP=1`): Uses non-blocking SDL buffer swaps (`swap_interval = 0`) and paces the loop on the rational NTSC VBL timer.
- `--no-vbl-paced-swap` (`M2_VBL_PACED_SWAP=0`): Uses blocking vsync swap intervals.
- `M2_GL_SWAP_INTERVAL`: Explicitly overrides the OpenGL swap interval.

## Physics Simulation Timers

`CarSimThread` (`.text+0xb054`) runs on its own thread and requests a 16,666 µs metronome timer. Each wake advances the physics world by six substeps ($6 \times 1/360\text{ s}$). If host CPU contention causes the simulation thread to miss a deadline, [`guest_kernel.c`](../../runtime/src/guest_kernel.c) advances the timer deadline by the missed interval count and coalesces the pending timer signal so simulation ticks do not backlog.

## Audio Service and Muting

- **Interactive Playback**: A background audio thread steps the DSPP interpreter in batches of up to 128 sample frames, keeping the output queue filled to three device blocks. `GetAudioTime` advances from frames rendered by the device, one tick every 184 stereo samples (`GK_AUDIO_GLOBAL_CLOCK_DURATION`).
- **Output Underruns**: If the worker falls behind, the DSPP layer fades out over 128 frames, plays silence, and fades back in. Silence already played is not produced later, so a stall leaves output latency unchanged. The DSPP simulation pauses for the gap and falls behind device time by its length; `GetAudioTime`, music, and FMV audio keep running on device time.
- **Audio Mute (`M2_AUDIO_MUTE=1`)**: Silences output by setting the SDL stream gain to zero while keeping the background DSPP worker and audio clock running at normal speed.
- **Headless and Control Socket Runs**: When no audio device is opened, the host mixes audio synchronously in batches of 735 stereo samples ($44,100 / 60$) per display field.

## Execution Modes

| Mode | Timing Characteristics | Primary Use Case |
| :--- | :--- | :--- |
| **Normal Windowed** | Wall-clock VBL pacing; asynchronous audio worker | Interactive gameplay. |
| **Fast-Forward (`--fast-forward`)** | VBL wait disabled; runs unthrottled | Benchmarking and automated smoke tests. |
| **Headless Capture (`--headless`)** | Stepped frame permits; software rasterizer | Repeatable screenshot and regression capture. |
| **Control Socket (`--control-socket`)** | External tick stepping via `m2ctl.py`; synchronous audio | Interactive debugging and scripted state inspection. |

## Frame Sequence Tracking and Telemetry

The runtime tracks four distinct counters across the host and guest:
1. **Host Presented Frames**: Buffer swaps completed by the host renderer.
2. **Guest VBL Fields**: 59.94 Hz display field permits delivered to `StateDriver`.
3. **Application Frames**: Iterations of the top-level `StateDriver` loop (`.text+0x2fb88`).
4. **Physics Substeps**: $1/360\text{ s}$ rigid-body integration steps inside `do_simworld`.

At the top of each iteration, `StateDriver` writes a telemetry snapshot at `0x003F0000` (defined in [`runtime/include/ppc_ctx.h`](../../runtime/include/ppc_ctx.h) and read via `m2_guest_frame_query()`):

| Address | Field | Meaning |
| :--- | :--- | :--- |
| `0x003f0000` | `frame_count` | Monotonic application frame counter. |
| `0x003f0004` | `primary_state` | Current top-level state ID (`0x1877d8`). |
| `0x003f0008` | `guest_pc` | Publishing PowerPC program counter. |
| `0x003f0010` | `secondary_state` | Sub-state ID (menu screen or race phase). |
| `0x003f0014` | `old_phase` | Previous state ID. |
| `0x003f0018` | `menu_cursor` | Active menu selection index. |
| `0x003f001c` | `saved_r27` | Context register snapshot. |
