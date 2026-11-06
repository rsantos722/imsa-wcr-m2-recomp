# Unfinished, Cut, and Restorable Features

## Summary of Dormant Subsystems

| Feature | Compilation Unit | Entry Point(s) | Status in Prototype Binary | Restoration Flag |
| --- | --- | --- | --- | --- |
| **Aerodynamic Drafting (Slipstream)** | `ai.c` | `HandleDrafting` (`0x6d44`, 472 B) | Uncalled; naive reconnection exposes the state hazards below | `M2_RESTORE_DRAFTING=1` |
| **Post-Race "Best Times" Screen** | `ui.c` | `UI_DrawBestTimes` (`0x379cc`, 1,004 B), `UI_EnterTime` (`0x37928`) | Three-layout summary screen; bypassed in `StateDriver` | `M2_RESTORE_BEST_TIMES=1` |
| **Scrolling Credits Engine** | `credits.c` | `credits_init`, `credits_iterate`, `credits_teardown` | Dormant sprite/text scroller; active menu path plays placeholder `credits.mov` | `M2_RESTORE_CREDITS=1` |
| **Auto-Flip Upside-Down Car** | `Crashing.c` | `flipCarUpright` (`0xe574`, 292 B) | Uncalled; active crash path uses checkpoint relocation | *(Dormant)* |
| **Car Explosions** | `effects.c` | `EF_MakeExplosion` (`0x135e8`, 496 B), `EF_ExplosionPhysics` | `gexplosion` template initialized at boot; never triggered on crash | *(Dormant)* |
| **Animated Crowd Textures** | `game.c` | `initcrowdanimations` (`0x1a660`, 184 B), `outputrawcrowdtex` (`0x1a624`, 60 B) | Dormant; its three Testona `.rawtex` frames are absent from the disc | *(Dormant)* |

## Aerodynamic Drafting / Slipstream (`HandleDrafting`, `0x6d44`)

### How It Works in the Binary

`HandleDrafting` (472 B in `ai.c`) implements a physical aerodynamic slipstream wake behind leading cars:

1. Calls `RankOppPositions` (`0x36f4`) to identify the next car directly ahead of the player on track (`racers[0]+0x5c`).
2. Checks if the leading car is within 2 centerline checkpoints of the player.
3. Transforms the relative offset into the leading car's local coordinate frame and checks if the player lies inside a rectangle behind the leader:
   - Longitudinal distance: `0.0f >= z > -60.0f` feet behind the leader.
   - Lateral distance: `|x| < 6.0f` feet from the leader's centerline.
4. Computes the dimensionless slipstream reduction factor `k`:
   - **Deep pocket (`0 >= z >= -10` ft):** `k = 0.5f` (50% reduction in relative headwind).
   - **Tapered wake (`-10 > z > -60` ft):** Bilinear falloff in both length and width:
     ```c
     float k = 0.01f * (z + 60.0f) * (6.0f - fabsf(x)) / 6.0f;
     ```
5. Adds `k * leader_velocity` onto the player car's ambient wind vector `Vwind` (`0x17c7c4 + 0x48..0x50`). `aerofn` (`0x497f4`) subtracts `Vwind` when computing relative airspeed. For aligned cars at equal velocity, this scales drag and wing downforce by `(1-k)^2`, reaching 25% at `k = 0.5` (see [Simulation Step and Body Step](physics-and-aero.md#simulation-step-and-body-step)).

### State Hazards When Reconnecting the Dormant Routine

Tracing `HandleDrafting` alongside `CarSimFunction` (`0xa8d8`) and `aerofn` (`0x497f4`) reveals two bugs that would occur if `HandleDrafting` were called naively each frame:

1. **Unbounded `Vwind` Accumulation:** `HandleDrafting` *adds* (`fadds`) the wake vector into `Vwind` (`0x17c80c..0x17c814`), and nothing in `mill.c` or `aerofn` zeroes `Vwind` at the start of the frame. Within seconds of drafting a car, `Vwind` would accumulate into an infinite tailwind.
2. **Solo-Session Self-Drafting (`numDrones == 0`):** When racing with 0 AI opponents (`[0x187544] == 0`), `RankOppPositions` leaves the "next car ahead" index (`racers[0]+0x5c`) set to `0` (the player car itself). Because a car is always at `(x = 0, z = 0)` relative to itself, `HandleDrafting` would draft the player off their own car (`k = 0.5f`) permanently.

### Restoration (`M2_RESTORE_DRAFTING=1`)

Setting `M2_RESTORE_DRAFTING=1` hooks the `CarSimFunction` tail (`0xae74`), zeroes the 3-float `Vwind` vector (`0x17c80c..0x17c814`) every tick prior to evaluation, and guards the call to `HandleDrafting` on `numDrones > 0` (`*(uint32_t*)0x187544 > 0`).

## Post-Race "Best Times" Summary Screen (`UI_DrawBestTimes`, `0x379cc`)

### How It Works in the Binary

In the shipped prototype flow, finishing a race plays the Win/Lose/DNF `.mov` in Primary State 4 and jumps directly to High-Score Name Entry (Secondary 32) or the High-Score Table (Secondary 31), never showing the player's actual race time, best lap, or average speed if they did not set a top-5 record.

