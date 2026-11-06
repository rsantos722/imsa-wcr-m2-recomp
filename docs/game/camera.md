# Camera

## Camera Unit (`camera.c`) and Per-Frame Flow

The `camera.c` compilation unit (`0x798c..0x956c`) contains 22 functions that manage in-race view modes, follow-camera spring/damping kinematics, camera-to-camera interpolation tweens, track-surface height clamping, and replay camera switching.

### Key Globals and State Pointers

| Symbol / Address | Section / Offset | Description |
|---|---|---|
| `gdp` (`[0x184cf4]`) | `.data` init `0x182b48` | Master game data pointer (`GDP`), holding camera mode flags and race state |
| `[0x1875c8]` | `.bss+0x6328` | Active camera pointer passed to `M_SetCamera` and scene-graph culling |
| `CamControl` | `.bss` | Primary camera control block (`r31` in `Cam_Update` / `Cam_TweenTo`) |
| `replay_cams` | `.data` / `.bss` | Authored trackside replay camera regions and look-at configurations |
| `[0x184cf8]` | `.bss` | Active world root pointer (`world`), used for track centerline & surface queries |
| `0x181E10..0x181E18` | `.bss` | Published player render position `(X, Y, Z)` written once per frame by `GetCarPosition` |
| `0x181E20..0x181E28` | `.bss` | Per-iteration opponent scratch position `(X, Y, Z)` written by `GetDronePosition` |

### Frame Update Sequence

1. **Initialization (`Cam_InitModule`):** Called from `trackInit_2` (`0x1726c`) during track load. Initializes named camera mount points (cockpit/seat, hood, chase/car, side views), follow-camera state records, and tween blocks.
2. **Pose Publication (`GetCarPosition` at `0x13f68`, `GetDronePosition` at `0x13d34`):**
   - `GetCarPosition` (1,700 B, 119 FP ops in `game.c`) reads the player car's simulation pose (`+0x24`, `+0x28`, `+0x2c`) and writes the player's render-space position to `.bss` `0x181E10` (X), `0x181E14` (Y), and `0x181E18` (Z) at stores `0x13fec`, `0x13ff8`, and `0x14004` once per frame. Over active race windows, the published render triple equals `(sim[+0x08], sim[+0x0c], -sim[+0x10])` from the previous application frame. It also offsets the 4 converted wheel positions by the per-car graphics offset table `wheelpos` at `.data+0x5d98` (4 cars × 4 XYZ triples, `0x1402c..0x140a4`).
   - `GetDronePosition` (564 B, 52 FP ops) publishes each AI opponent's world position and orientation into the per-iteration scratch globals `0x181E20..0x181E28` (`0x13dc0..0x13dd0`, final loop stores `0x13f0c..0x13f28`).
3. **Camera Update (`Cam_Update`):** Called once per frame from `trackIterate` (`0x190c4`) alongside `Cam_FlyUpdate` and `Cam_TweenTo`. Updates the active camera mount, runs `Cam_Follow` or `Cam_new_replay`, advances active tweens via `Cam_TweenDo`, and builds the view matrix via `Matrix_LookAt` (`0x603fc`).

## Ground Clamping and Ray-Segment Intersection Chain

To prevent the chase and replay cameras from clipping through elevated track sections or embankments, `camera.c` implements a 2D/3D track-mesh height and boundary solver.

| Function | File Offset (Guest VA) | Size | Role |
|---|---|---|---|
| `tri_height` | `0x798c` (`0x10798c`) | 100 B (25 insns) | Barycentric plane height interpolation over a triangle |
| `get_height` | `0x79f0` (`0x1079f0`) | 392 B (98 insns) | Track quad split & height evaluation into `out->y` |
| `Cam_Find_track_point` | `0x7b78` (`0x107b78`) | 216 B (54 insns) | Nearest-point search over 32-byte stride track nodes |
| `intersection` | `0x7c50` (`0x107c50`) | 768 B (192 insns) | Circle-segment / ray-segment quadratic intersection solver |
| `pt_inside` | `0x7f50` (`0x107f50`) | 468 B (117 insns) | Point-in-corridor test; sole caller of `get_height` and `intersection` |

### `tri_height` (`0x798c`)
Signature: `f32 tri_height(Vec* p, Vec* w, Vec* q)`
Computes the vertical offset at planar offset `w = (w.x, w.z)` spanned by triangle edge vectors `p` and `q`:
```c
float den = p->z * q->x - p->x * q->z;
float f1  = p->y * q->z - p->z * q->y;
float f6  = p->x * q->y - p->y * q->x;
if (den != 0.0f) {
    return (-(f1 * w->x) - (f6 * w->z)) / den;
}
return 0.0f; // literal 0.0f at .text pool 0x109540
```

