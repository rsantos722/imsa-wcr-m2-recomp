#!/usr/bin/env python3
"""Bounded frozen-state sweeps of the original guest tire force calculation.

Requires m2_tire_probe from a game-data build. Only optional PDF plotting needs
matplotlib; the sweep, CSV and JSON report use the Python standard library.
"""
import argparse
import csv
import hashlib
import itertools
import json
import math
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
FIELDS = (
    "id", "car", "corner", "speed_fps", "slip_deg", "long_slip",
    "load_native", "grip_scale", "lateral_force_native", "normal_force_native",
    "longitudinal_force_native", "contact_load_native", "steps", "settled",
    "residual_native", "status",
)
INPUT_FIELDS = FIELDS[:8]
GROUP_FIELDS = ("car", "corner", "speed_fps", "long_slip", "load_native", "grip_scale")
STATUSES = {"ok", "unsettled", "nonfinite", "load_unreachable", "initialization_failed"}


def finite(value, lo, hi, name):
    if not math.isfinite(value) or not lo <= value <= hi:
        raise ValueError(f"{name} must be finite and between {lo} and {hi}")
    return value


def values(text, lo, hi, name, integer=False):
    result = [finite(float(v), lo, hi, name) for v in text.split(",")]
    if not result or len(result) != len(set(result)):
        raise ValueError(f"{name} must be a nonempty list without duplicates")
    if integer:
        if any(v != int(v) for v in result):
            raise ValueError(f"{name} must contain integers")
        return [int(v) for v in result]
    return result


def make_cases(cars, corners, speeds_kmh, loads, slips, grips, max_angle, step):
    finite(max_angle, 0.1, 60, "max-angle")
    finite(step, 0.05, max_angle, "angle-step")
    count = max_angle / step
    if not math.isclose(count, round(count), abs_tol=1.e-7):
        raise ValueError("max-angle must be an integer multiple of angle-step")
    angles = [i * step for i in range(-round(count), round(count) + 1)]
    total = math.prod(map(len, (cars, corners, speeds_kmh, loads, slips, grips, angles)))
    if total > 8192:
        raise ValueError(f"{total} samples exceed the 8192-sample limit; reduce the matrix")
    cases = []
    for car, corner, kmh, load, kappa, grip, angle in itertools.product(
            cars, corners, speeds_kmh, loads, slips, grips, angles):
        cases.append(dict(zip(INPUT_FIELDS, (
            len(cases), car, corner, kmh / 3.6 / 0.3048, angle, kappa, load, grip))))
    return cases


def read_results(path, cases):
    """Validate the protocol and preserve good rows when siblings fail."""
    rows, errors, seen = [], [], set()
    if not path.is_file():
        return rows, ["probe did not create samples.csv"]
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if tuple(reader.fieldnames or ()) != FIELDS:
            return rows, ["unexpected probe CSV schema"]
        for raw in reader:
            try:
                row = {k: float(raw[k]) for k in FIELDS[:-1]}
                row["status"] = raw["status"]
                ident = row["id"]
                if not math.isfinite(ident) or ident != int(ident) or not 0 <= ident < len(cases):
                    raise ValueError("invalid sample id")
                ident = int(ident)
                if ident in seen:
                    raise ValueError(f"duplicate sample id {ident}")
                seen.add(ident)
                for key in INPUT_FIELDS:
                    if not math.isclose(row[key], cases[ident][key], rel_tol=2.e-6, abs_tol=1.e-6):
                        raise ValueError(f"sample {ident}: input mismatch in {key}")
                if row["status"] not in STATUSES:
                    raise ValueError(f"sample {ident}: unknown status")
                if row["status"] == "ok":
                    if not all(math.isfinite(row[k]) for k in FIELDS[:-1]):
                        raise ValueError(f"sample {ident}: nonfinite successful result")
                    if row["settled"] != 1 or row["steps"] < 16 or row["steps"] != int(row["steps"]):
                        raise ValueError(f"sample {ident}: invalid settling receipt")
                    if abs(row["contact_load_native"] - row["load_native"]) > max(0.002, row["load_native"] * 0.0011):
                        raise ValueError(f"sample {ident}: load calibration mismatch")
                    if row["residual_native"] < 0:
                        raise ValueError(f"sample {ident}: negative residual")
                else:
                    errors.append(f"sample {ident}: {row['status']}")
                # JSON uses null for undefined results, never NaN/Infinity.
                rows.append({k: (None if isinstance(v, float) and not math.isfinite(v) else v)
                             for k, v in row.items()})
            except (ValueError, TypeError, KeyError) as exc:
                errors.append(str(exc))
    missing = len(cases) - len(seen)
    if missing:
        errors.append(f"{missing} samples missing")
    return rows, errors


