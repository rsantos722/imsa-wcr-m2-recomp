# Track Data Formats (`.qmf`, `.data`, `<track>.memimage`)

Each of the six tracks in `driving/data/tracks/` (`laguna`, `suzuka`, `leary`, `no`, `testona`, and `skidpad`) pairs a text physics quad mesh (`ra_<track>_trck.qmf`), an ASCII authoring script (`ra_<track>_trck.data`), a compiled binary container (`<track>.memimage`), a visibility zone map (`.area`), and a recorded phantom lap (`*_phantom.dat`). See [Shipped Tracks on Disc](../game/tracks.md#shipped-tracks-on-disc) for path prefixes, vertex counts, and the unshipped night-oval slot.

## `.qmf` Quad Mesh File Format (`M2ReadQMFFile`)

Each `.qmf` file is a CRLF-terminated ASCII text file with trailing C-style `/* index */` comments, parsed at runtime by `M2ReadQMFFile` -> `read_qmf_file` -> `process_qmf_data` to build the track surface collision and physics quad strip.

### Header Line (Line 1)

```text
N,-1,-1,0.000000,0.000000,0.000000,0.000000,0.000000,0.000000
```

- `N`: Total number of vertex rows following the header.

### Vertex Rows (`N` lines, indexed `0 .. N-1`)

Each line has 10 comma-separated fields followed by `/* i */`:

```text
x,y,z,A,B,C,D,E,F,G /* i */
```

| Field | Type | Description |
|---|---|---|
| `x, y, z` | `float` | 3D world-space vertex coordinates (`y` is vertical elevation). Even/odd vertex pairs `(2k, 2k+1)` form the Left/Right cross-section edges of the track ribbon. |
| `A, B, C, D` | `int` | Adjacency / quad connectivity indices linking each vertex to the forward and neighboring quad strip vertices (`-1` when unlinked). |
| `E` | `int` | Auxiliary index (`0`). |
| `F` | `int` | Surface material ID. The shipped meshes use `16`..`19`, plus `29`..`31` on New Orleans; what each value means is not established. |
| `G` | `int` | Quad surface flags (`3`). |

## Track `.data` Token Script and `<track>.memimage` Binary Layout

At runtime, `readinmemimage` loads the compiled `<track>.memimage` bundle directly rather than parsing `ra_<track>_trck.data`.

### Token Sections in `ra_<track>_trck.data`

| Token | Meaning |
|---|---|
| `TKID` | Track identifier integer. |
| `COLR` / `LGHT` | Sky/horizon color and directional sun lighting vectors. |
| `POPD` | Scenery pop-in / LOD distance threshold. |
| `FLAG` / `HGHT` | Track rules flag word and camera/ground reference height. |
| `STRT` | Starting grid / pit lane spawn table (`6` floats per row: `x, y, z, pitch, yaw, roll`). |
| `DLTA` | Starting grid row spacing delta vector (`dx, dy, dz`). |
| `SIZE` | Array-local row count reused across multiple authoring arrays. |
| `NMPT` | Number of track centerline/camera points. |
| `TARG` | Integer starting target/checkpoint index. |
| `ANGL` | Initial orientation angle in degrees. |
| `IVEL` | Three-value initial-velocity tuple. |
| `TRKM` / `TRKI` / `TRKO` | Main, inner, and outer line tables, with eight 32-bit words per record. |

### Compiled Track Root (`tdp`)

In the generic [`.memimage` container layout](memimage.md#memimage-binary-container-layout), the first export entry points to payload offset `0` across all six shipped track bundles. After loading, `tdp` (`.bss+0x3a58`) points to that exported root. The offsets below are relative to `tdp`:

| Root Offset | Type / Meaning | Notes |
| --- | --- | --- |
| `+0x00` | `void *` (relocated) | Track boundary rail pointer consumed by `FindPoints` (`0x4594`) and `GetcpDist` (`0x4658`). |
| `+0x04` | `void *` (relocated) | Main `TRKM` centerline array (`32`-byte stride). |
| `+0x08` | `void *` (relocated) | Opposite track boundary rail pointer. |
| `+0x14` | `u32 NMPT` | Checkpoint count; copied into `GDP+0x21a0` at `0x2541c`. |
| `+0x18` | `void *` (relocated) | Relocated pointer equal to `TRKM + rows * 32` (end of counted `TRKM` records). |
| `+0x1c` | `u32 TARG` | Starting target checkpoint index (e.g., `137` on Testona). |
| `+0x20` | `f32 ANGL` | Starting angle in degrees (`180` on Testona, `-53` on Laguna, `122` on Suzuka). |
| `+0x44` | `u32 TKID` | Track identity (`0..4`; Psychedelia also stores `0`). |
| `+0x4c..+0x54` | `f32[3] IVEL` | Initial velocity vector; the first two floats seed `.bss+0x00` and `.bss+0x04`. |
| `+0x84` | `Pod *` (relocated) | Suzuka Ferris wheel (`0x181e40`, export slot 6); `NULL` on all other tracks (see [Track Notes and Special Mechanics](../game/tracks.md#track-notes-and-special-mechanics)). |

### Line Tables and Lane Fractions

`TRKM`, `TRKI`, and `TRKO` line records use a 32-byte stride containing `f32 x, y, z` at `+0x00..+0x08`, situation bits at `+0x0c`, and an authored lane fraction at `+0x10`:

| Track | `NMPT` | Authoring `TRKM` Rows | Authored Lane Range (`+0x10`, rounded) |
| --- | ---: | ---: | --- |
| Testona | 144 | 147 | `0.3961..0.8632` |
| Leary | 144 | 147 | `0.3961..0.8632` |
| Laguna | 404 | 409 | `0.1111..0.8890` |
| New Orleans | 276 | 279 | `0.1667..0.8333` |
| Suzuka | 345 | 348 | `0.2211..0.7851` |
| Skid Pad | 11 | 14 | `0.3984..0.6667` |

## Auxiliary Files (`.area`, `pathout.txt`, `*_phantom.dat`, `tweek.dat`)

- **`.area`**: ASCII hexadecimal bitmask table mapping track sectors to visible scenery object groups for frustum/zone culling.
- **`driving/data/pathout.txt`**: 828-row whitespace-separated float table recording reference racing-line coordinates and velocities.
- **`*_phantom.dat`**: Binary phantom lap, read by `repRestorePhantom` (`0x27250`) when the MicroCard has none. Laguna's file is 1,354,716 bytes, the Testona, Leary, New Orleans, and Suzuka files are 63,080 bytes, and Skid Pad has none.
- **`driving/data/tweek.dat`**: ASCII table of tuning values, four numbers per row (112 rows). `CarSimThread` reads only the first 50 rows; see [`CarSimThread`](../game/physics-and-aero.md#carsimthread-0xb054-2460-b-and-60-hz-metronome).
