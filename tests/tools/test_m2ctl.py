"""Behaviour tests for tools/m2ctl.py against a fake control socket.

The fake host implements only the wire contract from
host/diagnostics/diagnostic_control.cpp: one tab-separated request line and
one JSON reply line per connection.  An opt-in smoke test launches a real
headless host when M2CTL_SMOKE_BINARY is set; normal CI skips it.
"""
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

import pytest

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
import m2ctl  # noqa: E402

UNIX = sys.platform != "win32" and hasattr(socket, "AF_UNIX")
requires_unix = pytest.mark.skipif(
    not UNIX, reason="the host control socket requires AF_UNIX on POSIX")

FAKE_HOST = r'''#!/usr/bin/env python3
import json
import os
import socket
import sys

arguments = sys.argv[1:]
capture = os.environ.get("FAKE_M2CTL_CAPTURE")
if capture:
    with open(capture, "w", encoding="utf-8") as stream:
        json.dump({"argv": sys.argv, "env": dict(os.environ)}, stream)
socket_path = arguments[arguments.index("--control-socket") + 1]
if not os.path.isabs(socket_path):
    socket_path = os.path.abspath(socket_path)
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(socket_path)
server.listen(8)
while True:
    connection, _ = server.accept()
    with connection:
        data = bytearray()
        while not data.endswith(b"\n"):
            block = connection.recv(65536)
            if not block:
                break
            data.extend(block)
        if not data:
            continue
        fields = data.decode().rstrip("\n").split("\t")
        reply = {"schema": 1, "id": fields[0], "pid": os.getpid(), "ok": True,
                 "error": None, "state": {"paused": True}, "result": {}}
        connection.sendall((json.dumps(reply) + "\n").encode())
        if fields[1] == "quit":
            break
server.close()
try:
    os.unlink(socket_path)
except OSError:
    pass
'''


@pytest.fixture
def short_root():
    """A short temporary root so AF_UNIX paths stay under the sun_path limit."""
    root = Path(tempfile.mkdtemp(
        prefix="m2ctl-test-", dir="/tmp" if os.path.isdir("/tmp") else None))
    try:
        yield root
    finally:
        for pid_file in root.glob("*/supervisor.pid"):
            if (pid_file.parent / "exit.json").exists():
                continue
            try:
                pid = int(pid_file.read_text())
            except (OSError, ValueError):
                continue
            try:
                os.killpg(pid, signal.SIGTERM)
            except (ProcessLookupError, PermissionError, AttributeError):
                continue
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                try:
                    os.killpg(pid, 0)
                except (ProcessLookupError, PermissionError):
                    break
                time.sleep(0.05)
            else:
                try:
                    os.killpg(pid, signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    pass
        time.sleep(0.05)
        shutil.rmtree(root, ignore_errors=True)


class FakeControlServer:
    """Accept one or more client connections and answer with canned replies."""

    def __init__(self, session, responder, delay=0.0):
        self.session = Path(session)
        self.path = self.session / "control.sock"
        self.responder = responder
        self.delay = delay
        self.requests = []
        self.errors = []
        self._stop = threading.Event()
        if self.path.exists():
            self.path.unlink()
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._socket.bind(str(self.path))
        self._socket.listen(8)
        self._socket.settimeout(0.05)
        self._thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, exc_type, exc, traceback):
        self._stop.set()
        try:
            self._socket.close()
        except OSError:
            pass
        self._thread.join(timeout=5)
        try:
            self.path.unlink()
        except OSError:
            pass

    def _run(self):
        while not self._stop.is_set():
            try:
                connection, _ = self._socket.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with connection:
                connection.settimeout(2)
                data = bytearray()
                try:
                    while not data.endswith(b"\n"):
                        block = connection.recv(65536)
                        if not block:
                            break
                        data.extend(block)
                    if not data:
                        continue
                    self.requests.append(bytes(data))
                    if self.delay:
                        time.sleep(self.delay)
                    try:
                        reply = self.responder(bytes(data))
                    except Exception as exc:  # pragma: no cover - test bug
                        self.errors.append(exc)
                        continue
                    if reply:
                        connection.sendall(reply)
                except OSError as exc:
                    self.errors.append(exc)