`UI_DrawBestTimes` (1,004 B in `ui.c`, paired with `UI_EnterTime` at `0x37928`) is a post-race summary screen that renders:

- **Total Race Time:** `racers[0]+0x38` formatted via `GetMinSecHund`.
- **Best Lap Time:** `racers[0]+0x34` formatted via `GetMinSecHund`.
- **Average Race Speed:** Printed as `%6.2f` MPH.
- **Finishing Standing:** Ordinal place (`racers[0]+0x14`) rendered in one of three layouts matching the `StateDriver` outcome code `r25` (`0` = Win, `1` = Lose, `2` = DNF).

### Missing Initialization and Host Restoration (`M2_RESTORE_BEST_TIMES=1`)

`UI_DrawBestTimes` references globals (`finishStill`, `nut`, `fHelv`, `fLcd`) that are left uninitialized by the front-end boot path.

Setting `M2_RESTORE_BEST_TIMES=1` intercepts the post-FMV transition at `0x3136c` (and Secondary States 31/32), populates the backdrop using the current track's High-Score background still (`HSstills + track * 12`), binds the loaded `lcdfont` alongside `driving/data/font/helv42.font` (loaded via `TwoD_LoadFont_memcall` and `MS_MyAlloc` at `0x23a40`), and presents `UI_DrawBestTimes` until the player presses a button to advance to the High-Score table.

Because a dedicated `finishStill` asset is not on the prototype disc, the restoration uses the track's High-Score still and draws the row labels (`RACE TIME`, `BEST LAP`, `AVG MPH`, `PLACE`, `LAPS`) with the menu font. It also re-selects the current destination as the destination-blend source after drawing the background (matching the shipped post-race screen at `0x395b4`) and clears `StateDriver`'s idle counter when arming the screen so attract mode cannot preempt Secondary 31/32 before the first summary frame.

In the dormant DNF layout (`r25 == 2`), the guest code skips the race-time and average-speed rows and formats the lap count twice, drawing an uninitialized stack buffer on the `y = 170` row when the lap count is nonzero. The restoration populates that buffer from `racers[0] + 0x34` (best lap) before the guest tests the count so the `BEST LAP` label displays properly.

## Scrolling Credits Engine (`credits.c`)

- **Implementation:** `credits.c` (`credits_init`, `credits_iterate`, `credits_teardown`) implements an interactive 2D scrolling credits sequence with studio role headers and developer names.
- In `StateDriver`, selecting Credits from the Options menu bypasses `credits.c` and instead invokes `VideoPlayer` on `driving/data/movies/credits.mov`—which on the prototype disc is a byte-for-byte duplicate of the 1-second placeholder `dummy.mov`.
- **Restoration (`M2_RESTORE_CREDITS=1`):** Redirects the Options -> Credits action away from `credits.mov` and drives `credits_init`, `credits_iterate`, and `credits_teardown`.

## Auto-Flip (`flipCarUpright`, `0xe574`) and Car Explosions (`EF_MakeExplosion`, `0x135e8`)

- **Auto-Flip Upside-Down Car (`flipCarUpright`, 292 B in `Crashing.c`):** Inspects the vertical component of the car's orientation matrix; when the car rolls onto its roof, it reconstructs an upright basis matrix in-place and logs `"Flipping car over"`. In the shipped prototype, `HandlePlayerCrashing` (`0xdfa8`) instead performs a full centerline relocation (`relocate_car`) whenever a rollover or severe off-track crash is detected. (A 4-byte `blr` stub `InitRestartPoint` also remains in `Crashing.c`.)
- **Car Explosions (`EF_MakeExplosion`, 496 B, and `EF_ExplosionPhysics` in `effects.c`):** During track initialization, `effects.c` sets up the `gexplosion` 3D sprite particle template alongside tire smoke (`gsmoke`), dust (`gdust`), and wall scrape sparks (`gspark`). While `EF_MakeSpark` (`0x1323c`) and smoke/dust emitters are called every frame, no collision threshold in `Crashing.c` or `collision.c` ever calls `EF_MakeExplosion`. It may have been intended for terminal car damage, though no caller survives in the binary.

## Animated Crowd Textures (`initcrowdanimations`, `0x1a660`)

`game.c` retains a crowd-texture setup routine (`initcrowdanimations` at `0x1a660`) and an export helper (`outputrawcrowdtex` at `0x1a624`), though the three frame files it references are absent from the disc:

- `driving/data/tracks/testona/ra_test_crowd2.rawtex`
- `driving/data/tracks/testona/ra_test_crowd3.rawtex`
- `driving/data/tracks/testona/ra_test_crowd4.rawtex`

When called, `initcrowdanimations` queries `M_FindFirstPodOfType` for a type-8 pod, skips setup if unavailable or if the secret track is active, and registers those paths with `swaptex_addtex`. Neither routine has a caller in the translated binary. (The disc does ship `crowd.imsasound` and `crowd.22m.imsasound`, which are crowd audio loops.)
