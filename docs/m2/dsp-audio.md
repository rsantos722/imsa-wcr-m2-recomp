# M2 Bulldog DSPP Audio Processor, Audio Folio (`folio 6`), and AudioPatch (`folio 5`)

The 3DO M2 **Bulldog ASIC** includes an enhanced **Digital Signal Processing Processor (DSPP)** running at a `44.1 kHz` sample frame rate (independent of display fields). All sound effects (`.imsasound`), streaming music and voiceovers (`.cbd2`), FMV audio (`MPAU` via `SoundSpooler`), and 3D Doppler/interaural spatialization (`Sound3D`) are synthesized and mixed through the Audio Folio (`folio 6`) and AudioPatch Folio (`folio 5`).

## M2 Bulldog DSPP Hardware Architecture

| Parameter | M2 Bulldog DSPP Specification |
|---|---|
| **Sample Frame Rate** | `44,100 Hz` (`1` full-rate pass per `22.6757 µs`) |
| **Instruction Memory (`NMEM`)** | `1,024 × 16-bit` words (`0x000 .. 0x3FF`) |
| **Data Memory (`DMEM`)** | `736 × 16-bit` signed words (`0x000 .. 0x2DF`, `DSPI_DATA_MEMORY_SIZE = 0x2E0`) |
| **Hardware I/O Registers (`DSPI`)** | `0x2E0 .. 0x3FF` (`OutputToDAC` stereo pair at `0x2E0..0x2E1`, FIFO status/control at `0x3D6..0x3DF`, sample clock at `0x3F7`) |
| **ALU & Accumulator (`ACC`)** | `20-bit` wide (`16-bit` fractional/integer + `4` guard/headroom bits; carry bit at `0x00100000`), barrel shifter, saturation/clipping, condition flags `N, Z, C, V, X` |
| **FIFO DMA Channels (`FDMA`)** | `32` channels (`dspi_FifoDma[0..31]`), supporting 16-bit PCM, 8-bit PCM, `SQS2` (`sampler_sqs2_*.dsp`), and `CBD2` (`sampler_cbd2_*.dsp`) hardware decompression |
| **Hardware Triggers** | `16` interrupt/trigger bits (`ArmTrigger`, used by `schmidt_trigger.dsp` and envelope/drift completion events) |

### Multi-Rate Execution (`nanokernel.dsp`)

`System.m2/Audio/dsp/nanokernel.dsp` orchestrates cyclic execution across three calculation rate lists (`AF_TAG_CALCRATE_DIVIDE`):
1. **Full-Rate (`Divide = 1`, `44,100 Hz`)**: Samplers, mixers, filters, and DAC output (`line_out.dsp`).
2. **Half-Rate (`Divide = 2`, `22,050 Hz`)**: Half-rate samplers/effects toggled on alternate `FrameCountLow` bit 0 ticks.
3. **Eighth-Rate (`Divide = 8`, `5,512.5 Hz`)**: Control-rate envelopes (`envelope.dsp`), LFOs, and 3D parameter smoothing.

## DSP Instrument Template Container (`FORM 3INS` / `FORM DSPP`)

`System.m2/Audio/dsp/` contains 73 M2 DSP instrument templates. Each template is an IFF `FORM 3INS` / `FORM DSPP` container (parsed on the host by [`dsp_template_loader.c`](../../runtime/src/dsp_template_loader.c)):

| Chunk | Structure & Role |
|---|---|
| `DHDR` | DSPP header: target ASIC version, flags, and calculation rate requirements. |
| `MRSC` (`20` B/entry) | Resource requirements table (`517` entries across the 73 files): `{ rsrcType, many, allocFlags, defaultVal, ... }` covering data memory (`IMEM`), code words, knobs, inputs, outputs, FIFO DMA channels (`IN_FIFO`, `OUT_FIFO`), triggers, and fixed hardware registers (`types 10..18`). |
| `DRLC` (`8` B/entry) | Code relocation records (`441` entries across the 73 files): `{ u16 rsrcIndex, u16 part, u8 reserved, u8 codeHunk, u16 codeOffset }`. Patches allocated DMEM addresses or FIFO registers into `DCOD` instruction words. |
| `DNMS` | NUL-separated ASCII symbol table naming every `MRSC` port/knob (`"Amplitude"`, `"SampleRate"`, `"Input"`, `"Output"`, `"InFIFO"`, etc.). |
| `DCOD` | Big-endian 16-bit DSPP microcode words. |
| `DINI` | Initial data memory values applied when the instrument is allocated or started. |
| `DLNK` | Dynamic link dependency table naming shared subroutine templates (`sub_envelope.dsp`, `sub_sampler_16_v1.dsp`). |

