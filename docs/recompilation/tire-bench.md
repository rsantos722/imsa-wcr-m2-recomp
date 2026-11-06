# Frozen-State Tire Bench

`m2_tire_probe` is an isolated physics test bench built alongside [Improved Analog Steering](analog-steering.md). It calls the original guest tire routines—`setup_tire` (`.text+0x40414`) and `patchforce` (`.text+0x40aa8`)—through the recompiler dispatch table to measure tire forces under controlled conditions without starting graphics, audio, or the main game loop.

## Running

Build the `m2_tire_probe` target, then use [`tools/tire_sweep.py`](../../tools/tire_sweep.py) or invoke `m2_tire_probe` directly:

```sh
# Sweep across slip angles, vertical loads, and speeds (--plot writes force-curves.pdf)
python3 tools/tire_sweep.py --probe build-game/tests/m2_tire_probe \
    --game-data /path/to/game-data --output /tmp/tire-sweep --plot

# Evaluate tire capacity across all four cars at a given temperature and wear level
build-game/tests/m2_tire_probe --game-data /path/to/game-data \
    --analyze-slip /tmp/slip-analysis.csv \
    --temperature 180 --wear 1.0 --reference-fraction 0.99

# Run the built-in self-test suite
build-game/tests/m2_tire_probe --game-data /path/to/game-data --self-test
```

## Experimental Setup

- **Tire Templates**: Copies the authored tire template from `.data+0x5d78` and the selected corner pointer (`cfg+0x58+4*corner`) into scratch guest memory before calling `setup_tire`.
- **Operating Conditions**: Holds tire temperature (default `180`), wear fraction (default `1.0`), and surface grip multiplier (default `1.0`) constant across the sweep.
- **Normal Load Calibration**: Solves the native vertical spring equation for the contact height that yields the target normal load ($F_z$) within 0.1%.
- **Kinematics**: Holds forward velocity ($v_x$) constant, sets lateral velocity from slip angle $\alpha$ ($v_y = -v_x \tan(\alpha)$), and applies longitudinal slip ratio $k = (v_{\text{rolling}} - v_x) / v_x$ ($0$ = free rolling, $-1$ = locked wheel, $>0$ = wheelspin).
- **Relaxation Settling**: Because `patchforce` relaxes tire forces over time, each sample starts from a clean state and steps at $\Delta t = 1/60\text{ s}$ for 16 to 512 iterations until eight consecutive residuals fall below $\max(0.0001, 0.00001 \times \text{scale})$.

## Output Metrics

The sweep writes per-point measurements to `samples.csv` and summary statistics to `summary.json`:

| Column | Description |
| :--- | :--- |
| `contact_load_native` | Target contact load set by the height solver. |
| `normal_force_native` | Vertical force reported by `patchforce`. |
| `max_force_native` | Peak or asymptotic lateral force capacity of the tire model. |
| `cornering_peak_deg` | Slip angle achieving maximum force perpendicular to the travel direction. |
| `cornering_peak_force` | Maximum perpendicular cornering force ($F_{\text{turn}}$). |
| `cornering_peak_at_bound` | Nonzero when the peak was clamped at the $60^\circ$ search boundary. |
| `kind` | Curve classification: `peak` (finite maximum), `plateau` (asymptotic), or `locked`. |

The `cornering_peak_*` columns use turning force rather than wheel-local $F_y$ (see [Slip-Angle Maxima and Force Direction](../game/tires-and-suspension.md#slip-angle-maxima-and-force-direction)). Braking slip ($k < 0$) shifts the peak inward.

## Scope and Caveats

`m2_tire_probe` evaluates a single isolated tire on a flat plane with fixed grip multipliers. Full-vehicle effects—including suspension geometry, roll and pitch load transfer, aerodynamic downforce, and track surface transitions—are tested separately in the full-car harness ([`tests/guest/test_steering_vehicle.c`](../../tests/guest/test_steering_vehicle.c)).
