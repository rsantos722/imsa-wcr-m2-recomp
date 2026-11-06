"""Behaviour tests for the parity harness's stable comparison boundary.

These cover the schedule-sensitive projections in tests/guest/parity.py without
needing a host binary or game data.
"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "guest"))
import parity  # noqa: E402


def write_frames(path, contents):
    os.makedirs(path)
    for index, content in enumerate(contents):
        with open(os.path.join(path, "frame_%04d.ppm" % index), "wb") as f:
            f.write(b"P6\n1 1\n255\n" + content)


def write_trace(path, records):
    with open(path, "w", encoding="utf-8") as f:
        for record in records:
            f.write(json.dumps(record) + "\n")


def record(kind, guest_frame, payload=None):
    return {
        "schema_version": 1,
        "build_id": "build",
        "run_id": "run",
        "sequence": 1,
        "host_time": 1,
        "guest_frame": guest_frame,
        "task_id": 0,
        "guest_pc": 1,
        "event_kind": kind,
        "payload": payload or {},
    }


def frame_complete(sequence):
    return record("frame_complete", sequence,
                  {"seq": sequence, "pc": 1, "state": 0})


def test_frame_hashes_ignore_capture_order_and_repeats(tmp_path):
    first = tmp_path / "first"
    second = tmp_path / "second"
    write_frames(first, [b"AA", b"BB", b"BB"])
    write_frames(second, [b"BB", b"AA", b"AA", b"AA"])

    assert parity.frame_hashes(first) == parity.frame_hashes(second)


def test_trace_ignores_guest_frame_and_input_poll_count(tmp_path):
    first = tmp_path / "first.ndjson"
    second = tmp_path / "second.ndjson"
    write_trace(first, [
        record("input", 0, {"buttons": 0, "pressed": 0}),
        frame_complete(0),
        record("selector_call", 1, {"folio": 1, "ordinal": 2}),
        frame_complete(1),
        frame_complete(2),
    ])
    write_trace(second, [
        record("input", 0, {"buttons": 0, "pressed": 0}),
        record("input", 0, {"buttons": 0, "pressed": 0}),
        record("input", 1, {"buttons": 0, "pressed": 0}),
        frame_complete(0),
        record("selector_call", 2, {"folio": 1, "ordinal": 2}),
        frame_complete(1),
        frame_complete(2),
    ])

    first_records, first_completed = parity.trace_records(first)
    second_records, second_completed = parity.trace_records(second)
    boundary = min(first_completed, second_completed)

    assert first_completed == second_completed == 3
    assert boundary == 3
    assert parity.trace_union([first_records], boundary) == \
        parity.trace_union([second_records], boundary)


def test_trace_boundary_excludes_incomplete_tail(tmp_path):
    first = tmp_path / "first.ndjson"
    second = tmp_path / "second.ndjson"
    shared = [
        frame_complete(0),
        frame_complete(1),
        record("asset_load", 1, {"path": "track.dat"}),
    ]
    write_trace(first, shared + [frame_complete(2)])
    write_trace(second, shared)

    first_records, first_completed = parity.trace_records(first)
    second_records, second_completed = parity.trace_records(second)
    boundary = min(first_completed, second_completed)

    assert (first_completed, second_completed) == (3, 2)
    assert boundary == 2
    assert parity.trace_union([first_records], boundary) == \
        parity.trace_union([second_records], boundary)


def test_frames_ignore_unstable_candidate_variant():
    reference = [{"a", "b"}, {"a", "b"}]
    candidate = [{"a", "b", "x"}, {"a", "b"}]

    assert parity.report_frames(reference, candidate) is True


def test_frames_accept_reference_image_seen_once_in_candidate():
    reference = [{"a", "b"}, {"a", "b"}]
    candidate = [{"a"}, {"a", "b"}]

    assert parity.report_frames(reference, candidate) is True


def test_frames_fail_on_missing_stable_reference_image():
    reference = [{"a", "b"}, {"a", "b"}]
    candidate = [{"a"}, {"a"}]

    assert parity.report_frames(reference, candidate) is False


def test_frames_fail_on_stable_unknown_candidate_image():
    reference = [{"a", "b"}, {"a", "b"}]
    candidate = [{"a", "b", "x"}, {"a", "b", "x"}]

    assert parity.report_frames(reference, candidate) is False
