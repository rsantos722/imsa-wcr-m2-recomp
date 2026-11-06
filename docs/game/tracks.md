# Tracks, Loading, and Secret Triggers

## Shipped Tracks on Disc

The prototype disc ships six playable tracks under `driving/data/tracks/`, resolved through `UI_GetTrack` (`0x31718`) and the 7-slot path table at `0x17C780` in `trackInit_1` (`0x166dc`):

| `0x17C780` Slot | `UI_GetTrack()` / `TKID` | Directory & Prefix | Display Name | `.memimage` Size | `.qmf` Verts | `NMPT` (`SIZE`) | `TARG` | `ANGL` | `IVEL` `(v0, v1, v2)` | Start Rows |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | `0` | `testona/ra_test` | **300 World Speedway** | 1,702,032 B | 1,152 | 144 (147) | 137 | `180.0` | `(4, 25, 140)` | 24 |
| 1 | `1` | `laguna/ra_laguna` | **Laguna Seca** | 2,309,796 B | 2,327 | 404 (409) | 389 | `-53.0` | `(7, 25, 125)` | 36 |
| 2 | `2` | `no/ra_no` | **New Orleans** | 1,877,064 B | 1,104 | 276 (279) | 269 | `80.0` | `(0, 0, 50)` | 36 |
| 3 | `3` | `suzuka/ra_suzuka` | **Suzuka** | 2,132,668 B | 2,428 | 345 (348) | 339 | `122.0` | `(2, 10, 150)`\* | 36 |
| 4 | `4` | `skidpad/ra_skidpad` | **Skid Pad** | 811,436 B | 78 | 11 (14) | 0 | `180.0` | `(0, 0, 0)` | 12 |
| 5 | *(unmapped)* | `testona_night/ra_test` | *(Unshipped Night Oval)* | *Absent on disc* | — | — | — | — | — | — |
| 6 | `0` (`UI_IsSecretTrack()==1`) | `leary/ra_leary` | **Psychedelia** | 1,631,692 B | 1,152 | 144 (147) | 137 | `180.0` | `(4, 25, 140)` | 24 |

*\*Note on Suzuka `IVEL`: the human-readable `ra_suzuka.data` text file lists `IVEL (4, 25, 50)`, whereas the compiled `ra_suzuka.memimage` binary shipped on disc stores `(2.0, 10.0, 150.0)`.*

The result-speed `trackLengths` array at `0x17C79C` holds ten floats, of which the first six correspond to the shipped tracks.

### Track Notes and Special Mechanics

- **300 World Speedway (`testona`):** Oval inspired by Daytona, with the start/finish line on the backstretch. Uses special ground-height snapping (`UI_GetTrack() == 0`) in `get_height` (`0x79f0`). The drive-through preview video still refers to the track as "Testona" instead of 300 World Speedway.
- **Laguna Seca (`laguna`):** Full road course (~3,200 × 2,700 world units, elevation range `-4..150` units).
- **New Orleans (`no`):** A recreation of the New Orleans Street Circuit used by IMSA in 1992 and 1995 around the Louisiana Superdome, a circuit that never officially appeared in a released commercial racing game.
- **Suzuka (`suzuka`):** `2,428` QMF vertices, the largest physics mesh on disc. Contains the only animated trackside scenery prop in the game, the Suzuka Ferris Wheel:
  - **Asset Binding and Export (`tdp+0x84`):** `ra_suzuka.memimage` exports the Ferris wheel Mercury `Pod` at payload offset `0x181e40` (export table slot 6). During `trackInit_1` (`0x166dc`), `getnextmemimagepointer` (`0x1f8e0`) loads this pointer into `fwheel` (`.bss+0x87c`) and stores it into `tdp+0x84` (offset `+0x84` in the Track Data Block). Suzuka is the only track with a non-zero pointer at `tdp+0x84` (`NULL` on all other five tracks). In `trackInit_2` (`0x1726c`), `MS_UpdateLoadedPodLodGroup` (`0x258f8`) prepares its LOD hierarchy, and `M_Support.c` reserves `gFerrisPodList` (`.bss+0x6424`) for scene-graph rendering.
  - **Yaw Alignment (`EF_InitEFObjects`, `0x12a74` in `effects.c`):** During track effect initialization (`trackInit_2 -> EF_InitEFObjects`), if `tdp+0x84 != NULL`, the game inspects the wheel pod's transform matrix (`pod->0x24->0x14`) and computes its fixed trackside heading angle `fwheel_angle = atan2f(matrix[6], matrix[8])` (stored at `.bss+0x838`).
  - **Per-Frame Rotation (`EF_TurnFWheel`, `0x12974` in `effects.c`):** In the per-frame effect iterate loop (`EF_EFObjectIterate` at `0x13c68`), if `tdp+0x84 != NULL`, the game dispatches to `EF_TurnFWheel(tdp+0x84)`:
    - Increments a static running rotation angle at `.data+0x4870` by `+0.015f` rad/tick (`~51.6°/s` at 60 Hz, wrapping modulo $2\pi$ for a full revolution every ~7.0 seconds).
    - Seeds a 48-byte transformation matrix from the identity matrix `Ident` (`.data+0x48c8`) on the local stack.
    - Applies track yaw alignment via `turnY(matrix, fwheel_angle)` (`0x128e4`).
    - Applies wheel rotation around its spindle axis via `turnZ(matrix, angle)` (`0x12854`).
    - Copies the resulting 36-byte 3×3 rotation matrix directly into the Mercury pod transform (`pod->0x24->0x14`) using kernel `memmove` (`1:31`, `M2_KSEL_MEMMOVE`).
