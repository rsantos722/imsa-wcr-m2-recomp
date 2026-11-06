# Architectural Scope and Known Limitations

## Static Recompilation vs. Hardware Emulation

The project is an ahead-of-time (AOT) static binary recompiler and Portfolio OS shim layer built specifically for the *IMSA World Championship Racing* NTSC prototype disc (`LaunchMe.m2`, `driving/game`, and `shooter2d/shooter2d`), rather than a general-purpose 3DO M2 emulator:

- Executable identities and SHA-256 hashes are verified at configure time and during [startup image loading](runtime.md#startup-image-loading-and-relocation-fixup) against [`launcher/disc-manifest.json`](../../launcher/disc-manifest.json). Modified or unrecognized binaries are rejected.
- Recompiled PowerPC code executes at native host CPU speed rather than simulating instruction cycles, pipeline stalls, or cache misses of the 66.67 MHz PowerPC 602. Execution is paced at synchronization boundaries: NTSC VBL, the 16,666 µs physics metronome, the PowerPC Time Base, and the audio sample clock (see [Timing Model](timing-model.md)).
- Guest tasks share a single 32-bit big-endian memory buffer (`c->mem`) with bounds checking. PowerPC supervisor-mode MMU, BAT registers, and page tables are not simulated.

## Graphics and Triangle Engine Approximations

- Destination Blender (DB) modes—including alpha blending, additive blending, decals, color modulation, SSB/alpha discard, and 16-bit depth testing—are evaluated per draw call in GLSL fragment shaders. Mid-scanline register writes or arbitrary bitwise ALU modes (`DBALUCNTL`) are not emulated.
- Real M2 hardware can read arbitrary system RAM as the background source during blending. In OpenGL, when `DBSRCBASEADDR` references a guest bitmap (such as during screen wipes or rear-view mirror overlays), the renderer samples a cached framebuffer texture snapshot rather than reading unified RAM directly.
- Because both 3D scenery and 2D HUD/menu cels are submitted as screen-space triangles in shared command lists, raising the internal render scale (`M2_RENDER_SCALE=2..8`) sharpens 3D geometry while 2D sprite pairs may show minor texel alignment seams unless `M2_TEXTURE_FILTER=guest` or `sharp` is used.

## FMV

When built with `-DIMSA_WITH_FFMPEG=OFF`, the MPEG decoder shim reports unavailable and the game skips video sequences.

## Savestate (`M2SV` / `M2EX`) Continuation Scope

Diagnostic savestates are supported via `--savestate-save PATH` and `--savestate-load PATH`:

- `M2SV` (`kMagic = 0x4D325356`, v1) stores the outer register header, sparse 4 KiB page allocation bitmap, and guest RAM pages. `M2EX` (`M2_EXTRA_MAGIC = 0x4D324558`, v23) stores kernel item tables, task states, and synchronization primitives.
- Savestates capture task state at top-level synchronization boundaries (such as `StateDriver` VBL waits) and cannot reconstruct arbitrary mid-function C call stacks on worker threads.
- Player settings, high scores, and ghost laps are saved through the virtual MicroCard directory (`/card`) rather than diagnostic savestates.

## Floating-Point Semantics

- The `frsqrte` host helper meets the PowerPC Architecture relative error bound ($|e| \le 1/32$) without replicating the undocumented PowerPC 602 hardware seed lookup table (see [PowerPC 602](../m2/ppc602.md#reciprocal-square-root-accuracy-and-open-hardware-questions)).
- PowerPC `FPSCR` exception traps are mapped to standard host IEEE-754 single-precision semantics.