### `get_height` (`0x79f0`)
Signature: `void get_height(Vec* out, Vec* org, Vec* node)`
Writes only `out->y` (`4(r29)`) and returns `void`:
1. Calls `UI_GetTrack` (`0x31718`). When on track `0` (300 World Speedway), snaps `node` onto the 32-byte aligned world table via `tab = *(0x184cf8)`:
   `node = tab[0] + (((node - tab[0x0c]) >> 5) << 5)`.
2. Forms edge `p = *(node + 0x20) - *org` and planar query offset `w = (out->x - org->x, out->z - org->z)`.
3. Tests the 2D cross product `crs = p.x * w.z - p.z * w.x` to select the upper or lower triangle of the quad (`q = (crs >= 0.0f) ? (org2 - org) : (node - org)`).
4. Evaluates `out->y = tri_height(&p, &w, &q) + org->y`.
5. If `UI_GetTrack() == 0` and `out->y < 0.0f`, clamps `out->y = 0.0f`.

### `Cam_Find_track_point` (`0x7b78`)
Searches the 32-byte stride track point array (`r5 + idx * 32`, total count from `[0x184cf8]`) for the point closest to a query position:
- **Fast local window:** Biases the hint index by `-4` and scans a 7-point window (`r3 >= 0`) against an acceptance squared-distance threshold of `22500.0f` (`150.0` units, pool `0x109548`).
- **Full-array fallback:** If the biased start index is negative or no point in the 7-point window lies within `22500.0f`, branches back to `0x7b98` and scans all `count` points from index `0`.

### `intersection` (`0x7c50`) and `pt_inside` (`0x7f50`)
- `pt_inside` is the sole caller of both `get_height` (at `0x8054`, `0x80a8`, `0x80fc`) and `intersection` (at `0x8084` and `0x80d8`, sitting on opposite arms of `ble` at `0x806c`).
- `intersection` (`f32 intersection(Vec* out, Vec* r4, Vec* r5, Vec* r6, Vec* r7, f32 t)`) solves the quadratic intersection between segment `u = r7 - r6` and radius `t` around `r5` (`v = r5 - r6`, `L2 = |u|^2`, `n2 = |v|^2`, `dot = u · v`), evaluating discriminant `F = 4 * (dot^2 - L2 * (n2 - t^2))` at `0x7cec`:
  - **Negative discriminant (`F < 0`):** Projects onto the closest point `mu = dot / L2`, `P = r6 + mu * u`, and calls `sqrtf` (`0x4aca0`) on `|r5 - P|^2`.
  - **Non-negative discriminant (`F >= 0`):** Calls `sqrtf(F)` at `0x7dc4`, computes roots `s1 = (sqrt(F) + 2*dot) / (2*L2)` and `s2 = (2*dot - sqrt(F)) / (2*L2)`, writes the clipped XZ point to `out+0` and `out+8`, and returns `{0.0f, t, sqrt(d^2)}` across 8 return paths.

## Follow, Tween, and Replay Camera Modes

### `Cam_Follow` (`0x8840`, 800 B, 200 insns)
Called from two sites in `Cam_Update` to update chase/follow camera positions:
- **Record addressing:** Takes control block `r3`, output slot `A` (`out = r3 + A*16 + 0x144`), reference slot `B` (`recB = r3 + B*16 + 0x144`), and spring parameter slot `C` (`recC = r3 + C*28 + 0x90`).
- **Planar tether and ceiling clamp:** Computes the 2D delta `(dx, dy)` between `recC` and `recB`, normalizes using `sqrtf` with epsilon `1.0e-6f` (`0x109560`), and clamps length against both the per-record limit `[recC+0x0c]` and the `.data` ceiling table at `0x1777D0` (`.data+0x1760`: `{10.0f, 30.0f, 30.0f, 0.0f, 1.0f, 2.0f, 0.0f, 1.5f}`).
- **Track-specific surface clamping:**
  - On Skid Pad (`UI_GetTrack() == 4`), takes a straight-line publish path (`0x8b0c..0x8b50`) writing `recC` and `out+0..+0x0c` directly (`out+0x0c = 0.0f`).
  - On all other tracks, runs two 4-iteration neighborhood scans (`0x896c` reflection `subf` and `0x8a14` wrap `add`) calling `Cam_Find_track_point` and `pt_inside` against running minimum `100000.0f` (`0x109568`) and adding a `+1.0f` vertical clearance offset (`0x109558`).

