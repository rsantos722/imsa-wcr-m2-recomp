"""Tire sweep protocol, incomplete experiments and sampled-range summaries."""
import csv
import importlib.util
import json
from pathlib import Path
import subprocess

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "tire_sweep.py"
SPEC = importlib.util.spec_from_file_location("tire_sweep", SCRIPT)
sweep = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sweep)


def cases():
    return sweep.make_cases([0], [0], [36], [600], [0], [1], 2, 1)


def result(case, status="ok"):
    return {
        **case, "lateral_force_native": case["slip_deg"] * 100,
        "normal_force_native": case["load_native"], "longitudinal_force_native": 0,
        "contact_load_native": case["load_native"], "steps": 24, "settled": int(status == "ok"),
        "residual_native": 0, "status": status,
    }


def write_results(path, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=sweep.FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def test_case_units_and_bounded_matrix():
    sample = cases()
    assert sample[0]["speed_fps"] == pytest.approx(10 / 0.3048)
    assert [r["slip_deg"] for r in sample] == [-2, -1, 0, 1, 2]
    with pytest.raises(ValueError, match="multiple"):
        sweep.make_cases([0], [0], [30], [600], [0], [1], 2, 0.7)
    with pytest.raises(ValueError, match="8192"):
        sweep.make_cases([0, 1, 2, 3], [0, 1], [30, 60, 120], [300, 600, 1200], [0, -.1], [1], 20, 0.05)
    for invalid in ("nan", "inf", "1,1", ""):
        with pytest.raises(ValueError):
            sweep.values(invalid, 0, 3, "test")


def test_valid_curve_is_explicitly_sampled_not_global_optimum(tmp_path):
    q = cases()
    path = tmp_path / "samples.csv"
    write_results(path, [result(c) for c in q])
    rows, errors = sweep.read_results(path, q)
    assert not errors
    curve, = sweep.summarize(rows, q)
    assert curve["complete"]
    assert curve["sampled_near_peak_slip_deg"] == pytest.approx(1.9)
    assert curve["peak_at_sweep_limit"]


def test_failed_sibling_preserves_good_rows_but_disqualifies_curve(tmp_path):
    q = cases()
    path = tmp_path / "samples.csv"
    write_results(path, [result(c, "unsettled" if c["slip_deg"] == 1 else "ok") for c in q])
    rows, errors = sweep.read_results(path, q)
    assert errors and len(rows) == len(q)
    curve, = sweep.summarize(rows, q)
    assert curve["valid_samples"] == 4
    assert not curve["complete"]
    assert curve["sampled_near_peak_slip_deg"] is None


@pytest.mark.parametrize("change", [
    {"lateral_force_native": "nan"}, {"speed_fps": 2}, {"settled": 0},
    {"steps": 1}, {"contact_load_native": 2}, {"status": "invented"},
])
def test_success_receipt_must_be_finite_settled_and_match_request(tmp_path, change):
    q = cases()
    rows = [result(c) for c in q]
    rows[0].update(change)
    path = tmp_path / "samples.csv"
    write_results(path, rows)
    parsed, errors = sweep.read_results(path, q)
    assert errors
    assert not sweep.summarize(parsed, q)[0]["complete"]


def test_missing_and_duplicate_rows_fail(tmp_path):
    q = cases()
    path = tmp_path / "samples.csv"
    write_results(path, [result(q[0]), result(q[0])])
    _, errors = sweep.read_results(path, q)
    assert any("duplicate" in error for error in errors)
    assert any("missing" in error for error in errors)


def test_zero_speed_has_no_inferred_useful_angle():
    q = sweep.make_cases([0], [0], [0], [600], [0], [1], 2, 1)
    rows = [{**result(c), "lateral_force_native": 0} for c in q]
    curve, = sweep.summarize(rows, q)
    assert curve["sampled_near_peak_slip_deg"] is None


def test_integration_keeps_partial_csv_and_returns_failure(tmp_path, monkeypatch):
    root = tmp_path / "game"
    (root / "driving").mkdir(parents=True)
    (root / "driving" / "game").write_bytes(b"test image placeholder")
    probe = tmp_path / "probe"
    probe.write_text("test probe placeholder")
    output = tmp_path / "sweep"

    def fake_probe(command, *, input, **kwargs):
        q = []
        for line in input.splitlines():
            q.append(dict(zip(sweep.INPUT_FIELDS, map(float, line.split()))))
        rows = [result(c, "unsettled" if i == 0 else "ok") for i, c in enumerate(q)]
        write_results(Path(command[command.index("--output") + 1]), rows)
        return subprocess.CompletedProcess(command, 1, "", "bounded nonconvergence\n")

    monkeypatch.setattr(sweep.subprocess, "run", fake_probe)
    code = sweep.main(["--probe", str(probe), "--game-data", str(root), "--output", str(output),
                       "--corners", "0", "--speeds-kmh", "36", "--loads-native", "600",
                       "--long-slips", "0", "--max-angle", "2", "--angle-step", "1"])
    assert code == 1
    report = json.loads((output / "summary.json").read_text())
    assert not report["valid"]
    assert report["returned_samples"] == 5
    assert report["curves"][0]["valid_samples"] == 4
    assert (output / "samples.csv").is_file()


def test_output_inside_repository_is_rejected_before_probe(monkeypatch):
    def should_not_run(*args, **kwargs):
        pytest.fail("probe started before output validation")
    monkeypatch.setattr(sweep.subprocess, "run", should_not_run)
    with pytest.raises(SystemExit) as exc:
        sweep.main(["--probe", "missing", "--game-data", "missing", "--output", str(sweep.REPO / "generated-measurements")])
    assert exc.value.code == 2