### Key DSP Templates Used by `driving/game`

| Template File | Function in `driving/game` |
|---|---|
| `nanokernel.dsp` | Supervisor frame loop and multi-rate dispatcher. |
| `line_out.dsp` | Final stereo accumulator write to `OutputToDAC` (`0x2E0..0x2E1`). |
| `sampler_16_f1.dsp`, `sampler_16_f2.dsp` | Fixed-rate (`44.1 kHz`) 16-bit mono/stereo PCM samplers (`SoundSpooler` FMV audio). |
| `sampler_16_v1.dsp`, `sampler_16_v2.dsp` | Variable-pitch (`SampleRate` knob) 16-bit mono/stereo PCM samplers (`.imsasound` PCM banks). |
| `sampler_sqs2_f1.dsp`, `sampler_sqs2_v1.dsp` | Fixed-rate and variable-pitch SQS2 2:1 compressed samplers (`.imsasound` SQS2 banks). |
| `sampler_cbd2_f1.dsp`, `sampler_cbd2_f2.dsp`, `sampler_cbd2_v1.dsp`, `sampler_cbd2_v2.dsp`, `sampler_cbd2_v4.dsp` | Fixed-rate (`f1` mono, `f2` stereo) and variable-rate (`v1, v2, v4`) **CBD2** 2:1 compressed samplers (`SoundPlayer` `.cbd2` music and voiceovers). |
| `sampler_drift_v1.dsp`, `delay_f1.dsp`, `delay_f2.dsp`, `filter_3d.dsp` | Used by the 11-block `Sound3D` spatializer (`0x6d`) to compute interaural time delay (ITD), Doppler pitch shift, and head-shadow filtering (`filter_3d.dsp`). |
| `svfilter.dsp` | State-variable resonant filter (low-pass, band-pass, high-pass). |
| `envelope.dsp` | Piece-wise linear ADSR / multi-point envelope generator (`sub_envelope.dsp`). |
| `schmidt_trigger.dsp` | Threshold comparator firing a DSPP hardware trigger (`ArmTrigger`). |
| `add.dsp`, `subtract.dsp`, `multiply.dsp`, `times_256.dsp`, `timesplus_noclip.dsp` | Arithmetic building blocks composed inside `CreatePatchTemplate` (`5:0`) graphs. |
| `mixer` (`CreateMixerTemplate`) | Runtime-generated $N \times M$ gain matrix (`MakeMixerSpec(inputs, outputs, flags)`). |

## Imported Audio Folio (`folio 6`) and AudioPatch (`folio 5`) Selectors

### Folio 5: `audiopatch` (`System.m2/Modules/audiopatch`)

| Ordinal | Function | Description |
|---|---|---|
| `5:0` | `CreatePatchTemplate(const PatchCmd *cmds, const TagArg *tags)` | Compiles a `PatchCmd` graph (`PC_ADD_TEMPLATE`, `PC_DEFINE_PORT`, `PC_DEFINE_KNOB`, `PC_CONNECT`, `PC_EXPOSE`, `PC_SET_CONSTANT`) into a reusable instrument template `Item`. Used by `Sound3D` and custom effects. |

### Folio 6: `audio` (`System.m2/Modules/audio`)

`driving/game` imports 24 selectors from the Audio Folio (`CreateInstrument`, `CreateSample`, and `CreateAttachment` are Kernel Folio `CreateItem` `1:119` macros over audio node types):