def response_for(wire, *, pid=4242, ok=True, error=None, state=None,
                 result=None, schema=1, request_id=None):
    fields = wire.decode("utf-8", "replace").rstrip("\n").split("\t")
    if request_id is None:
        request_id = fields[0] if fields else ""
    if state is None:
        state = {"paused": True}
    if result is None:
        result = {}
    return (json.dumps({
        "schema": schema,
        "id": request_id,
        "pid": pid,
        "ok": ok,
        "error": error,
        "state": state,
        "result": result,
    }) + "\n").encode()


def run_cli(root, session_name, *arguments, timeout=30, request_id="req-1",
            with_request_id=True, env=None):
    command = [sys.executable, str(TOOLS / "m2ctl.py"),
               "--session", str(session_name)]
    if with_request_id:
        command.extend(("--request-id", request_id))
    command.extend(arguments)
    return subprocess.run(command, cwd=root, capture_output=True, text=True,
                          timeout=timeout, env=env)


def payload(process):
    try:
        return json.loads(process.stdout)
    except ValueError as exc:
        raise AssertionError(
            f"stdout is not JSON: {process.stdout!r}\n"
            f"stderr: {process.stderr!r}") from exc


def write_fake_host(path, body=FAKE_HOST):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body)
    path.chmod(0o755)
    return path


def test_help_works_from_any_directory(tmp_path):
    process = subprocess.run(
        [sys.executable, str(TOOLS / "m2ctl.py"), "--help"],
        cwd=tmp_path, capture_output=True, text=True)
    assert process.returncode == 0
    assert "tools/m2ctl.py" in process.stdout
    assert "--control-socket" in process.stdout or "control socket" in process.stdout

    process = subprocess.run(
        [sys.executable, str(TOOLS / "m2ctl.py"), "status", "--help"],
        cwd=tmp_path, capture_output=True, text=True)
    assert process.returncode == 0


def test_encode_request_rejects_framing_breaks():
    assert m2ctl.encode_request(["id", "wait", "secondary", "eq", "4"]) == \
        b"id\twait\tsecondary\teq\t4\n"
    for broken in ("tab\there", "line\nfeed", "carriage\rreturn", "nul\0byte"):
        with pytest.raises(ValueError):
            m2ctl.encode_request(["id", "read", broken])
    with pytest.raises(ValueError):
        m2ctl.encode_request(["", "status"])
    with pytest.raises(ValueError):
        m2ctl.encode_request(["x" * 65, "status"])
    with pytest.raises(ValueError):
        m2ctl.encode_request(["id", "read", "x" * 5000])


def test_host_build_type_searches_ancestor_caches(tmp_path):
    binary = tmp_path / "build" / "host" / "m2host"
    binary.parent.mkdir(parents=True)
    binary.write_text("")
    cache = tmp_path / "build" / "CMakeCache.txt"
    cache.write_text("# cache\nCMAKE_BUILD_TYPE:STRING=Release\n")
    assert m2ctl.host_build_type(binary) == "Release"
    cache.write_text("CMAKE_BUILD_TYPE:STRING=\n")
    assert m2ctl.host_build_type(binary) is None
    cache.unlink()
    assert m2ctl.host_build_type(binary) is None


def test_socket_target_reports_missing_socket(tmp_path):
    session = tmp_path / "session"
    session.mkdir()
    with pytest.raises(m2ctl.ClientError) as info:
        m2ctl.socket_target(session)
    assert info.value.outcome == "not_sent"
    assert "no control socket" in str(info.value)


FRAMING_CASES = [
    (["status"], "status"),
    (["graphics"], "graphics"),
    (["pause"], "pause"),
    (["quit"], "quit"),
    (["tap", "a"], "input\t1\t128\t128\t1\t1800\t30000"),
    (["tap", "left"], "input\t1024\t0\t128\t1\t1800\t30000"),
    (["tap", "0x10"], "input\t16\t128\t128\t1\t1800\t30000"),
    (["hold", "b", "--x", "5", "--y", "6", "--timeout", "2",
      "--max-ticks", "7"], "input\t2\t5\t6\t0\t7\t2000"),
    (["release"], "input\t0\t128\t128\t0\t1800\t30000"),
    (["release", "--x", "5", "--y", "6"], "input\t0\t5\t6\t0\t1800\t30000"),
    (["advance", "5", "--timeout", "2"], "advance\t5\t2000"),
    (["wait", "secondary", "eq", "4", "--timeout", "3",
      "--max-ticks", "9"], "wait\tsecondary\teq\t4\t9\t3000"),
    (["read", "0x10", "4"], "read\t16\t4"),
    (["watch", "0x10", "4", "--label", "footer"], "watch\t16\t4\tfooter"),
]