### Camera Tweening (`Cam_TweenInit`, `Cam_TweenDo`, `Cam_TweenTo`)
Interpolates both camera position and look-at target when switching views:
- **`Cam_TweenInit` (`0x8b60`, 296 B):** Initializes a `0x48`-byte tween state block `r3` from source pointer `r4` (`[r3+0x30]`), target pointer `r5` (`[r3+0x34]`), and integer frame duration `r6` (`[r3+0x38]` and `[r3+0x3c]`). If `r6 == 0` (immediate snap), copies 4 floats from `TO` to `FROM`. Otherwise, copies `TO` into `phase` (`r3+0x10..0x1c`), converts `d = (float)r6`, sets `[r3+0x44] = 1.0f / d`, clears accumulators (`+0x20..0x2c`, `+0x40`), and stores per-component acceleration `([TO+i*4] - [FROM+i*4]) / (d * d)` at `r3+0x00..0x0c`.
- **`Cam_TweenDo` (`0x8c88`, 452 B):** Advances `current[i] += target[i] - phase[i]` and updates `phase[i] = target[i]` so the tween tracks a moving target. While `[r3+0x3c] != 0` (and gated by `.data+0x1838` = `0x1778a8`), applies triangular velocity integration (accelerating during the first phase by decrementing `+0x38`, then decelerating by decrementing `+0x3c`) before snapping onto `target` when `+0x3c` reaches `0`.
- **`Cam_TweenTo` (`0x8e4c`, 396 B):** Copies two 8-word index permutation tables from `.data+0x15d8` (`0x177648`: `{0, 1, 4, 6, 5, 7, 8, 9}`) and `.data+0x15f8` (`0x177668`: `{10, 0, 0, 2, 3, 1, 1, 1}`) onto the stack. Passing slot `r4 == -1` cycles backward (`[r31+0x1f4] - 1`, wrapping `0 -> 3`). If duration argument `r5 < 0`, derives frame count from Euclidean distance: `duration = min(60, (int)(distance / (float)(-r5)))`. Initializes two parallel tweens (`r31` for eye position and `r31+0x48` for look-at target).

### Replay Camera Switching (`Cam_new_replay`, `0x844c`)
`Cam_new_replay` (952 B, 238 insns) implements an 8-way `bctr` switch (`0x85c0..0x85dc`, indexed by `(uint32_t)(int32_t)r31[0]`) over authored replay camera behaviors. It queries `Cam_find_camera_region` (`0x8368`) and `camera_intersect` (`0x8168`), computes look-at matrices via `Matrix_LookAt` (`0x603fc`), and normalizes sightlines with `sqrtf` (`0x4ac64`) and `rsqrtf` (`0x4abe8`).

## Transform and Display Math (`Matrix_*.c`, `DISPMATH.c`)

Two math libraries coexist in the binary:
1. **Mercury Matrix Helpers (`0x603fc..0x60800`):**
   - `Matrix_LookAt` (`0x603fc`, 272 B): Builds a 4×3 view matrix from eye, target, and up vectors.
   - `Matrix_Mult` (`0x60510`, 508 B): Concatenates 4×3 / 4×4 affine transforms.
   - `Matrix_Perspective` (`0x6070c`, 256 B) and `Matrix_Invert`: Projection and inverse transform helpers.
2. **Display and Simulation Vector/Matrix Library (`DISPMATH.c`, `0x49aa4..0x4aa2c`, 36 functions, 4,036 B):**
   - **3×1 Vector Ops:** `fnadd3x1`, `fnsub3x1`, `fncop3x1`, `fnscprod3x1`, `fndotp3x1`, `fncrossp3x1`.
   - **3×3 Rotation and Matrix Ops:** `transform`, `xyzrot`, `mulmat`, `transpmulmat`, `transpmat`, `transpmatip`, `subtract_rotate`, `subtract_rotate_x/_y/_z`, `wxrot`, `xrot`, `wyrot`, `yrot`, `wzrot`, `wxyzrot`, `zrot`, `fncopymat`, `matsprint`.
   - **Angles, Projection, and Normalization:** `getxangxz`, `getzangxz`, `getzangyz`, `sincos`, `project` (`0x4a53c`), `unproject` (`0x4a620`), `trans_project` (`0x4a6c0`), `quick_normalize` (`0x4a78c`), `normalize` (`0x4a81c`), `makeperp` (`0x4a8d8`), `safesqrt` (`0x4aa2c`). A physics-specific counterpart `8DISPMATH.c` provides double-precision equivalents for vehicle simulation (see [Simulation Step and Body Step](physics-and-aero.md#simulation-step-and-body-step)).

For how the camera's view reaches the renderer, see [Mercury Rendering Pipeline](mercury.md).
