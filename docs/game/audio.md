# Audio Architecture, DSP Synthesis, 3D Sound, and Music Streaming

## Audio Folio Initialization and DSPP Architecture

At startup (`0x1f0f0` and inside the MPEG audio thread at `0x60b8c`), `OpenAudioFolio` (`0x73de8`, 44 B, 5 call sites) invokes `1:262` (`FindCurrentModule`) and `1:103` (`ImportByName` with `"audio"` at `0x173e14`), paired with `OpenAudioPatchFolio` (`0x73e1c`).

`driving/game` imports 24 Audio Folio selectors, wiring samplers, filters (`svfilter.dsp`, `filter_3d.dsp`), `schmidt_trigger.dsp`, `line_out.dsp`, and dynamic `mixer:8x1` / `mixer:7x1` patches into its signal graph at the 44.1 kHz sample clock. SQS2 sample prediction and CBD2 streamed audio decoding are covered in [`.imsasound` Format](../formats/imsasound.md#sqs2-0x53515332-decompression-algorithm) and [`.cbd2` Format](../formats/cbd2.md#cbd2-decompression-algorithm).

## `.imsasound` Asset Format and `sound.spt`

`driving/data/aiff/sound.spt` (119 B) is an offline 3DO authoring script (`setsourcedir Racing:M2_Driving_...:sound_sampler:`, `setdatadir "Racing:3DO:3DO_OS:M2_3.0b:Remote:"`) left on the disc from asset production.

The disc contains 47 `.imsasound` files (24 with sustain loops, 0 with release loops). At runtime, `trackInit_2` (`0x1726c`, 18 calls) and `sndInitUISound` load them through a two-stage loader:
1. **`imsasound_loadfile` (`0x2c6cc`):** Opens and reads the raw file into memory using File Folio `4:35` (`OpenRawFile`), `4:40` (`GetRawFileInfo`), `4:37` (`ReadRawFile`), and `4:36` (`CloseRawFile`).
2. **`imsasound_buffertosample` (`0x2c7c0`):** The [serialized `TagArg` header](../formats/imsasound.md#binary-layout-0x60-byte-tagarg-header--payload) precedes its sample payload. `imsasound_buffertosample` patches the first `AF_TAG_ADDRESS` argument at `buffer + 0x04` to point to `buffer + 0x60`, then passes the `TagArg` array to `CreateSample` (`0x7396c`).

## In-Race Sound Synthesis (`sfx.c`)

`sfx.c` (`sndInitGameSnd` at `0x2956c`, 3,584 B; `sndTeardownGameSnd` at `0x2a36c`, 276 B; `sndKillSnds` at `0x2baec`, 452 B) drives all interactive race audio.

### Voice Reuse
Each logical channel in `sfx.c` owns a fixed instrument and replaces its active sound in-place (`ReleaseInstrument` `6:68` or `StopInstrument` `6:67` -> `SetKnobPart` `6:70` -> `StartInstrument` `6:69`).

### Two-Stage Player Engine Synthesis (`sndSet2StageEng`, `0x2aa64`)
The player car's engine audio passes through a state-variable resonant filter graph:
```text
sampler (Up/Down .imsasound) -> mixer:8x1 -> svfilter.dsp -> mixer:7x1 -> line_out.dsp -> DAC
```
- **Per-Engine Sample Binding (`trackInit_2` at `0x17c90`):** Binds an Up/Down sample `Item` pair from `.bss` into two 28-byte channel records at `gdp+0x20e8` and `gdp+0x2104` based on `UI_GetEngine`:
  - Index `0` (Oldsmobile Aurora V8): `.bss+0xb9c` / `+0xba0` (`engine.0.memimage`)
  - Index `1` (Ford V8): `.bss+0xba4` / `+0xba8` (`engine.1.memimage`)
  - Index `2` (Ferrari V12): `.bss+0xb8c` / `+0xb90` (`engine.2.memimage`)
  - Index `3` (Mazda Rotary): `.bss+0xb94` / `+0xb98` (`engine.3.memimage`)
- **RPM Pitch Curve and Hysteresis (`sndSet2StageEng`, 704 B):**
  Computes the pre-clamp sample rate from engine RPM and load jitter `f3`:
  ```c
  float rate = 22050.0f * powf(fabsf(rpm) / 10000.0f, 1.3f)
             + ((rand() % (26 - (int)fabsf(f3))) * 50 - 625);
  ```
  - When `rate < 750.0f`, calls `ReleaseInstrument` (`6:68`) on both stage channels.
  - When `rate > 750.0f`, starts both channels (`StartInstrument` `6:69`) and clamps the `SampleRate` knob (`SetKnobPart` `6:70`) to `[4500, 44200]` Hz while modulating `svfilter.dsp` cutoff and amplitude between the Up (on-throttle) and Down (off-throttle/overrun) layers. A simpler single-stage companion `sndSetEng` (`0x2ad24`, 448 B) handles non-filtered engine playback.

### Engine DC Offset and Race Mix Level
The player engine reaches the final `mixer:7x1` pair through `svfilter.dsp`'s `HighPass` output, and that output carries a DC offset:
- `svfilter.dsp` truncates its integrator updates, giving a DC offset of about `0.5 / Frequency` in 16-bit units at any signal level.
- The game repeatedly writes `Frequency = 0` to this filter, which holds the integrator at its last value. `HighPass` then carries that held value as a constant, producing DC offsets in the thousands (of either sign) on this mixer input during races.

The race mix has little headroom: the final mixers run at unity `Gain`, the engine input alone can reach full scale, impact samples peak near full scale, and music adds at `Amplitude` 0.6061. Those levels cause brief output saturation in bursts of up to about 17 samples.

### Tire Skid, Road Surface, Scrape, Crash, and Echo SFX
- **Tire Skid (`sndSetSkid` at `0x2afc0`, 648 B):** Calls `TireLoad` (`0x28da0`) across the 4 wheel physics pointers (`car+0x14`, `+0x18`, `+0x1c`, `+0x20`, reading slip/load fields `+0x1fc`, `+0x200`, `+0x204`, and `+0x238`) and reads back its retained DSP knob via `ReadKnobPart` (`6:60`) to modulate multi-wheel squeal pitch and gain. (Its sibling helper `SimpleTireLoad` at `0x28e1c` has no callers.)
- **Road and Kerb Modulation (`sndSetRoad` at `0x2b438`, 740 B):** Modulates road noise and rumble-strip vibration based on wheel speed and the `.qmf` surface material ID.
- **Collisions and Tunnel Echo:** `sndSetScrape` (`0x2b248`, 232 B), `sndSetCrash` (`0x2b330`, 264 B), and `sndSetEcho` (`0x2aee4`, 220 B) trigger impact samples and tunnel/overpass reverb feedback.

## Opponent 3D Spatial and Doppler Audio (`Sound3D`)

AI cars near the player are spatialized through the 3DO `Sound3D` library (`Set3DCarSnds` at `0x2b71c`, `sfxStartOppSnd` at `0x2b8e0`, `sfxProcessOppSnd` at `0x2b98c`, `sfxStopOppSnd` at `0x2b8a8`):
- **Patch Construction (`Create3DSound` at `0x6b2cc`, `MakeSound3DPatchTemplate` at `0x6c8a8`, 4,664 B):** Constructs a binaural DSP patch combining `filter_3d.dsp` head-related filtering, interaural delay lines, and left/right amplitude panning.
- **Per-Frame Spatial Update (`Move3DSound` at `0x6bd2c`):** Evaluates relative polar coordinates (`Calc3DSoundEar` at `0x6b440`), low-pass occlusion (`Calc3DSoundFilters` at `0x6b5c0`), interaural time delay (`Calc3DSoundDelays` at `0x6b620`), interaural intensity difference (`Calc3DSoundIID` at `0x6b82c`), and cone directionality (`Calc3DSoundDirectionality` at `0x6b8ac`).
- **Doppler Pitch Shift (`Calc3DSoundDoppler` at `0x6b7ac`):** Computes elapsed sample frames `DT` using the low 16 bits of `GetAudioFrameCount` (`6:37`, 44.1 kHz counter) and scales the opponent's pitch by `DT / (DT + deltaRadius)`.

## Announcer Voiceovers, CBD2 Music Spooling, and FMV MPEG Audio

### Announcer Voiceovers (`sndDoVoiceOver`)
Race-track commentary is selected by `sndDoVoiceOver`, which opens `.cbd2` AIFC files. For the malformed `.aiff` counterparts left on disc, see [IFF `FORM`/`AIFC` Container Structure](../formats/cbd2.md#iff-formaifc-container-structure).

### Interactive Music Streaming (`sndStartMusic`, `0x28ad4`)
Menu music (`ui_music.cbd2`) and the four in-race soundtracks (`grunge_*`, `techno_*`, `rock_*`, and `pop_*` `.cbd2` segments) are streamed from disc via `SoundPlayer` (`spCreatePlayer` at `0x6f0c0`, `spAddSoundFile` at `0x6f7e8`, `spService` at `0x6e5f4`) and `SoundSpooler` (`ssplCreateSoundSpooler` at `0x70bd4`, `ssplSendBuffer` at `0x70f68`, `LinkAttachments` into `sampler_cbd2_f2.dsp`). At segment boundaries, the registered callback `sndDecisionFunc` (`0x2bcb0`) checks `UI_GetSoundTrack` and `rand()` to pick the next segment via `spSetBranchAction` (`0x6e7cc`).

### FMV MPEG-1 Layer II Audio (`MPEGAudioSubscriberThread`, `0x60d6c`)
During `VideoPlayer` `.mov` playback, `MPEGAudioSubscriberThread` and `MPADecoderThread` (`0x63cf8`) demultiplex `MPAU` stream chunks (`GetCompressedBfr` at `0x63be8`, `CompressedBfrRead` at `0x63c9c`), decoding 1,152 stereo PCM samples (`4,608` bytes) per MPEG-1 Layer II frame into the Audio Folio spooler.

## DataStream Video Playback

When `VideoPlayer` (`0x3c27c`) plays an FMV `.mov` file (`VStreamInit` -> `VStreamPlay` -> `VStreamCleanup`), five cooperating threads run concurrently across four subscriber roles:

1. **`DataAcqThread` (priority `197`)**:
   - Opens the `.mov` file via `OpenFile` (`4:0`) and reads 32 KB / 64 KB sector-aligned stream buffers via `CMD_BLOCKREAD`.
2. **`DataStreamThread` (priority `196`)**:
   - Parses `SubsChunkHeader` packets inside each filled stream buffer (`DeliverDataChunks` at `0x68bc0`), increments the buffer's `useCount` (`bp + 0x10`) for each dispatched chunk, and sends subscriber messages from a 16-message pool (`ctx + 0x90 / +0x94`) to the registered `'MPVD'` and `'MPAU'` subscriber ports.
   - Reclaims stream buffers when all subscribers reply (`ReplyMsg` decrements `bp->useCount` to `0`).
   - Maintains the master presentation clock (`DSSetPresentationClock` at `0x684bc` / `DSGetPresentationClock`, using an 8-tick jitter window before resynchronizing).
3. **`MPEGAudioSubscriberThread` (priority `202`) and `MPADecoderThread` (priority `195`)**:
   - Receives `'MPAU'` chunks, decodes 1,152-sample PCM frames via `MPAudioDecode` (`7:0`), feeds the `SoundSpooler`, and advances the DataStream presentation clock as audio buffers finish DMA playback.
4. **`MPEGVideoSubscriberThread` (priority `198`)**:
   - Receives `'MPVD'` chunks, queues compressed slices to `mpegvideo.device` (`MPEGVIDEOCMD_WRITE`), and queues `MPEGVIDEOCMD_READ` requests for target display `Bitmap` buffers.
   - Compares each decoded frame's PTS (`FMVGetPTS` at `0x65114`) against `DSGetPresentationClock` (or VBL pacing on video-only streams like `credits.mov` and `dummy.mov`) before displaying the frame and signaling `VideoPlayer` on stream completion (`kDSStreamEoFErr` / `STOP` chunk).