@requires_unix
@pytest.mark.parametrize("arguments,expected", FRAMING_CASES)
def test_request_framing(short_root, arguments, expected):
    session = short_root / "s"
    session.mkdir()
    with FakeControlServer(session, lambda wire: response_for(wire)) as server:
        process = run_cli(short_root, "s", *arguments)
    assert process.returncode == 0, process.stderr
    assert payload(process)["ok"] is True
    assert [request.decode() for request in server.requests] == \
        [f"req-1\t{expected}\n"]


@requires_unix
@pytest.mark.parametrize("arguments,verb,extra", [
    (["capture", "out.ppm"], "capture", []),
    (["capture", "nested/out.ppm"], "capture", []),
    (["provenance", "out.json"], "provenance", []),
    (["trace", "events.ndjson", "--kinds", "input,frame_complete"],
     "trace", ["input,frame_complete"]),
])
def test_output_paths_are_absolute_inside_session(short_root, arguments, verb,
                                                  extra):
    session = short_root / "s"
    session.mkdir()
    with FakeControlServer(session, lambda wire: response_for(wire)) as server:
        process = run_cli(short_root, "s", *arguments)
    assert process.returncode == 0, process.stderr
    expected = "\t".join(["req-1", verb,
                          str((session / arguments[1]).resolve()), *extra])
    assert [request.decode() for request in server.requests] == [expected + "\n"]
    assert (session / arguments[1]).parent.is_dir()


@requires_unix
def test_output_path_cannot_escape_session(short_root):
    session = short_root / "s"
    session.mkdir()
    with FakeControlServer(session, lambda wire: response_for(wire)) as server:
        process = run_cli(short_root, "s", "capture", "../escape.ppm")
        absolute_outside = short_root / "outside.ppm"
        second = run_cli(short_root, "s", "capture", str(absolute_outside))
    assert process.returncode == 2
    assert second.returncode == 2
    assert payload(process)["ok"] is False
    assert "inside the session" in payload(process)["error"]
    assert server.requests == []
    assert not (short_root / "escape.ppm").exists()
    assert not absolute_outside.exists()


@requires_unix
def test_host_error_is_exit_status_one(short_root):
    session = short_root / "s"
    session.mkdir()
    with FakeControlServer(
            session,
            lambda wire: response_for(
                wire, ok=False, error="tick_budget_exhausted",
                result={"ticks_advanced": 1800})):
        process = run_cli(short_root, "s", "advance", "5")
    assert process.returncode == 1
    reply = payload(process)
    assert reply["ok"] is False
    assert reply["error"] == "tick_budget_exhausted"
    assert reply["result"]["ticks_advanced"] == 1800


@requires_unix
@pytest.mark.parametrize("responder,message", [
    (lambda wire: b"{not json}\n", "not JSON"),
    (lambda wire: response_for(wire, schema=2), "schema"),
    (lambda wire: response_for(wire, request_id="other"), "identity mismatch"),
    (lambda wire: None, "disconnected"),
])
def test_protocol_failures_exit_status_two(short_root, responder, message):
    session = short_root / "s"
    session.mkdir()
    with FakeControlServer(session, responder):
        process = run_cli(short_root, "s", "status")
    assert process.returncode == 2
    reply = payload(process)
    assert reply["ok"] is False
    assert message in reply["error"]
    assert reply["outcome"] == "unconfirmed"


@requires_unix
def test_reply_without_boolean_ok_is_rejected(short_root):
    session = short_root / "s"
    session.mkdir()
    def responder(wire):
        fields = wire.decode().rstrip("\n").split("\t")
        return (json.dumps({"schema": 1, "id": fields[0], "pid": 1,
                            "state": {"paused": True}, "result": {}}) + "\n").encode()
    with FakeControlServer(session, responder):
        process = run_cli(short_root, "s", "status")
    assert process.returncode == 2
    assert "boolean ok" in payload(process)["error"]


