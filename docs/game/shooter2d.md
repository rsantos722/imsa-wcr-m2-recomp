# `shooter2d` Runtime and Assets

The companion game is a separate M2 executable (`shooter2d/shooter2d`) translated using the same static recompiler and folio runtime.

## Module and Frame Boundaries

The [verified disc manifest](../../launcher/disc-manifest.json) records an 800,643-byte `ET_REL` ELF. Its raw ELF startup offset is `0x122c4`, while the host starts lifted `shooter2d.c::main` (`0x2a0`) at guest EA `0x002c02a0` (see [Startup Image Loading and Relocation Fixup](../recompilation/runtime.md#startup-image-loading-and-relocation-fixup)).

`LaunchMe.m2` switches to the shooter when `driving/game` returns exit status `1`, or `M2_BOOT_APP=shooter2d` launches it directly. Its working directory is `shooter2d` (passed via [`launch_data_root`](../../launcher/run_flags.h)), while system DSP templates come from the disc's `System.m2` tree.

`main` initializes folios, GState lists/bitmaps, controls, sprites, sound, wave paths, and scoring. Its frame loop starts at `.text+0x5dc`, submits a command list, waits for video/GState service, reads input, and updates enemies, bullets, the ship, waves, and score. Idle, active, death, and respawn paths run inside this loop, with [`m2_guest_shooter_frame_tick`](../../runtime/src/recomp_runtime.c) publishing frame progress at the loop head.

## Controls

`controls.c::GetCPortData` (`0x7208`) publishes held, released, and pressed raw pad masks from the EventBroker record, and `main` tests those bits directly. The shooter has no action table, so while it runs the host's Custom profile sets these bits from its own action list ([`input_map.cpp`](../../host/input/input_map.cpp)). The launcher's **Play shooter2d** button and the Skid Pad handoff both use that list.

| Pad Bit | Mask | Custom Action | Read | Effect |
| --- | --- | --- | --- | --- |
| `C` | `0x02000000` | shooter2d: Start | Pressed, game over | Starts a new game. The first game needs no input, because the game state (`0x2d2408`) and lives (`0x2d240c`) start at `1` and `3` in `.data`. |
| `A` | `0x08000000` | shooter2d: Fire | Pressed or held, in play | Fires; holding repeats at the current weapon interval. |
| `B` | `0x04000000` | shooter2d: Beam | Pressed | In play, calls `startbeam` (`0x5a48`) when the weapon level permits. After game over, sets the flag read at `0x834`, which calls `addnewwave` (`0xfef8`); starting a new game clears it. |
| D-pad | `0xf0000000` | Digital or Analog Up/Down/Left/Right | Held, in play | Feeds `movespaceship` (`0xf154`). |
| `Start` | `0x01000000` | Pause | Pressed, also while paused | Toggles pause. |
| `E` | `0x00080000` | None | Pressed, also while paused | Toggles pause. |
| `R` | `0x00400000` | Screenshot | Pressed, also while paused | Requests a screenshot. |
| `L` | `0x00200000` | None | Pressed, also while paused | Flips which frame parity starts a screenshot. |

All other Custom actions do nothing here. Profiles saved before the shooter2d actions existed copy Fire, Beam, and Start from Confirm (A), Back (B), and Cycle HUD displays on first load.

The shooter has no analog control. `GetCPortData` copies the pad's analog bytes only when its AnalogValid byte is nonzero, the host leaves that byte zero, and `main` never reads those fields. Custom's analog direction actions set their D-pad bit above 50% travel.

Pressing `R` makes `writerawimage` (`0x10288`) dump each of the two display Bitmaps on consecutive frames, as `screenshot.N` and `screenshot.N+1`. Each file is 248,832 bytes of headerless 576×216 big-endian RGB555, half the size of the racing game's [`Dsply_screenshot`](../formats/rgb.md#headerless-screenshot-variant-dsply_screenshot-0x124e8) dump because the shooter's `View` line-doubles its Bitmap. A failed open or write calls `exit(-1)`. The host writes the files and matching `.ppm` previews to `M2_SCREENSHOT_ROOT`.

## Asset Families

| Disc Path under `shooter2d/` | Use |
| --- | --- |
| `*.utf` | Sprite textures, including ship/car orientations, enemies, explosions, and upgrade animations; parsed through the texture/IFF path |
| `background.raw`, `clouds.even.raw`, `clouds.odd.raw`, `fog.even.raw`, `fog.odd.raw` | Background/layer image data |
| `bullet.raw`, `goodbullet.raw` | Small projectile raster data |
| `wavepattern.0` .. `wavepattern.3` | Authored enemy path samples |
| `shooter2d.S3M` | Tracker music played by the guest's `PLAY-S3M.c` code through the Audio Folio/DSPP (guest code opens lowercase `.s3m`; host file lookup is case-insensitive) |
| `badguydie.aifc`, `boing.aifc`, `powerup.aifc`, `youdie.aifc`, `youfire.aifc` | Sound effects |
| `Helvetica.24pt` | Font data opened through the Font Folio |

## Wave Paths

`waves.c::initwaves` (`0xfe5c`) loads the four numbered wave files. Each starts with a big-endian `u32` sample count followed by pairs of big-endian `float32` path coordinates:

| File | Bytes | Count in First Word | Trailing Bytes after Counted Pairs |
| --- | ---: | ---: | ---: |
| `wavepattern.0` | 2,468 | 308 | 0 |
| `wavepattern.1` | 3,484 | 434 | 8 |
| `wavepattern.2` | 7,044 | 879 | 8 |
| `wavepattern.3` | 6,492 | 810 | 8 |

`addnewwave` (`0xfef8`) selects a path and seeds a wave's emission interval and count; `walkwavelist` (`0x10094`) schedules enemies through `addwavebadguy` (`0x41b0`) and recycles completed wave records.

## Music Track: ScreamTracker 3 Cover of Jarre's *Chronologie* Part 4

`shooter2d.S3M` is the only ScreamTracker 3 module on the disc (whereas the racing game uses the `.cbd2` and MPEG-1 audio paths described in [Audio Architecture](audio.md)). The module header (`SCRM`, 568,176 bytes, 40 order slots, 99 patterns, 16 used channels, initial speed 6 / tempo 125) carries the title:

```text
Jarre:CHRONOLOGIE P4 (kk)
```

Instruments 1–23 store a message across their 12-byte filename fields:
> `I know, it's not nice to clear the samplenames, but in this case it took a lot of knowledge to create this tune. Thats why I wouldn't be happy, if someone copies the pattern and the notes of this music and makes a remix or something else out of it. (Kuki)`

The 28-byte name fields of instruments 1–19 carry the credits:
- `k.koch 6.9.94` and `time: 4:01`
- `composed by jean michel jarre`
- `original published on ~cronologie~ part 4 (dreyfuss records, france)`

Because the `.S3M` module embeds its own PCM instrument samples, `PLAY-S3M.c` synthesizes the tracker channels directly through the Audio Folio DSPP. (The author's 1994 personal contact details in the remaining instrument fields are omitted here.)

## Audio, Folios, and Persistence

The per-image selector array in `selector_names.gen.h` (see [Folio Shims](../recompilation/shims.md)) resolves imports across `kernel`, `filesystem`, `iff`, `gstate`, `graphics`, `font`, and `audio`. Unlike `driving/game` (which statically links `gstate`), `shooter2d` invokes both `gstate` and `font` across folio import vectors.

`score.c::savehighscore` (`0xd08c`) writes four bytes through the shared MicroCard helpers to `/card/shooter2d.hiscore` when the high score is dirty on game over. For savestate continuation details across child executables, see [Savestate Continuation Scope](../recompilation/limitations.md#savestate-m2sv--m2ex-continuation-scope).