def summarize(rows, cases, fraction=0.95):
    finite(fraction, 0.5, 1, "force-fraction")
    grouped, expected = {}, {}
    for case in cases:
        key = tuple(case[k] for k in GROUP_FIELDS)
        expected[key] = expected.get(key, 0) + 1
    for row in rows:
        # Match by id so fp32 formatting cannot split one experimental group.
        key = tuple(cases[int(row["id"])][k] for k in GROUP_FIELDS)
        grouped.setdefault(key, []).append(row)
    report = []
    for key, count in expected.items():
        samples = grouped.get(key, [])
        good = sorted((r for r in samples if r["status"] == "ok"), key=lambda r: r["slip_deg"])
        complete = len(good) == count
        positive = [r for r in good if r["slip_deg"] >= 0]
        peak = max((r["lateral_force_native"] for r in positive), default=0)
        knee = None
        # A sampled near-peak crossing, not a certified global optimum.
        if complete and peak > 0 and key[2] > 0:
            target = peak * fraction
            for lower, upper in zip(positive, positive[1:]):
                lo, hi = lower["lateral_force_native"], upper["lateral_force_native"]
                if lo <= target <= hi and hi > lo:
                    knee = lower["slip_deg"] + (upper["slip_deg"] - lower["slip_deg"]) * (target - lo) / (hi - lo)
                    break
        at_edge = bool(positive and peak > 0 and positive[-1]["lateral_force_native"] >= peak * (1 - 1.e-6))
        report.append({
            **dict(zip(GROUP_FIELDS, key)), "expected_samples": count,
            "valid_samples": len(good), "complete": complete,
            "sampled_peak_lateral_force_native": peak if positive else None,
            "peak_at_sweep_limit": at_edge,
            "force_fraction": fraction,
            "sampled_near_peak_slip_deg": knee,
        })
    return report


def plot_results(rows, cases, destination):
    import matplotlib
    matplotlib.use("Agg")
    from matplotlib import pyplot as plt
    from matplotlib.backends.backend_pdf import PdfPages

    grouped = {}
    for row in rows:
        if row["status"] == "ok":
            key = tuple(cases[int(row["id"])][k] for k in GROUP_FIELDS)
            grouped.setdefault(key, []).append(row)
    groups = list(grouped.items())
    with PdfPages(destination) as pdf:
        for start in range(0, len(groups), 6):
            fig, axes = plt.subplots(3, 2, figsize=(10, 11), squeeze=False)
            fig.suptitle("Original patchforce: frozen tire state\nNative forces; sampled range does not establish a global optimum")
            for ax, (key, samples) in zip(axes.flat, groups[start:start + 6]):
                samples.sort(key=lambda r: r["slip_deg"])
                ax.plot([r["slip_deg"] for r in samples], [r["lateral_force_native"] for r in samples], label="Lateral")
                ax.plot([r["slip_deg"] for r in samples], [r["longitudinal_force_native"] for r in samples], label="Longitudinal", linestyle="--")
                car, corner, speed, slip, load, grip = key
                ax.set_title(f"Car {car}, front {corner}; {speed * 0.3048 * 3.6:.0f} km/h\nLoad {load:g}, κ {slip:g}, grip scale {grip:g}", fontsize=9)
                ax.set_xlabel("Tire slip angle (degrees)")
                ax.set_ylabel("Force (native)")
                ax.grid(alpha=0.25)
                ax.legend(fontsize=8)
            for ax in list(axes.flat)[len(groups[start:start + 6]):]:
                ax.set_visible(False)
            fig.tight_layout(rect=(0, 0, 1, 0.94))
            pdf.savefig(fig)
            plt.close(fig)


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--probe", type=Path, required=True)
    p.add_argument("--game-data", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True, help="new directory outside the repository")
    p.add_argument("--cars", default="0")
    p.add_argument("--corners", default="0,1", help="front corners 0 and 1")
    p.add_argument("--speeds-kmh", default="30,90,180")
    p.add_argument("--loads-native", default="300,600,1200")
    p.add_argument("--long-slips", default="0,-0.15", help="κ=(rolling-forward)/forward; use --long-slips=-1,0 for lockup")
    p.add_argument("--grip-scales", default="1", help="controlled global coefficient scale, not a named road surface")
    p.add_argument("--max-angle", type=float, default=20)
    p.add_argument("--angle-step", type=float, default=0.5)
    p.add_argument("--max-steps", type=int, default=512)
    p.add_argument("--dt", type=float, default=1 / 60)
    p.add_argument("--temperature", type=float, default=180, help="frozen native temperature")
    p.add_argument("--wear", type=float, default=1, help="frozen remaining-tire fraction")
    p.add_argument("--force-fraction", type=float, default=0.95)
    p.add_argument("--timeout", type=float, default=90)
    p.add_argument("--plot", action="store_true", help="also write force-curves.pdf; requires matplotlib")
    return p


