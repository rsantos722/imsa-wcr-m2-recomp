#!/usr/bin/env python3
"""Headless parity run: compare two host builds frame by frame.

Runs a reference and a candidate host binary with the same arguments and
captures N headless frames from each. The comparison uses stable projections
rather than raw run output, because guest threads are scheduled by the host OS:

  * frames: the set of distinct captured frame images. A run can repeat or
    reorder the same guest image depending on how host and guest threads
    interleave; only the image content is behaviour.
  * trace: the set of distinct NDJSON trace records up to the last guest frame
    every run completed. run_id, build_id, host_time, sequence and guest_frame
    are ignored: poll counts and event placement depend on scheduling, not
    behaviour. Records at or after the common completed-frame boundary are
    still in flight when the host stops and are excluded.

The candidate fails when it produces a frame image that is stable across its
runs but was never observed in the reference, or when it never produces a
frame image that is stable across reference runs. Frame images that appear in
only some runs of one side are reported as scheduling variants and ignored.
Trace record sets must match exactly.

Guest threads are scheduled by the host OS, so even one build does not always
produce the same run sequence. Each side is therefore run several times, and
the candidate passes when the stable projections match. Raise --runs for more
confidence.

Needs the user's game data (--root); exits with status 77 (skipped) when none
is given and GAME_ROOT is not set. The host accepts either the extracted root
or its driving/data folder and resolves the image root from it.

Usage:
  python3 tests/guest/parity.py --reference REFERENCE/m2host --candidate CANDIDATE/m2host \\
      --root path/to/game-data [--frames 60] [--runs 5]
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile

SKIP = 77
IGNORED_TRACE_FIELDS = ("run_id", "build_id", "host_time", "sequence", "guest_frame")


def run_host(binary, root, frames, workdir, extra, timeout=600):
    frames_dir = os.path.join(workdir, "frames")
    trace = os.path.join(workdir, "trace.ndjson")
    cmd = [binary, "--headless", "--frames", str(frames), "--root", root,
           "--capture", frames_dir, "--trace-file", trace] + extra
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True,
                              errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        raise SystemExit("host timed out after %ds: %s" % (timeout, binary))
    if proc.returncode != 0:
        print(proc.stdout[-4000:], file=sys.stderr)
        raise SystemExit("host exited with status %d: %s" % (proc.returncode, binary))
    return frames_dir, trace


def frame_hashes(frames_dir):
    """Return the set of distinct frame image hashes from one capture run."""
    hashes = set()
    for name in sorted(os.listdir(frames_dir)):
        with open(os.path.join(frames_dir, name), "rb") as f:
            hashes.add(hashlib.sha256(f.read()).hexdigest()[:16])
    return hashes


def trace_records(path):
    """Return (records, completed_frames) for one trace file.

    ``records`` is a list of (guest_frame, event_kind, canonical_json).
    guest_frame is removed from canonical_json: it records when the scheduler
    happened to run an emitter, not what the guest did. It is returned
    separately so the caller can choose a common comparison boundary.
    """
    records = []
    completed_frames = 0
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            kind = rec.get("event_kind")
            payload = rec.get("payload") or {}
            guest_frame = int(rec.get("guest_frame") or 0)
            frame = guest_frame
            if kind == "frame_complete":
                # The frame record carries the authoritative sequence number;
                # the outer guest_frame can lag it by one poll.
                seq = payload.get("seq", guest_frame)
                try:
                    seq = int(seq)
                except (TypeError, ValueError):
                    seq = guest_frame
                frame = seq
                completed_frames = max(completed_frames, seq + 1)
            for key in IGNORED_TRACE_FIELDS:
                rec.pop(key, None)
            records.append((frame, kind, json.dumps(rec, sort_keys=True)))
    return records, completed_frames


def trace_union(run_records, boundary):
    """Merge traces from all runs, dropping the unfinished shutdown tail."""
    union = set()
    for records in run_records:
        for guest_frame, kind, canonical in records:
            # Shutdown is harness lifecycle, emitted exactly once, and its
            # guest frame is inherently arbitrary. Keep it; every other record
            # must be inside the common completed-frame prefix.
            if kind == "shutdown" or guest_frame < boundary:
                union.add(canonical)
    return union


def collect(binary, root, frames, runs, extra):
    """Run `binary` repeatedly.

    Returns (per-run frame hash sets, per-run trace records, per-run completed
    guest frame counts).
    """
    frame_runs, trace_runs, completed_runs = [], [], []
    for _ in range(runs):
        with tempfile.TemporaryDirectory() as tmp:
            frames_dir, trace = run_host(binary, root, frames, tmp, extra)
            frame_runs.append(frame_hashes(frames_dir))
            records, completed = trace_records(trace)
            trace_runs.append(records)
            completed_runs.append(completed)
    return frame_runs, trace_runs, completed_runs


def report_frames(ref_runs, cand_runs):
    ref_union = set().union(*ref_runs)
    cand_union = set().union(*cand_runs)
    ref_stable = set.intersection(*ref_runs) if ref_runs else set()
    cand_stable = set.intersection(*cand_runs) if cand_runs else set()
    # A stable image is present in every run of its side, so it is behaviour,
    # not a sampling artefact. Images seen in only some runs are scheduling
    # variants and stay outside the comparison boundary.
    missing = ref_stable - cand_union
    unexpected = cand_stable - ref_union
    ignored = (cand_union - ref_union) - unexpected
    ok = bool(ref_union and cand_union) and not missing and not unexpected
    print("frames: reference %d image(s) (%d stable over %d run(s)), "
          "candidate %d image(s) (%d stable over %d run(s)) -> %s" % (
              len(ref_union), len(ref_stable), len(ref_runs),
              len(cand_union), len(cand_stable), len(cand_runs),
              "ok" if ok else "MISMATCH"))
    if not ref_union or not cand_union:
        print("  no frames captured; check --frames and the host capture path")
    if missing:
        print("  stable reference image(s) never produced by candidate: %s" %
              sorted(missing)[:5])
    if unexpected:
        print("  stable candidate image(s) never produced by reference: %s" %
              sorted(unexpected)[:5])
    if ignored:
        print("  ignored %d candidate-only scheduling variant(s): %s" %
              (len(ignored), sorted(ignored)[:5]))
    return ok


def report_trace(ref, cand, boundary):
    only_ref, only_cand = ref - cand, cand - ref
    print("trace: boundary guest_frame<%d, %d distinct records (reference), %d (candidate), "
          "%d only in reference, %d only in candidate -> %s" % (
              boundary, len(ref), len(cand), len(only_ref), len(only_cand),
              "ok" if not only_ref and not only_cand else "MISMATCH"))
    for label, only in (("reference", only_ref), ("candidate", only_cand)):
        for rec in sorted(only)[:3]:
            print("  only in %s: %s" % (label, rec))
    return not only_ref and not only_cand


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--reference", required=True, help="reference host binary")
    ap.add_argument("--candidate", required=True, help="candidate host binary")
    ap.add_argument("--root", default=os.environ.get("GAME_ROOT"),
                    help="extracted game data folder (default: $GAME_ROOT)")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--runs", type=int, default=5, help="runs per binary (default 5)")
    ap.add_argument("extra", nargs="*", help="extra host arguments")
    args = ap.parse_args(argv)
    if args.frames < 1:
        ap.error("--frames must be at least 1")
    if args.runs < 1:
        ap.error("--runs must be at least 1")
    if not args.root or not os.path.isdir(args.root):
        print("parity: no game data (--root or GAME_ROOT); skipping")
        return SKIP
    ref_frames, ref_trace_runs, ref_completed = collect(
        args.reference, args.root, args.frames, args.runs, args.extra)
    cand_frames, cand_trace_runs, cand_completed = collect(
        args.candidate, args.root, args.frames, args.runs, args.extra)
    boundary = min(ref_completed + cand_completed)
    if boundary == 0:
        # Comparing two shutdown-only traces would pass while proving nothing.
        print("trace: no guest frame completed in every run; nothing to compare -> FAIL")
        return 1
    ref_trace = trace_union(ref_trace_runs, boundary)
    cand_trace = trace_union(cand_trace_runs, boundary)
    if not ref_trace or not cand_trace:
        print("trace: one side produced no records inside the comparison boundary -> FAIL")
        return 1
    ok_frames = report_frames(ref_frames, cand_frames)
    ok_trace = report_trace(ref_trace, cand_trace, boundary)
    return 0 if ok_frames and ok_trace else 1


if __name__ == "__main__":
    sys.exit(main())