@requires_unix
def test_reply_without_integer_pid_is_rejected(short_root):
    session = short_root / "s"
    session.mkdir()

    def responder(wire):
        fields = wire.decode().rstrip("\n").split("\t")
        return (json.dumps({"schema": 1, "id": fields[0], "ok": True,
                            "state": {"paused": True},
                            "result": {}}) + "\n").encode()

    with FakeControlServer(session, responder):
        process = run_cli(short_root, "s", "status")
    assert process.returncode == 2
    assert "integer pid" in payload(process)["error"]


@requires_unix
def test_pid_mismatch_refuses_a_different_host(short_root):
    session = short_root / "s"
    session.mkdir()
    (session / "session.json").write_text(json.dumps({"pid": 111}) + "\n")
    with FakeControlServer(session, lambda wire: response_for(wire, pid=222)):
        process = run_cli(short_root, "s", "status")
    assert process.returncode == 2
    reply = payload(process)
    assert "PID mismatch" in reply["error"]
    assert reply["outcome"] == "unconfirmed"


def test_malformed_session_identity_is_rejected_before_sending(tmp_path):
    session = tmp_path / "s"
    session.mkdir()
    (session / "session.json").write_text("{")
    process = subprocess.run(
        [sys.executable, str(TOOLS / "m2ctl.py"), "--session", str(session),
         "--request-id", "req-1", "status"],
        cwd=tmp_path, capture_output=True, text=True)
    assert process.returncode == 2
    reply = payload(process)
    assert "not valid JSON" in reply["error"]
    assert reply["outcome"] == "not_sent"


@requires_unix
@pytest.mark.parametrize("arguments", [
    ["capture", "../escape.ppm"],
    ["tap", "bogus"],
    ["advance", "0"],
    ["read", "-1", "4"],
    ["watch", "0", "0"],
    ["tap", "a", "--timeout", "0"],
    ["tap", "a", "--max-ticks", "0"],
    ["trace", "events.ndjson"],
])
def test_invalid_commands_never_open_a_socket(short_root, arguments):
    session = short_root / "s"
    session.mkdir()
    with FakeControlServer(session, lambda wire: response_for(wire)) as server:
        process = run_cli(short_root, "s", *arguments)
    assert process.returncode == 2
    assert server.requests == []


@requires_unix
def test_reply_size_limit(short_root, monkeypatch):
    session = short_root / "s"
    session.mkdir()
    monkeypatch.setattr(m2ctl, "MAX_REPLY_BYTES", 64)
    with FakeControlServer(session, lambda wire: b"x" * 128 + b"\n"):
        with pytest.raises(m2ctl.ClientError) as info:
            m2ctl.request(session, "status", timeout=2)
    assert "exceeds" in str(info.value)
    assert info.value.outcome == "unconfirmed"


@requires_unix
def test_wait_timeout_is_unconfirmed(short_root):
    session = short_root / "s"
    session.mkdir()
    with FakeControlServer(session, lambda wire: None, delay=0.5):
        with pytest.raises(m2ctl.ClientError) as info:
            m2ctl.request(session, "status", timeout=0.1)
    assert "timed out" in str(info.value)
    assert info.value.outcome == "unconfirmed"


@requires_unix
def test_audit_log_is_bounded(short_root, monkeypatch):
    session = short_root / "s"
    session.mkdir()
    monkeypatch.setattr(m2ctl, "MAX_AUDIT_BYTES", 1)
    with FakeControlServer(session, lambda wire: response_for(wire)):
        first = m2ctl.request(session, "status", timeout=2, request_id="a")
        second = m2ctl.request(session, "status", timeout=2, request_id="b")
    assert first["ok"] is True and second["ok"] is True
    lines = (session / "commands.ndjson").read_text().splitlines()
    assert len(lines) == 1
    record = json.loads(lines[0])
    assert record["request"][1] == "status"