| Ordinal | Selector | Role |
|---|---|---|
| `6:8` | `CreateMixerTemplate` | Builds a mixer instrument template from a `MixerSpec`. |
| `6:17` | `UnloadInstrument` | Unloads a loaded instrument template. |
| `6:19` | `GetAudioResourceInfo` | Queries an audio resource/item descriptor. |
| `6:23` | `GetCueSignal` | Returns the task signal mask associated with an `AudioCue`. |
| `6:27` | `LoadInstrument` | Loads an instrument template from the disc. |
| `6:28` | `GetAudioItemInfo` | Queries an audio Item's attributes. |
| `6:32` | `LoadInsTemplate` | Loads a patch/instrument template definition. |
| `6:35` | `GetAudioTime` | Reads the current audio clock. |
| `6:37` | `GetAudioFrameCount` | Reads the audio frame counter. |
| `6:38` | `ReadProbePart` | Reads a probed DSPP output port. |
| `6:41` | `AbortTimerCue` | Cancels a scheduled timer cue. |
| `6:44` | `ResumeInstrument` | Resumes a paused instrument. |
| `6:45` | `PauseInstrument` | Pauses an executing instrument. |
| `6:50` | `MonitorAttachment` | Binds a cue to an attachment's end-of-buffer event. |
| `6:51` | `LinkAttachments` | Links two attachments for gapless streaming. |
| `6:54` | `StartAttachment` | Starts an attachment. |
| `6:57` | `ConnectInstrumentParts` | Connects an output part to an input/knob part. |
| `6:58` | `SignalAtAudioTime` | Schedules a signal at an audio-clock deadline. |
| `6:59` | `DisconnectInstrumentParts` | Breaks a connection made by `6:57`. |
| `6:60` | `ReadKnobPart` | Reads a knob array element. |
| `6:67` | `StopInstrument` | Stops an instrument and its attachments. |
| `6:68` | `ReleaseInstrument` | Enters the instrument's release phase. |
| `6:69` | `StartInstrument` | Starts an instrument and its auto-start attachments. |
| `6:70` | `SetKnobPart` | Sets a knob array element. |

## DMA Streaming, Looping, and `Sound3D` Architecture

### Sample Attachments (`AudioAttachment`) and Loop Semantics

When a sample is attached to a sampler instrument (`CreateAttachment`), the FDMA channel follows one of three progression modes:
1. **One-Shot (`SustainBegin == -1`)**: Plays from byte `0` to `AF_TAG_NUMBYTES`. If linked via `LinkAttachments(att1, att2)` (`6:51`), switches to `att2` and fires `att1`'s `MonitorAttachment` (`6:50`) cue signal so `SoundSpooler` / `SoundPlayer` can refill `att1`. If unlinked and `AF_ATTF_FATLADYSINGS` is set, stops the owning instrument at buffer end.
2. **Sustain Loop (`SustainBegin >= 0, SustainEnd > SustainBegin`)**: Loops continuously between frame `SustainBegin` and `SustainEnd` while the instrument is running (used by engine RPM and tire skid loops in `.imsasound`).
3. **Release Phase (`ReleaseInstrument` `6:68`)**: Exits the sustain loop and either plays to end or enters the `ReleaseBegin..ReleaseEnd` loop.

### `Sound3D` Spatializer (`0x6d`)

Opponent car engines and trackside audio sources use the `Sound3D` library (`OpenSound3D` -> `CreatePatchTemplate`), which builds an 11-block DSP patch combining:
- A 200-byte supervisor `DelayLine` read by two variable-drift delay taps (`sampler_drift_v1.dsp`) to model left/right ear path-length differences (ITD) and radial velocity Doppler shift.
- Two `filter_3d.dsp` blocks modeling high-frequency head-shadow attenuation (IID/HRTF).
- A `schmidt_trigger.dsp` block armed via `ArmTrigger` to synchronize delay tap wrap-arounds with CPU position updates.

## Host Interpreter

See [DSPP Interpreter](../recompilation/shims.md#dspp-interpreter) for how the host runs these templates.
