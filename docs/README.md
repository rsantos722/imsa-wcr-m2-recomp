# Documentation

## Reading addresses and selectors

- Unless a page says otherwise, `0x2fa4c` and `.text+0x2fa4c` are offsets into the `.text` section of `driving/game`. Add `0x100000` to get the guest address (`0x12fa4c`). See the [guest address map](recompilation/runtime.md#guest-virtual-address-space-map) for the other images and sections.
- `1:166` is host folio 1, ordinal 166. Host folio IDs are listed under [Selector Dispatch Architecture](recompilation/shims.md#selector-dispatch-architecture). Tables that already name a folio may list just the ordinal as `#166`.
- `.imp3do` module IDs are local to each binary and differ from host folio IDs. Pages label them as module IDs.

## Game Internals

| Page | Covers |
| --- | --- |
| [boot.md](game/boot.md) | Disc shell handoff, C runtime startup, and shutdown. |
| [state-machine.md](game/state-machine.md) | `StateDriver`, state transitions, track loading, and what runs each frame. |
| [menus.md](game/menus.md) | Menu input, the 3D showroom, saved preferences, and the in-race HUD. |
| [race-rules.md](game/race-rules.md) | Rolling starts, laps, standings, replays, and post-race scoring. |
| [tracks.md](game/tracks.md) | The shipped tracks, their quirks, and the Skid Pad secret triggers. |
| [physics-and-aero.md](game/physics-and-aero.md) | The rigid-body simulation, controls, car models, powertrain, and aerodynamics. |
| [tires-and-suspension.md](game/tires-and-suspension.md) | Suspension, tire grip and wear, brakes, and steering geometry. |
| [collision.md](game/collision.md) | Car-to-car impacts, the track surface mesh, wall bumpers, and cones. |
| [ai.md](game/ai.md) | How opponents choose states, pass, pick targets, and drive. |
| [camera.md](game/camera.md) | Camera modes, ground clamping, and the math libraries. |
| [mercury.md](game/mercury.md) | Scene culling, Mercury PODs, and how the game submits graphics. |
| [audio.md](game/audio.md) | Sound effects, engine audio, 3D sound, music, and video playback threads. |
| [shooter2d.md](game/shooter2d.md) | The companion game's frame loop, waves, assets, music, and saves. |
| [unfinished-features.md](game/unfinished-features.md) | Dormant features and which ones can be restored. |

## Panasonic M2 Platform

| Page | Covers |
| --- | --- |
| [ppc602.md](m2/ppc602.md) | The CPU, the compiler ABI, and open questions about `frsqrte` accuracy. |
| [portfolio-os.md](m2/portfolio-os.md) | Items, modules, errors, memory, scheduling, messaging, and device I/O. |
| [operafs.md](m2/operafs.md) | The File Folio, mounted volumes, block I/O, and MicroCard saves. |
| [triangle-engine.md](m2/triangle-engine.md) | The Triangle Engine device, command list format, and registers. |
| [dsp-audio.md](m2/dsp-audio.md) | The DSP hardware, instrument templates, and audio APIs. |
| [mpeg-fmv.md](m2/mpeg-fmv.md) | The MPEG video device and audio decoder APIs. |
| [iff.md](m2/iff.md) | The IFF parser API, chunk framing, and guest structures. |

## File Formats

| Page | Covers |
| --- | --- |
| [operafs.md](formats/operafs.md) | The disc's filesystem layout and how to extract it. |
| [kernel-opera.md](formats/kernel-opera.md) | Boot images, M2 ELF modules, and LZSS compression. |
| [cbd2.md](formats/cbd2.md) | CBD2 compressed audio, used for music and voiceovers. |
| [imsasound.md](formats/imsasound.md) | `.imsasound` sample files and SQS2 compression. |
| [mov.md](formats/mov.md) | `.mov` DataStream video files. |
| [rgb.md](formats/rgb.md) | Raw RGB still images and developer screenshots. |
| [memimage.md](formats/memimage.md) | `.memimage` bundles, textures, fonts, and `Message.cel`. |
| [track.md](formats/track.md) | Track meshes, authoring scripts, compiled track data, and auxiliary files. |

## Recompilation

| Page | Covers |
| --- | --- |
| [translation.md](recompilation/translation.md) | How PowerPC code becomes C, and what the recompiler generates. |
| [runtime.md](recompilation/runtime.md) | Guest memory layout, image loading, function dispatch, and tasks. |
| [shims.md](recompilation/shims.md) | How OS calls reach host code, and the DSPP interpreter. |
| [timing-model.md](recompilation/timing-model.md) | How display, physics, and audio clocks are paced. |
| [rendering.md](recompilation/rendering.md) | Command list interpretation, rendering backends, and presentation. |
| [limitations.md](recompilation/limitations.md) | Where the recomp differs from real hardware. |
| [tire-bench.md](recompilation/tire-bench.md) | Measuring the original tire model in isolation. |
| [analog-steering.md](recompilation/analog-steering.md) | The optional live analog steering controller. |
| [environment.md](recompilation/environment.md) | Every environment variable (generated). |
| [Diagnostics](../AGENTS.md#diagnostics) | Control sessions, traces, observation settings, and unattended races. |

## External references

- [3DO M2 Mercury Programmer's Guide v2.7, part 2](https://3dodev.com/_media/documentation/m2_releases/3do_m2_v2.7_-_part_2_-_mercury_programmer_s_guide.pdf)
- [3DO M2 Command List Toolkit v2.7](https://3dodev.com/_media/documentation/m2_releases/3do_m2_v2.7_-_part_3_-_command_list_toolkit.pdf)
- [Portfolio OS v3.0 source archive (trapexit/portfolio_os_m2)](https://github.com/trapexit/portfolio_os_m2)
- [PowerPC 602 User's Manual (MPR602UMU-01)](https://zxgit.org/RomanRom2/awesome-cpus/raw/commit/8115b6c97d547c13037b0cfcb0125d09a3e90543/PowerPC/PowerPC_602/manual.pdf)
- [NXP PowerPC Floating-Point Documentation (MPCFPE-AD/R1)](https://www.nxp.com/docs/en/user-guide/MPCFPE_AD_R1.pdf)