@requires_unix
def test_start_launches_fake_host_and_quit_stops_it(short_root):
    binary = write_fake_host(short_root / "bin" / "m2host")
    process = run_cli(
        short_root, "session", "start", "--binary", str(binary),
        "--startup-timeout", "10", "--backend", "software",
        with_request_id=False, timeout=30)
    assert process.returncode == 0, process.stdout + process.stderr
    started = payload(process)
    assert started["ok"] is True
    assert started["state"]["paused"] is True
    session = short_root / "session"
    assert (session / "session.json").is_file()
    assert (session / "launch.json").is_file()
    assert (session / "supervisor.pid").is_file()
    assert (session / "host.pid").is_file()
    assert (session / "host.log").is_file()
    identity = json.loads((session / "session.json").read_text())
    assert identity["pid"] == started["pid"]
    assert identity["binary_sha256"] == m2ctl.digest(binary)
    assert identity["build_type"] is None
    assert identity["command"][:2] == [str(binary.resolve()),
                                        "--control-socket"]
    assert "--headless" in identity["command"]

    quit_process = run_cli(short_root, "session", "quit")
    assert quit_process.returncode == 0, quit_process.stderr
    assert payload(quit_process)["ok"] is True
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not (session / "exit.json").exists():
        time.sleep(0.05)
    record = json.loads((session / "exit.json").read_text())
    assert record["exit_code"] == 0
    assert not (session / "control.sock").exists()


@requires_unix
def test_start_forwards_windowed_options_and_clears_m2_environment(short_root):
    binary = write_fake_host(short_root / "bin" / "m2host")
    game_data = short_root / "data"
    game_data.mkdir()
    capture = short_root / "fake-host.json"
    environment = dict(os.environ, FAKE_M2CTL_CAPTURE=str(capture), M2_LEAK="1")
    process = run_cli(
        short_root, "session", "start", "--binary", str(binary),
        "--backend", "cel", "--game-data", str(game_data),
        "--width", "640", "--height", "480", "--perf-file", "perf.json",
        "--env", "M2_FOO=bar", with_request_id=False, env=environment)
    assert process.returncode == 0, process.stdout + process.stderr
    recorded = json.loads(capture.read_text())
    arguments = recorded["argv"]
    child_environment = recorded["env"]
    assert "--headless" not in arguments
    assert child_environment["M2_GL_BACKEND"] == "cel"
    assert child_environment["M2_GL_SWAP_INTERVAL"] == "0"
    assert child_environment["M2_FOO"] == "bar"
    assert "M2_LEAK" not in child_environment
    assert str(game_data.resolve()) in arguments
    assert arguments[arguments.index("--width") + 1] == "640"
    assert arguments[arguments.index("--height") + 1] == "480"
    session = short_root / "session"
    assert str((session / "perf.json").resolve()) in arguments
    assert (session / "perf.json").parent.is_dir()
    launch = json.loads((session / "launch.json").read_text())
    assert launch["backend"] == "cel"
    assert launch["environment"] == {"M2_FOO": "bar"}
    quit_process = run_cli(short_root, "session", "quit",
                           with_request_id=False, env=environment)
    assert quit_process.returncode == 0, quit_process.stderr


@requires_unix
def test_start_reports_early_host_exit(short_root):
    binary = write_fake_host(
        short_root / "bin" / "m2host",
        "#!/usr/bin/env python3\nimport sys\nsys.exit(3)\n")
    process = run_cli(
        short_root, "session", "start", "--binary", str(binary),
        "--startup-timeout", "5", with_request_id=False)
    assert process.returncode == 2
    reply = payload(process)
    assert reply["ok"] is False
    assert "during startup" in reply["error"]
    session = short_root / "session"
    assert (session / "exit.json").is_file()
    assert json.loads((session / "exit.json").read_text())["exit_code"] == 3
    assert (session / "host.log").is_file()


@requires_unix
def test_start_rejects_missing_binary_without_creating_session(short_root):
    process = run_cli(
        short_root, "session", "start", "--binary", str(short_root / "missing"),
        with_request_id=False)
    assert process.returncode == 2
    assert "not found" in payload(process)["error"]
    assert not (short_root / "session").exists()


@requires_unix
def test_start_rejects_unoptimized_host_without_allow_debug(short_root):
    binary = write_fake_host(short_root / "bin" / "m2host")
    (short_root / "CMakeCache.txt").write_text(
        "CMAKE_BUILD_TYPE:STRING=Debug\n")
    process = run_cli(
        short_root, "session", "start", "--binary", str(binary),
        with_request_id=False)
    assert process.returncode == 2
    assert "optimized host" in payload(process)["error"]
    assert not (short_root / "session").exists()


