#!/usr/bin/env python3
"""Host CLI status regressions for recordings, replays and capture output.

Runs the stub-guest m2host and checks that a requested stream/image that
cannot be produced exits nonzero before/as the request fails, while valid
recordings, replays, screenshots and captures still succeed.  Parent
obstruction uses a regular file, not permission bits, so it behaves the same
under root and on Windows.
"""
import argparse
import os
import signal
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

try:
    import resource
except ImportError:  # pragma: no cover - Windows
    resource = None

FAILURES = []


def check(condition, message):
    if condition:
        return
    FAILURES.append(message)
    print(f"FAIL: {message}")


def invoke(host, work, args, preexec_fn=None, env=None):
    # Headless with one bounded host frame: the same shape as the review
    # reproductions, and a valid request must terminate rather than enter an
    # interactive loop.
    return subprocess.run(
        [str(host), "--headless", "--frames", "1", *args],
        cwd=str(work),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
        preexec_fn=preexec_fn,
        env=env,
    )


def output(proc):
    return (proc.stdout + proc.stderr).decode("utf-8", "replace")


def guest_started(proc):
    return "m2_game_entry STUB reached" in output(proc)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True, type=Path)
    args = parser.parse_args()
    host = args.host.resolve()
    if not host.is_file():
        print(f"FAIL: host binary not found: {host}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="m2_evidence_cli_") as tmp:
        work = Path(tmp)
        blocker = work / "blocked-parent"
        blocker.write_text("not a directory\n", encoding="utf-8")

        # Every fixture is written before any host starts: the invocations
        # below run concurrently (process start-up dominates on Windows) and
        # each one owns its own output path.
        empty_replay = work / "empty.ndjson"
        empty_replay.write_bytes(b"")

        replay_path = work / "replay.ndjson"
        replay_path.write_text(
            '{"sequence":0,"host_time":0,"guest_frame":0,'
            '"buttons":1,"pressed":1,"released":0,'
            '"stick_x":128,"stick_y":128,"device":0}\n',
            encoding="utf-8",
        )

        broker_replay_path = work / "broker-replay.ndjson"
        broker_replay_path.write_text(
            '{"sequence":0,"host_time":0,"guest_frame":0,'
            '"what":7,"x":-3,"y":4,"data":99}\n',
            encoding="utf-8",
        )

        unicode_dir = work / "evidence-é-日本"
        unicode_dir.mkdir()
        unicode_replay = unicode_dir / "replay.ndjson"
        unicode_replay.write_text(
            '{"sequence":0,"host_time":0,"guest_frame":0,'
            '"buttons":1,"pressed":1,"released":0,'
            '"stick_x":128,"stick_y":128,"device":0}\n',
            encoding="utf-8",
        )

        # Limit the child's file size so the header/payload writes fail rather
        # than only fopen.  SIGXFSZ is ignored so the host observes EFBIG and
        # reports it instead of being killed by the signal.  preexec_fn is not
        # safe once other threads exist, so this one runs before the pool.
        short_shot_proc = None
        if os.name == "posix" and resource is not None:
            short_shot = work / "short.ppm"

            def limit_file_size():
                signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
                resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))

            short_shot_proc = invoke(
                host, work,
                ["--screenshot", str(short_shot), "--width", "8", "--height", "6"],
                preexec_fn=limit_file_size,
            )

        steering_env = dict(os.environ, M2_IMPROVED_ANALOG_STEERING="1")
        record_path = work / "record.ndjson"
        broker_path = work / "broker-record.ndjson"
        blocked_shot = blocker / "shot.ppm"
        valid_shot = work / "shot.ppm"
        unreachable_shot = work / "unreachable.ppm"
        blocked_capture = blocker / "capture"
        valid_capture = work / "capture"
        unicode_record = unicode_dir / "record.ndjson"
        unicode_shot = unicode_dir / "shot.ppm"
        unicode_capture = unicode_dir / "capture"

        with ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 4)) as pool:
            def start(arguments, env=None):
                return pool.submit(invoke, host, work, arguments, env=env)

            steering = [
                start([recording, str(work / f"steering{recording[1:]}.ndjson")],
                      env=steering_env)
                for recording in ("--record-input", "--record-broker")
            ]
            has_guest = start(["--has-guest"])
            missing_replay = start(["--replay-input", str(work / "missing.ndjson")])
            empty_replay_run = start(["--replay-input", str(empty_replay)])
            blocked_record = start(["--record-input", str(blocker / "record.ndjson")])
            blocked_broker = start(["--record-broker", str(blocker / "broker.ndjson")])
            missing_broker = start(
                ["--replay-broker", str(work / "missing-broker.ndjson")])
            valid_record = start(["--record-input", str(record_path)])
            valid_broker = start(["--record-broker", str(broker_path)])
            valid_replay = start(["--replay-input", str(replay_path)])
            valid_broker_replay = start(["--replay-broker", str(broker_replay_path)])
            blocked_shot_run = start(["--screenshot", str(blocked_shot)])
            valid_shot_run = start(
                ["--screenshot", str(valid_shot), "--width", "8", "--height", "6"])
            unreachable_shot_run = start(
                ["--screenshot", str(unreachable_shot),
                 "--screenshot-frame", "5", "--frames", "1"])
            blocked_capture_run = start(["--capture", str(blocked_capture)])
            valid_capture_run = start(
                ["--capture", str(valid_capture), "--width", "8", "--height", "6"])
            unicode_record_run = start(["--record-input", str(unicode_record)])
            unicode_replay_run = start(["--replay-input", str(unicode_replay)])
            unicode_shot_run = start(
                ["--screenshot", str(unicode_shot), "--width", "8", "--height", "6"])
            unicode_capture_run = start(
                ["--capture", str(unicode_capture), "--width", "8", "--height", "6"])

        for future in steering:
            proc = future.result()
            check(proc.returncode == 2 and "Improved Analog Steering off" in output(proc),
                  "improved steering refuses recording without the required metadata")
            check(not guest_started(proc), "unsupported steering recording does not boot")

        # --- guest capability query (this suite runs the stub host) ----------
        proc = has_guest.result()
        check(proc.returncode == 3, "stub host reports no game with exit status 3")
        check("guest: stub" in output(proc), "stub host names its guest")
        check(not guest_started(proc), "guest query does not enter the guest")

        # --- requested evidence stream failures -----------------------------
        proc = missing_replay.result()
        check(proc.returncode != 0, "missing input replay exits nonzero")
        check("failed to open" in output(proc),
              "missing input replay diagnoses the open failure")
        check("aborting before guest execution" in output(proc),
              "missing input replay aborts before guest execution")
        check(not guest_started(proc),
              "missing input replay does not enter the guest")

        proc = empty_replay_run.result()
        check(proc.returncode != 0, "empty input replay exits nonzero")
        check("empty or no input events" in output(proc),
              "empty input replay diagnoses the empty recording")
        check(not guest_started(proc),
              "empty input replay does not enter the guest")

        proc = blocked_record.result()
        check(proc.returncode != 0, "unwritable input recording exits nonzero")
        check("failed to open" in output(proc),
              "unwritable input recording diagnoses the open failure")
        check("aborting before guest execution" in output(proc),
              "unwritable input recording aborts before guest execution")
        check(not guest_started(proc),
              "unwritable input recording does not enter the guest")

        proc = blocked_broker.result()
        check(proc.returncode != 0, "unwritable broker recording exits nonzero")
        check("broker failed to open" in output(proc),
              "unwritable broker recording diagnoses the open failure")
        check(not guest_started(proc),
              "unwritable broker recording does not enter the guest")

        proc = missing_broker.result()
        check(proc.returncode != 0, "missing broker replay exits nonzero")
        check("broker failed to open" in output(proc),
              "missing broker replay diagnoses the open failure")
        check(not guest_started(proc),
              "missing broker replay does not enter the guest")

        # --- valid evidence streams -----------------------------------------
        proc = valid_record.result()
        check(proc.returncode == 0, "valid input recording exits zero")
        check(record_path.is_file(), "valid input recording creates its file")
        check(guest_started(proc), "valid input recording enters the guest")

        proc = valid_broker.result()
        check(proc.returncode == 0, "valid broker recording exits zero")
        check(broker_path.is_file(), "valid broker recording creates its file")
        check(guest_started(proc), "valid broker recording enters the guest")

        proc = valid_replay.result()
        check(proc.returncode == 0, "valid input replay exits zero")
        check("replaying 1 records" in output(proc),
              "valid input replay loads the recording")
        check(guest_started(proc), "valid input replay enters the guest")

        proc = valid_broker_replay.result()
        check(proc.returncode == 0, "valid broker replay exits zero")
        check("broker replaying 1 records" in output(proc),
              "valid broker replay loads the recording")
        check(guest_started(proc), "valid broker replay enters the guest")

        # --- screenshots -----------------------------------------------------
        proc = blocked_shot_run.result()
        check(proc.returncode != 0, "blocked screenshot exits nonzero")
        check("[screenshot]" in output(proc),
              "blocked screenshot diagnoses the output failure")
        check(not blocked_shot.exists(), "blocked screenshot writes no file")

        if short_shot_proc is not None:
            check(short_shot_proc.returncode != 0,
                  "short screenshot write exits nonzero")
            check("[screenshot]" in output(short_shot_proc),
                  "short screenshot write diagnoses the output failure")

        proc = valid_shot_run.result()
        check(proc.returncode == 0, "valid screenshot exits zero")
        check(valid_shot.is_file(), "valid screenshot writes its file")
        if valid_shot.is_file():
            data = valid_shot.read_bytes()
            header = b"P6\n8 6\n255\n"
            check(data.startswith(header), "valid screenshot writes a P6 header")
            check(len(data) == len(header) + 8 * 6 * 3,
                  "valid screenshot writes a complete payload")

        proc = unreachable_shot_run.result()
        check(proc.returncode != 0,
              "unreached screenshot frame exits nonzero")
        check("not written" in output(proc),
              "unreached screenshot frame diagnoses the missing output")
        check(not unreachable_shot.exists(),
              "unreached screenshot frame writes no file")

        # --- captures --------------------------------------------------------
        proc = blocked_capture_run.result()
        check(proc.returncode != 0, "blocked capture exits nonzero")
        check("[capture]" in output(proc),
              "blocked capture diagnoses the output failure")
        check(not blocked_capture.exists(), "blocked capture writes no directory")

        proc = valid_capture_run.result()
        check(proc.returncode == 0, "valid capture exits zero")
        frames = sorted(valid_capture.glob("frame_*.ppm")) if valid_capture.is_dir() else []
        check(len(frames) == 1, "valid capture writes one frame")
        if len(frames) == 1:
            data = frames[0].read_bytes()
            header = b"P6\n8 6\n255\n"
            check(data.startswith(header), "valid capture writes a P6 header")
            check(len(data) == len(header) + 8 * 6 * 3,
                  "valid capture writes a complete payload")

        # --- UTF-8 paths through both streams and both output surfaces ------
        proc = unicode_record_run.result()
        check(proc.returncode == 0, "UTF-8 input recording exits zero")
        check(unicode_record.is_file(),
              "UTF-8 input recording creates its file")

        proc = unicode_replay_run.result()
        check(proc.returncode == 0, "UTF-8 input replay exits zero")
        check("replaying 1 records" in output(proc),
              "UTF-8 input replay loads the recording")

        proc = unicode_shot_run.result()
        check(proc.returncode == 0, "UTF-8 screenshot exits zero")
        check(unicode_shot.is_file(), "UTF-8 screenshot writes its file")

        proc = unicode_capture_run.result()
        check(proc.returncode == 0, "UTF-8 capture exits zero")
        unicode_frames = (
            sorted(unicode_capture.glob("frame_*.ppm"))
            if unicode_capture.is_dir()
            else []
        )
        check(len(unicode_frames) == 1, "UTF-8 capture writes one frame")

    if FAILURES:
        print(f"m2_evidence_cli: {len(FAILURES)} failure(s)", file=sys.stderr)
        return 1
    print("m2_evidence_cli: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
