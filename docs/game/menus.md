# Menus, 3D Showroom, HUD, and Preferences

## UI States and Video Screens

`StateDriver` (`0x2fa4c`) coordinates the screens using `phase` (`0x1877d8`) and `state` (`0x1877dc`). Primary phase `1` owns 2D menus, phase `2` owns the 3D showroom, and phase `3` calls the [race-session controller](race-rules.md#race-session-lifecycle-and-racephase-0x2f1d4). Secondary state `29` is Configure Car (`UI_DrawConfigCar`).

`UI_LoadArt` supplies the MPEG still backgrounds for phase 1. Configure Race (secondary state `28`) also plays a 204×128 track-preview video at `(66, 250)`: the filename table at `0x17d884..0x17d898` selects `DriveThru.Testona.mov`, `DriveThru.Laguna.mov`, `DriveThru.NewOrleans.mov`, or `dummy.mov` for indices `3..5`. Intro and outcome videos use `VideoPlayer` (`0x3c27c`); see [DataStream Video Playback](audio.md#datastream-video-playback) and [`.mov` Format](../formats/mov.md).

## Menu Input and `doButtonMenu` (`0x2ee5c`)

`doButtonMenu` (888 B, 222 insns in `state.c`) processes directional pad and button inputs for all 2D and overlay menus using the input bitmask translated by `StateDriver` via lookup tables `0x17C718` (digital control pad) and `0x17C750` (analog joystick).

### Menu Descriptor Layout
Each menu screen passes a 12-byte descriptor struct to `doButtonMenu`:

| Offset | Type | Field | Description |
|---|---|---|---|
| `+0x00` | `u8` | `num_options` | Total number of vertical menu rows |
| `+0x01` | `u8` | `flags` | Menu behavior flags |
| `+0x02` | `u8` | `disabled_mask` | Bitmask of disabled/unselectable rows (`1 << row`) |
| `+0x03` | `u8` | `cursor` | Currently highlighted row index (`0 .. num_options - 1`) |
| `+0x04` | `u8*` | `max_limits` | Pointer to per-row maximum value array `max_limits[num_options]` |
| `+0x08` | `u8*` | `values` | Pointer to per-row current value byte array `values[num_options]` |

- **Vertical Navigation (UP / DOWN):** Increments or decrements `cursor` modulo `num_options`, automatically stepping over any row whose bit is set in `disabled_mask`. Implements key-repeat via counter `0x17da20` (immediate step on initial press, followed by a 30-tick repeat delay).
- **Horizontal Value Adjustment (LEFT / RIGHT):** Decrements or increments `values[cursor]`, clamping within `[0, max_limits[cursor]]`, and sets the UI redraw dirty flag at `0x17d8b8` (`.data+0x7848`).

## State 2 Interactive 3D Car Showroom (`showroom.c`)

Entering Primary State 2 (`Secondary 25` for Car/Setup selection, `Secondary 26` for Track confirmation) initializes the interactive 3D showroom (`showRoomInit` at `0x244d4`, `showRoomIterate`, `SR_ShowMerc`).

- **Shared Mercury POD Pipeline:** `SR_ShowMerc` feeds the standard Mercury pipeline (`MS_addcar_showroom` at `0x23fdc` -> `MS_addgroup` at `0x23888` -> `M_Sort` at `0x60098` -> `M_SetCamera` / `M_Draw` at `0x50e34`).
- **Showroom Geometry and Assets:**
  - Loads 4 chassis body models (`showroom.%d`, indices `0..3` for the Courage, Ferrari, Kudzu, and Riley & Scott) and 4 tire models (`showroom.tire.%d`).
  - Renders the 3D showroom floor `Pod` at `0x99ef6c` (`PodGeometry` at `0x99f4e8`, `1,296` vertices spanning `(-500, -31.4, -500)` to `(500, -31.4, 500)`) and surrounding room wall `Pod`s at `0x99eeb8`.
  - Applies per-car wheel mount matrices at descriptor `+0x30..+0x3c` (including the Courage rear wheel Y/Z scale factor of `0.856`).
- **Turntable Rotation:** After an initial ~10-frame camera settle, the camera basis remains stationary while the shared scene-root matrix rotates by ~2 degrees per frame so the car, showroom floor, and walls revolve together as a turntable.

## Configuration Accessors and MicroCard Persistence (`IMSAprefs`)

`state.c` (`0x31718+`) exposes typed accessor functions queried across physics, AI, audio, and rendering:
- **Race and Track:** `UI_GetTrack` (`0x31718`), `UI_IsSecretTrack` (`0x3179c`), `UI_GetNumLaps`, `UI_GetOpponent`, `UI_IsFog`.
- **Car Setup and Driving Aids:** `UI_GetBody`, `UI_GetEngine`, `UI_GetTires`, `UI_GetGears`, `UI_GetWing`, `UI_IsAutoTrans`, `UI_IsTraction` (`0x31a14`), `UI_IsAntilockBrakes` (`0x31a3c`), `UI_IsAutoBrakes` (`0x31a64`), `UI_IsArcadeMode`.
- **Audio, Controls, and HUD:** `UI_GetMusicVolume`, `UI_GetSfxVolume`, `UI_GetSoundTrack`, `UI_GetCpad` (`0x31b00`), `UI_IsTrackMapOn` (`0x31bf0`), `UI_IsProximityMapOn` (`0x31c18`), `UI_IsRearMirrorOn` (`0x31c58`).

### `IMSAprefs` Layout (`loadPrefs`, `0x2f778`; `savePrefs`, `0x2f82c`)

The guest queries `FindMicroCard` for a writable volume and reads/writes `/card/IMSAprefs` (sharing its host backing directory with the launcher's save manager). The payload consists of three contiguous `.data` chunks totaling 56 bytes:

| File Offset | Size | Memory Address | Backing Menu / Contents |
|---|---|---|---|
| `0x00..0x06` | 7 B | `0x17d96f` | **Chunk 1 — Configure Race (`0x17d980+8`, option counts at `0x17d968`):** Track (`0x17d96f`), laps, opponents, conditions, race mode, auto-braking, and car (default vs customized setup) |
| `0x07..0x2e` | 40 B | `0x17d9c8` | **Chunk 2 — Config Car customized setups:** Five per-track 8-byte records, each containing body, engine, tires, gear ratios, wing angle, transmission (`Auto/Manual`), traction control, and ABS |
| `0x2f..0x37` | 9 B | `0x17d915` | **Chunk 3 — Gameplay, HUD & Audio Options (`optionsMenu` at `0x17d928+8`):** Byte 0 (`0x17d915`) = Damage, Byte 1 (`0x17d916`) = Track Map, Byte 2 (`0x17d917`) = Proximity Map, Byte 3 (`0x17d918`) = Rear-View Mirror, Byte 4 (`0x17d919`) = Pit Radio, Byte 5 (`0x17d91a`) = Soundtrack, Byte 6 (`0x17d91b`) = Music Volume, Byte 7 (`0x17d91c`) = SFX Volume, Byte 8 (`0x17d91d`) = Controller Preset (`0..2`) |

## In-Race HUD Overlays (`game.c`)

At the end of every race frame (`trackIterate` at `0x190c4`), `game.c` composites the 2D/3D cockpit and telemetry overlays over the 3D viewport:

1. **Rear-View Mirror (`RenderMirrorFrame` at `0x19a84`):** Gated by `UI_IsRearMirrorOn`. Re-renders the scene behind the player car into the top mirror viewport.
2. **Proximity Radar (`MS_Radar_Set`, `RenderRadarFrame` at `0x19c08`):** Gated by `UI_IsProximityMapOn` and automatically suppressed on Skid Pad (`UI_GetTrack() == 4`). Displays nearby opponent positions relative to the player.
3. **Track Map (`RenderMap` at `0x1470c`, 668 B, called at `0x19fd0`):** Gated by `UI_IsTrackMapOn`. Plots the track ribbon and moving car dots.
4. **Cockpit Dashboard, Tachometers, and Steering Wheel:**
   - `DrawDashBoard` (`0x153b8`, 3,284 B, 138 FP ops, called twice per frame) and per-car tachometers `DrawTach1..4` (`0x14a24..0x15000`, warping needle/gauge quads via `p1..p6` and `uv1..uv6`).
   - `RenderWheel` (`0x16590`, 248 B): Rotates and draws the cockpit steering wheel sprite to match current steering angle.
   - `DrawInfoBar` (`0x1608c`, 1,284 B): Renders lap counter, race/lap/split clocks, speed, and race standing. It also draws the exterior-camera tachometer arc from the player RPM in `gMainSimOut+0xf8`; the caller passes the gauge scale in `f1` as the shipped constant `0.015` (180 units over 12,000 RPM). Because the four engines have different redlines (`plyrEng+0x00`: 11,500 RPM for the Ferrari V12 vs. 8,500 RPM for the other three), the fixed scale only lets the Ferrari reach the arc's yellow zone. The default-on `M2_HUD_TACH_FIX` rescales `f1` from that redline; the cockpit gauge face is already per-car and is not affected.

### HUD Cycling (`UI_NextToolOption`, `0x31ca8`)
Pressing the HUD toggle button during a race (action bit `0x08` at `trackIterate` `0x19328`) calls `UI_NextToolOption`, which cycles the three HUD enable bytes (`0x17d916` Track Map, `0x17d917` Proximity Map, `0x17d918` Rear-View Mirror) through an 8-state binary ripple counter:
- **Track Map (`0x17d916`)** toggles on every press.
- **Proximity Map (`0x17d917`)** toggles whenever Track Map wraps back to `1`.
- **Rear-View Mirror (`0x17d918`)** toggles whenever both Track Map and Proximity Map wrap to `1`.