@requires_unix
def test_start_allows_debug_with_explicit_flag(short_root):
    binary = write_fake_host(short_root / "bin" / "m2host")
    (short_root / "CMakeCache.txt").write_text(
        "CMAKE_BUILD_TYPE:STRING=Debug\n")
    process = run_cli(
        short_root, "session", "start", "--binary", str(binary),
        "--allow-debug", with_request_id=False)
    assert process.returncode == 0, process.stdout + process.stderr
    assert payload(process)["state"]["paused"] is True
    quit_process = run_cli(short_root, "session", "quit",
                           with_request_id=False)
    assert quit_process.returncode == 0, quit_process.stderr


@requires_unix
def test_start_rejects_existing_session(short_root):
    binary = write_fake_host(short_root / "bin" / "m2host")
    (short_root / "session").mkdir()
    process = run_cli(
        short_root, "session", "start", "--binary", str(binary),
        with_request_id=False)
    assert process.returncode == 2
    assert "already exists" in payload(process)["error"]


@requires_unix
def test_start_rejects_invalid_environment_override(short_root):
    binary = write_fake_host(short_root / "bin" / "m2host")
    process = run_cli(
        short_root, "session", "start", "--binary", str(binary),
        "--env", "NOT_M2=1", with_request_id=False)
    assert process.returncode == 2
    assert "M2_NAME=value" in payload(process)["error"]
    assert not (short_root / "session").exists()


SMOKE_BINARY = os.environ.get("M2CTL_SMOKE_BINARY")
SMOKE_DATA = os.environ.get("M2CTL_SMOKE_DATA") or os.environ.get("IMSA_DATA_DIR")


@requires_unix
@pytest.mark.skipif(
    not SMOKE_BINARY,
    reason="set M2CTL_SMOKE_BINARY (and optionally M2CTL_SMOKE_DATA) "
           "to launch a real headless host")
def test_real_headless_host_smoke(short_root):
    binary = Path(SMOKE_BINARY).expanduser().resolve()
    card = short_root / "card"
    card.mkdir()
    arguments = ["start", "--binary", str(binary),
                 "--startup-timeout", "120", "--backend", "software",
                 "--env", f"M2_MICROCARD_ROOT={card}"]
    if SMOKE_DATA:
        arguments.extend(("--game-data", str(Path(SMOKE_DATA).expanduser().resolve())))
    process = run_cli(short_root, "smoke", *arguments,
                      with_request_id=False, timeout=180)
    assert process.returncode == 0, process.stdout + process.stderr
    started = payload(process)
    assert started["state"]["paused"] is True

    session = short_root / "smoke"
    launch = json.loads((session / "launch.json").read_text())
    assert launch["environment"]["M2_MICROCARD_ROOT"] == str(card)
    identity = json.loads((session / "session.json").read_text())
    assert identity["environment"]["M2_MICROCARD_ROOT"] == str(card)
    status = run_cli(short_root, "smoke", "status", timeout=60,
                     with_request_id=False)
    assert status.returncode == 0, status.stderr
    assert payload(status)["state"]["paused"] is True

    read = run_cli(short_root, "smoke", "read", "0", "16", timeout=60,
                   with_request_id=False)
    assert read.returncode == 0, read.stdout + read.stderr
    assert len(payload(read)["result"]["hex"]) == 32

    advance = run_cli(short_root, "smoke", "advance", "1",
                      "--timeout", "10", timeout=60, with_request_id=False)
    assert advance.returncode == 0, advance.stdout + advance.stderr
    assert payload(advance)["result"]["ticks_advanced"] == 1

    graphics = run_cli(short_root, "smoke", "graphics", timeout=60,
                       with_request_id=False)
    assert graphics.returncode == 0, graphics.stdout + graphics.stderr

    capture = run_cli(short_root, "smoke", "capture", "screen.ppm", timeout=60,
                      with_request_id=False)
    assert capture.returncode == 0, capture.stdout + capture.stderr
    image = session / "screen.ppm"
    assert image.read_bytes()[:2] == b"P6"

    quit_process = run_cli(short_root, "smoke", "quit", timeout=60,
                           with_request_id=False)
    assert quit_process.returncode == 0, quit_process.stdout + quit_process.stderr
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and not (session / "exit.json").exists():
        time.sleep(0.05)
    assert json.loads((session / "exit.json").read_text())["exit_code"] == 0
    host_log = (session / "host.log").read_text(errors="replace")
    assert f'[fs] microcard volume "/card" -> {card}' in host_log
    assert "default; set M2_MICROCARD_ROOT to relocate" not in host_log