- **Skid Pad (`skidpad`):** Open test facility (`TKID 4`). Disables lap counting (`ProcessCheckPoints` at `0x3364`), proximity radar (`trackIterate`), and post-race FMVs. Auto-braking is forced off here, and the default car setup disables traction control (see [Per-Track Driving Assists](#per-track-driving-assists)).
- **Psychedelia (`leary`):** Hidden bonus track (UI selector index `5`, mapped by `UI_GetTrack` to `0` while `UI_IsSecretTrack` at `0x3179c` returns `1` so `trackInit_1` selects slot `6` of `0x17C780`). Shares the centerline (`NMPT = 144`, `TARG = 137`) and `1,152`-vertex QMF collision mesh of 300 World Speedway (`testona`), re-textured with rainbow/psychedelic art (`ra_leary.memimage`). Can also be booted directly via `-Tdata/tracks/leary/ra_leary`. Its internal directory name (`leary`) is likely a nod to Timothy Leary.

## Track Loading

`trackInit_1` (`0x166dc`) selects a path prefix, loads `<track>.memimage`, and stores its first exported payload root in `tdp` (`.bss+0x3a58`). `trackInit_2` (`0x1726c`) constructs the scene and physics world; see [Track Initialization](state-machine.md#track-initialization-trackinit_1-0x166dc-trackinit_2-0x1726c), the [`.memimage` container format](../formats/memimage.md#memimage-binary-container-layout), and [Mercury POD structures](mercury.md).

## Skid Pad Trigger Volumes

On track 4, `trackIterate` (`0x1a454..0x1a5c0`) tests the live position `(X,Y,Z)` at `0x181e10/14/18` against three L1 (Manhattan-distance) octahedral regions in the following order:

- `|X - 818| + |Y - 1| + |Z + 1829| < 50`: **`shooter2d` Launcher:** Closed garage bay immediately to the right of the open garage door. Driving or clipping into this area calls `doTrackTearDown` (`0x2e6d8`) and terminates `driving/game` with `exit(1)` (`1:8`), which causes `LaunchMe.m2` to boot `shooter2d/shooter2d` (see [Child Exit Handoff](boot.md#child-exit-handoff) and [`shooter2d` Reference](shooter2d.md)).
- `|X - 705| + |Y - 1| + |Z + 1916| < 30`: **Secret Track Unlock (Psychedelia):** Rear-left corner inside the garage bay. Calls `activatesecrettrack` (`0x2e578`), setting the Choose Track menu limit `0x17D968 = 6` to unlock Psychedelia (`ra_leary`).
- `|X - 706| + |Y - 1| + |Z + 1822| < 50`: **In-Session Car Swap Garage:** Open garage bay door. Returns `1000` from `ViewTrack(2)` / `trackIterate`, advancing `racePhase` from inspection (Secondary State `17`) to Secondary State `33` (`0x21`, interactive car setup via `createNewCar` at `0x1a718`; also reachable from the pause menu).

## Per-Track Driving Assists

The driving-assist accessors follow two pointers stored in the Configure Race and Config Car menu records (`0x17d988` for Configure Race and `0x17da08` for whichever 8-byte record Config Car currently selects):

| Menu Record | Pointer Cell | Current Values |
| --- | --- | --- |
| `configraceMenu` (`0x17d980`) | `+8` (`0x17d988`) | `configraceCurSub` (`0x17d96f`), 7 bytes: track, laps, opponents, conditions, mode, auto-braking, car |
| `configcarMenu` (`0x17da00`) | `+8` (`0x17da08`) | One 8-byte record: body, engine, tires, gears, wing, transmission, traction, ABS |

`setconfigcarpointer` (`0x2e588`) writes the car-setup pointer after `loadPrefs` and after each Configure Race menu update. It calls `UI_GetTrack` (`0x31718`) and selects:
- `configcarCurSub_default` (`0x17d9a0 + track*8`) when the Configure Race "Car" option (`configraceCurSub+6`) is `0`;
- `configcarCurSub_customized` (`0x17d9c8 + track*8`) otherwise.

The default table is baked into the image, while the customized table is the 40-byte chunk persisted by `savePrefs` (initialized in the shipped binary to the same five records as the default table). Selecting `1` on the Configure Race "Car" row enters the Config Car screen; `0` uses the baked-in default record. Because `UI_GetTrack` folds raw selector `5` (Psychedelia) onto `0`, Psychedelia uses the Testona car-setup record from either table.

| Accessor | Reads | Track-Dependent | Returns `1` When |
| --- | --- | --- | --- |
| `UI_IsAutoTrans` (`0x319ec`) | car setup `+5` | Indirectly, via the per-track setup record | stored value is `0` (automatic) |
| `UI_IsTraction` (`0x31a14`) | car setup `+6` | Indirectly, via the per-track setup record | stored value is `1` |
| `UI_IsAntilockBrakes` (`0x31a3c`) | car setup `+7` | Indirectly, via the per-track setup record | stored value is `1` |
| `UI_IsArcadeMode` (`0x31ba0`) | race config `+4` | No | stored value is `0` |
| `UI_IsAutoBrakes` (`0x31a64`) | race config `+0`, `+4`, `+5` | **Yes**, on the raw selector | raw track is not `0` or `4`, mode is not `1`, and auto-braking is `1` |

The shipped default car-setup table contains:

| Raw Selector | Track | Body | Engine | Tires | Gears | Wing | Transmission | Traction | ABS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `0` | 300 World Speedway | `1` | `2` | `1` | `2` | `0` | `0` (auto) | `1` (on) | `1` (on) |
| `1` | Laguna Seca | `3` | `0` | `2` | `0` | `4` | `0` (auto) | `1` (on) | `1` (on) |
| `2` | New Orleans | `0` | `1` | `0` | `1` | `3` | `0` (auto) | `1` (on) | `1` (on) |
| `3` | Suzuka | `2` | `3` | `3` | `1` | `2` | `0` (auto) | `1` (on) | `1` (on) |
| `4` | Skid Pad | `3` | `2` | `3` | `1` | `1` | `0` (auto) | `0` (off) | `1` (on) |
| `5` (uses `0`) | Psychedelia | `1` | `2` | `1` | `2` | `0` | `0` (auto) | `1` (on) | `1` (on) |

The first five bytes are remapped by `UI_GetBody`, `UI_GetEngine`, `UI_GetTires`, `UI_GetGears`, and `UI_GetWing`; the last three control driving assists. In the shipped defaults, Skid Pad is the only track that changes an assist (disabling traction control), while automatic transmission and ABS are enabled on every track.

Auto-braking is controlled separately by the Configure Race "Auto Braking" byte (`configraceCurSub+5`), gated by the raw track byte (`+0`) and mode byte (`+4`). `StateDriver` clears the Auto Braking enable bit in the Configure Race option mask (`configraceMenu+0x1c`) whenever `UI_GetTrack()` is `0` or `4` or `UI_IsArcadeMode()` is false, disabling the menu row on Testona, Skid Pad, and Psychedelia and outside arcade mode. However, because `UI_IsAutoBrakes` checks the raw selector (`track != 0 && track != 4`), a Psychedelia session (raw `5`) in arcade mode (`0`) with untouched options (`auto-braking == 1`) keeps auto-braking active even though its menu row is greyed out.