def main(argv=None):
    p = parser()
    args = p.parse_args(argv)
    try:
        cases = make_cases(
            values(args.cars, 0, 3, "cars", True), values(args.corners, 0, 1, "corners", True),
            values(args.speeds_kmh, 0, 1097.28, "speeds"), values(args.loads_native, 0.001, 100000, "loads"),
            values(args.long_slips, -1, 1, "long-slips"), values(args.grip_scales, 0.001, 10, "grip-scales"),
            args.max_angle, args.angle_step)
        finite(args.dt, 0.0001, 0.1, "dt")
        finite(args.temperature, 0, 1000, "temperature")
        finite(args.wear, 0, 1, "wear")
        finite(args.force_fraction, 0.5, 1, "force-fraction")
        finite(args.timeout, 1, 300, "timeout")
        if not 16 <= args.max_steps <= 4096 or len(cases) * args.max_steps > 4_000_000:
            raise ValueError("max-steps must be 16..4096 and the matrix must fit 4,000,000 settling steps")
        output = args.output.resolve()
        if output == REPO or REPO in output.parents:
            raise ValueError("output must be outside the repository (game-derived measurements)")
        if output.exists():
            raise ValueError("output directory must not already exist")
        probe = args.probe.resolve()
        root = args.game_data.resolve()
        image = root / "driving" / "game"
        if not probe.is_file() or not image.is_file():
            raise ValueError("probe and game-data/driving/game must exist")
        if args.plot:
            import matplotlib  # noqa: F401
    except (ValueError, ImportError) as exc:
        p.error(str(exc))

    output.mkdir(parents=True, exist_ok=False)
    request = "".join(" ".join(str(case[k]) for k in INPUT_FIELDS) + "\n" for case in cases)
    (output / "requests.txt").write_text(request, encoding="utf-8")
    command = [str(probe), "--game-data", str(root), "--output", str(output / "samples.csv"),
               "--max-steps", str(args.max_steps), "--dt", str(args.dt),
               "--temperature", str(args.temperature), "--wear", str(args.wear)]
    errors = []
    try:
        result = subprocess.run(command, input=request, text=True, capture_output=True, timeout=args.timeout)
        (output / "probe.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode:
            errors.append(f"probe exited with status {result.returncode}")
    except subprocess.TimeoutExpired as exc:
        errors.append("probe timed out")
        log = (exc.stdout or b"") + (exc.stderr or b"")
        (output / "probe.log").write_bytes(log if isinstance(log, bytes) else log.encode())
    except OSError as exc:
        errors.append(f"probe could not start: {exc}")
    rows, protocol_errors = read_results(output / "samples.csv", cases)
    errors.extend(protocol_errors)
    report = {
        "schema": 1, "experiment": "frozen_original_tire_force",
        "game_sha256": digest(image), "probe_sha256": digest(probe), "command": command,
        "dt": args.dt, "temperature_native": args.temperature, "wear": args.wear,
        "requested_samples": len(cases), "returned_samples": len(rows),
        "valid": not errors, "errors": errors,
        "limits": ["No full-car dynamics, road lookup, or Arcade/Simulation preset initialization.",
                   "Normal load is calibrated using original patchforce; temperature/wear are frozen.",
                   "Exact zero sliding velocity returns a zero force vector in original patchforce.",
                   "Near-peak slip is relative to the sampled range, not a proven optimal steering angle."],
        "curves": summarize(rows, cases, args.force_fraction),
    }
    (output / "summary.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    if args.plot and any(r["status"] == "ok" for r in rows):
        plot_results(rows, cases, output / "force-curves.pdf")
    print(f"{len(rows)}/{len(cases)} samples; {'PASS' if not errors else 'INCOMPLETE'}: {output}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
