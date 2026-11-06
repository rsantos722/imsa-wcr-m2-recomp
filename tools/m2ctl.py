#!/usr/bin/env python3
"""Control a suspended IMSA m2host diagnostic session.

m2host owns the guest; this client only frames one-line tab-separated
requests on a local Unix-domain socket and prints the JSON response.  The
client uses only the Python standard library and never builds the host.

Examples:
  python3 tools/m2ctl.py --session /tmp/m2-session start --binary build/host/m2host
  python3 tools/m2ctl.py --session /tmp/m2-session status
  python3 tools/m2ctl.py --session /tmp/m2-session wait secondary eq 4
  python3 tools/m2ctl.py --session /tmp/m2-session tap a
  python3 tools/m2ctl.py --session /tmp/m2-session capture screen.ppm
  python3 tools/m2ctl.py --session /tmp/m2-session quit

A session directory must be short enough for an AF_UNIX path and must be new
for start.  The control socket is not available on Windows; the Windows host
compiles the transport out.  See tools/README.md for the full contract.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time
import uuid

REPO = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = REPO / "build" / "host" / "m2host"
MAX_WIRE_BYTES = 4096
MAX_REPLY_BYTES = 2 * 1024 * 1024
MAX_LOG_BYTES = 8 * 1024 * 1024
MAX_AUDIT_BYTES = 1024 * 1024
# Conservative AF_UNIX sun_path limit: 104 bytes on macOS including the NUL.
MAX_SOCKET_PATH_BYTES = 103
OPTIMIZED_BUILD_TYPES = ("Release", "RelWithDebInfo", "MinSizeRel")
BUTTONS = {
    "a": 1, "b": 2, "c": 4, "start": 8,
    "l": 16, "r": 32, "x": 64, "lookback": 128,
    "up": 256, "down": 512, "left": 1024, "right": 2048,
    "none": 0,
}
AXES = {
    "up": (128, 0), "down": (128, 255),
    "left": (0, 128), "right": (255, 128),
}

EPILOG = """\
Protocol and session:
  m2host must be started with --control-socket PATH.  Each request is one
  tab-separated line terminated by LF; each reply is one JSON object
  terminated by LF.  The client checks the reply schema, request id, and
  (when session.json exists) the host pid.

  start creates a new session directory and launches an already-built host.
  It never configures, builds, or downloads anything.  The other commands
  attach to SESSION/control.sock, so a host launched by hand works too.

  Exit status: 0 request succeeded, 1 host reported ok=false, 2 client,
  transport, startup, or protocol failure.  A lost reply is reported with
  outcome "unconfirmed"; do not blindly retry an input command.

Limits:
  Unix-domain sockets only (macOS and Linux).  Keep the session path short.
  No memory-write command exists.  Advancing commands require positive tick
  and wall-clock budgets; the host enforces 12000 ticks and 120 seconds.

See tools/README.md for the command and evidence contract.
"""


class ClientError(RuntimeError):
    """A failure that carries whether the request may have reached the host."""

    def __init__(self, message, outcome="unconfirmed"):
        super().__init__(message)
        self.outcome = outcome


def digest(path):
    """SHA-256 of a file without loading it into memory."""
    hasher = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def host_build_type(binary):
    """CMAKE_BUILD_TYPE recorded in a cache beside or above the executable."""
    binary = Path(binary).resolve()
    for directory in (binary.parent, binary.parent.parent,
                      binary.parent.parent.parent):
        cache = directory / "CMakeCache.txt"
        if not cache.is_file():
            continue
        try:
            lines = cache.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return None
        for line in lines:
            if line.startswith("CMAKE_BUILD_TYPE:"):
                value = line.partition("=")[2].strip()
                return value or None
        return None
    return None


def encode_request(fields):
    """Encode one protocol request, rejecting values that would break framing."""
    fields = [str(field) for field in fields]
    if len(fields) < 2:
        raise ValueError("a request needs a request id and a verb")
    request_id, verb = fields[0], fields[1]
    if not request_id:
        raise ValueError("request id must not be empty")
    if len(request_id.encode("utf-8")) > 64:
        raise ValueError("request id exceeds the host's 64-byte limit")
    if not verb:
        raise ValueError("verb must not be empty")
    for field in fields:
        if any(character in field for character in "\t\r\n\0"):
            raise ValueError("protocol fields cannot contain tabs, newlines, or NUL")
    wire = ("\t".join(fields) + "\n").encode("utf-8")
    if len(wire) > MAX_WIRE_BYTES:
        raise ValueError(f"request exceeds {MAX_WIRE_BYTES} bytes")
    return wire


def load_identity(session):
    """Recorded host identity, or None for a host launched by hand."""
    path = Path(session) / "session.json"
    if not path.exists():
        return None
    try:
        identity = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ValueError(f"{path} is not valid JSON: {exc}") from exc
    if not isinstance(identity, dict) or not isinstance(identity.get("pid"), int):
        raise ValueError(f"{path} does not record an integer host pid")
    return identity


def socket_target(session):
    """A short path plus an optional working directory for connect()."""
    path = Path(session) / "control.sock"
    if not path.exists():
        raise ClientError(
            f"no control socket at {path}; start m2host with --control-socket "
            "or use the start command", "not_sent")
    if not path.is_socket():
        raise ClientError(f"{path} is not a Unix-domain socket", "not_sent")
    relative = os.path.relpath(path, Path.cwd())
    if len(os.fsencode(relative)) <= MAX_SOCKET_PATH_BYTES:
        return relative, None
    absolute = str(path)
    if len(os.fsencode(absolute)) <= MAX_SOCKET_PATH_BYTES:
        return absolute, None
    # The socket exists but neither spelling fits.  Connect from its parent
    # directory so the final component is the only path the kernel sees.
    short = f"{Path(session).name}/control.sock"
    if len(os.fsencode(short)) > MAX_SOCKET_PATH_BYTES:
        raise ClientError(
            "session path is too long for a Unix-domain socket; "
            "use a shorter session directory", "not_sent")
    return short, Path(session).parent


def connect(session, timeout):
    if sys.platform == "win32" or not hasattr(socket, "AF_UNIX"):
        raise ClientError(
            "the diagnostic control socket needs AF_UNIX, which this "
            "platform's Python or host does not support (Windows)", "not_sent")
    target, working_directory = socket_target(session)
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(timeout)
    previous = Path.cwd()
    try:
        if working_directory is not None:
            os.chdir(working_directory)
        client.connect(target)
    except OSError as exc:
        client.close()
        raise ClientError(
            f"cannot connect to {Path(session) / 'control.sock'}: {exc}",
            "not_sent") from exc
    finally:
        if working_directory is not None:
            os.chdir(previous)
    return client


def read_reply(client, request_id):
    data = bytearray()
    try:
        while b"\n" not in data:
            block = client.recv(65536)
            if not block:
                raise ClientError(
                    "host disconnected before a complete reply; outcome is "
                    "unconfirmed", "unconfirmed")
            data.extend(block)
            if len(data) > MAX_REPLY_BYTES + 1:
                raise ClientError(
                    f"reply exceeds {MAX_REPLY_BYTES} bytes; outcome is "
                    "unconfirmed", "unconfirmed")
    except socket.timeout as exc:
        raise ClientError(
            "timed out waiting for the host reply; outcome is unconfirmed",
            "unconfirmed") from exc
    except OSError as exc:
        raise ClientError(
            f"failed reading the host reply: {exc}; outcome is unconfirmed",
            "unconfirmed") from exc
    line = bytes(data).split(b"\n", 1)[0]
    if len(line) > MAX_REPLY_BYTES:
        raise ClientError(f"reply exceeds {MAX_REPLY_BYTES} bytes", "unconfirmed")
    try:
        reply = json.loads(line)
    except ValueError as exc:
        raise ClientError(
            f"host reply is not JSON: {exc}; outcome is unconfirmed",
            "unconfirmed") from exc
    if not isinstance(reply, dict):
        raise ClientError("host reply is not a JSON object", "unconfirmed")
    if reply.get("schema") != 1:
        raise ClientError(
            f"unsupported protocol schema {reply.get('schema')!r}; "
            "outcome is unconfirmed", "unconfirmed")
    if reply.get("id") != request_id:
        raise ClientError(
            f"protocol identity mismatch: sent {request_id!r}, "
            f"received {reply.get('id')!r}", "unconfirmed")
    if not isinstance(reply.get("ok"), bool):
        raise ClientError("host reply has no boolean ok field", "unconfirmed")
    if not isinstance(reply.get("pid"), int):
        raise ClientError("host reply has no integer pid field", "unconfirmed")
    return reply


def audit(session, fields, reply):
    """Append a bounded request/reply record; never fail the operation."""
    path = Path(session) / "commands.ndjson"
    try:
        if path.exists() and path.stat().st_size >= MAX_AUDIT_BYTES:
            return
        with path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(
                {"request": fields, "reply": reply},
                separators=(",", ":")) + "\n")
    except OSError:
        pass


def request(session, verb, *args, timeout=30, request_id=None):
    """One transaction.  A lost reply is never retried automatically."""
    session = Path(session).expanduser().resolve()
    request_id = request_id or uuid.uuid4().hex
    fields = [request_id, verb, *map(str, args)]
    wire = encode_request(fields)
    identity = load_identity(session)
    client = connect(session, timeout)
    try:
        try:
            client.sendall(wire)
        except OSError as exc:
            raise ClientError(
                f"failed sending request: {exc}; outcome is unconfirmed",
                "unconfirmed") from exc
        reply = read_reply(client, request_id)
    finally:
        client.close()
    if identity is not None and reply.get("pid") != identity["pid"]:
        raise ClientError(
            f"session PID mismatch: {session / 'session.json'} records "
            f"{identity['pid']}, host reports {reply.get('pid')}; refusing a "
            "different host", "unconfirmed")
    audit(session, fields, reply)
    return reply


def stop_process_group(supervisor):
    """Terminate only the detached group created by start()."""
    pgid = supervisor.pid
    if pgid <= 1:
        return
    try:
        os.killpg(pgid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        pass
    try:
        supervisor.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass
    # The supervisor can exit while its host child is still in the group.
    try:
        os.killpg(pgid, 0)
    except (ProcessLookupError, PermissionError):
        return
    try:
        os.killpg(pgid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass
    try:
        supervisor.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass


def start(args, session):
    """Create a new session and launch an existing host, paused."""
    if sys.platform == "win32":
        raise ClientError(
            "diagnostic control sockets are not supported on Windows; "
            "the Windows host compiles the transport out", "startup_failed")
    session = Path(session).expanduser().resolve()
    if session.exists():
        raise ValueError(f"session directory already exists: {session}")
    if args.startup_timeout <= 0:
        raise ValueError("--startup-timeout must be positive")
    binary = (Path(args.binary).expanduser().resolve()
              if args.binary else DEFAULT_BINARY)
    if not binary.is_file():
        raise ValueError(
            f"host executable not found: {binary}; build m2host first "
            "(m2ctl never builds)")
    if not os.access(binary, os.X_OK):
        raise ValueError(f"host executable is not executable: {binary}")
    build_type = host_build_type(binary)
    if build_type is None:
        print(f"[m2ctl] no CMAKE_BUILD_TYPE beside {binary}; host optimization "
              "is unverified and timing claims are unconfirmed", file=sys.stderr)
    elif build_type not in OPTIMIZED_BUILD_TYPES and not args.allow_debug:
        raise ValueError(
            f"host build is '{build_type}'; diagnostic control expects an "
            "optimized host (configure with -DCMAKE_BUILD_TYPE=Release). "
            "Pass --allow-debug only for source-level stepping; an "
            "unoptimized host makes tick and pause budgets unrepresentative")
    game_data = None
    if args.game_data:
        game_data = Path(args.game_data).expanduser().resolve()
        if not game_data.is_dir():
            raise ValueError(f"--game-data is not a directory: {game_data}")
    if (args.width is None) != (args.height is None):
        raise ValueError("--width and --height must be given together")
    if args.width is not None:
        if not (320 <= args.width <= 7680 and 240 <= args.height <= 4320):
            raise ValueError(
                "--width and --height must be between 320x240 and 7680x4320")
    perf_file = None
    if args.perf_file:
        perf_file = (session / args.perf_file).expanduser().resolve()
        if not perf_file.is_relative_to(session) or perf_file == session:
            raise ValueError("--perf-file must remain inside the session directory")
    overrides = {}
    for spec in args.env:
        key, separator, value = spec.partition("=")
        if not separator or not key.startswith("M2_") or key == "M2_":
            raise ValueError("--env requires M2_NAME=value")
        overrides[key] = value
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("M2_")}
    environment.update(overrides)
    if args.backend != "software":
        environment["M2_GL_BACKEND"] = "cel" if args.backend == "cel" else "clean"
        environment["M2_GL_SWAP_INTERVAL"] = "0"

    working_directory = Path.cwd()
    socket_argument = os.path.relpath(session / "control.sock", working_directory)
    if len(os.fsencode(socket_argument)) > MAX_SOCKET_PATH_BYTES:
        working_directory = session.parent
        socket_argument = f"{session.name}/control.sock"
        if len(os.fsencode(socket_argument)) > MAX_SOCKET_PATH_BYTES:
            raise ValueError(
                "session path is too long for a Unix-domain socket; use a "
                "shorter --session directory")
    command = [str(binary), "--control-socket", socket_argument]
    if args.backend == "software":
        command.append("--headless")
    if game_data is not None:
        command.extend(("--game-data", str(game_data)))
    if args.width is not None:
        command.extend(("--width", str(args.width), "--height", str(args.height)))
    if perf_file is not None:
        command.extend(("--perf-file", str(perf_file)))

    session.mkdir(parents=True, exist_ok=False, mode=0o700)
    if perf_file is not None:
        perf_file.parent.mkdir(parents=True, exist_ok=True)
    launch = {
        "schema": 1,
        "session": str(session),
        "socket": str(session / "control.sock"),
        "command": command,
        "cwd": str(working_directory),
        "environment": overrides,
        "backend": args.backend,
        "binary": str(binary),
        "binary_sha256": digest(binary),
        "build_type": build_type,
        "game_data": str(game_data) if game_data is not None else None,
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    (session / "launch.json").write_text(
        json.dumps(launch, indent=2) + "\n", encoding="utf-8")
    supervisor = subprocess.Popen(
        [sys.executable or "python3", str(Path(__file__).resolve()),
         "--supervise", str(session)],
        cwd=working_directory, env=environment,
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, start_new_session=True)
    (session / "supervisor.pid").write_text(
        f"{supervisor.pid}\n", encoding="utf-8")
    deadline = time.monotonic() + args.startup_timeout
    try:
        while time.monotonic() < deadline:
            if supervisor.poll() is not None:
                raise ClientError(
                    f"supervisor exited during startup; see {session / 'host.log'}",
                    "startup_failed")
            exit_path = session / "exit.json"
            if exit_path.exists():
                try:
                    exit_record = json.loads(exit_path.read_text(encoding="utf-8"))
                except ValueError:
                    exit_record = {}
                raise ClientError(
                    "host exited during startup "
                    f"(exit {exit_record.get('exit_code')}); see "
                    f"{session / 'host.log'}", "startup_failed")
            if (session / "control.sock").exists():
                try:
                    reply = request(session, "status", timeout=2)
                except (OSError, RuntimeError, ValueError):
                    pass
                else:
                    state = reply.get("state")
                    if reply.get("ok") and isinstance(state, dict) and \
                            state.get("paused") is True:
                        identity = dict(launch)
                        identity["pid"] = reply["pid"]
                        # The supervisor also records this; writing it here
                        # makes the metadata available as soon as start returns.
                        (session / "host.pid").write_text(f"{reply['pid']}\n", encoding="utf-8")
                        (session / "session.json").write_text(
                            json.dumps(identity, indent=2) + "\n",
                            encoding="utf-8")
                        result = dict(reply)
                        result["session"] = str(session)
                        result["binary_sha256"] = launch["binary_sha256"]
                        result["build_type"] = build_type
                        return result
            time.sleep(0.05)
        raise ClientError(
            f"startup deadline exceeded; see {session / 'host.log'}. "
            "The launched process group was stopped", "startup_failed")
    except BaseException:
        stop_process_group(supervisor)
        raise


def supervise(directory):
    """Detached owner of the host process; drains and caps its output."""
    directory = Path(directory).resolve()
    try:
        launch = json.loads((directory / "launch.json").read_text(
            encoding="utf-8"))
        environment = os.environ.copy()
        environment.update(launch.get("environment") or {})
        with (directory / "host.log").open("xb", buffering=0) as log:
            child = subprocess.Popen(
                launch["command"], cwd=launch.get("cwd") or None,
                env=environment, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (directory / "host.pid").write_text(f"{child.pid}\n", encoding="utf-8")
            written = 0
            capped = False
            for block in iter(lambda: child.stdout.read(8192), b""):
                remaining = MAX_LOG_BYTES - written
                if remaining > 0:
                    chunk = block[:remaining]
                    log.write(chunk)
                    written += len(chunk)
                if written >= MAX_LOG_BYTES and not capped:
                    log.write(b"\n[control supervisor] log cap reached; "
                              b"further output drained\n")
                    capped = True
            child.stdout.close()
            status = child.wait()
            record = {"pid": child.pid, "exit_code": status}
            if status < 0:
                record["signal"] = -status
            (directory / "exit.json").write_text(
                json.dumps(record) + "\n", encoding="utf-8")
            return 0 if status == 0 else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        try:
            (directory / "exit.json").write_text(
                json.dumps({"pid": None, "exit_code": None,
                            "error": str(exc)}) + "\n",
                encoding="utf-8")
        except OSError:
            pass
        return 1


def command_request(args, session):
    """Map a parsed command to (verb, arguments, socket timeout seconds)."""
    command = args.command
    if command in ("status", "pause", "quit", "graphics"):
        return command, [], 30
    if command in ("tap", "hold", "release"):
        if args.timeout <= 0:
            raise ValueError("--timeout must be positive")
        if args.max_ticks <= 0:
            raise ValueError("--max-ticks must be positive")
        name = "none" if command == "release" else args.button.lower()
        if name in BUTTONS:
            button = BUTTONS[name]
        else:
            try:
                button = int(name, 0)
            except ValueError as exc:
                raise ValueError(
                    f"unknown button {args.button!r}; use a name from "
                    f"{', '.join(sorted(BUTTONS))} or a numeric mask") from exc
        x, y = AXES.get(name, (128, 128))
        if args.x is not None:
            x = args.x
        if args.y is not None:
            y = args.y
        milliseconds = int(args.timeout * 1000)
        if milliseconds <= 0:
            raise ValueError("--timeout is below one millisecond")
        return "input", [button, x, y, int(command == "tap"),
                         args.max_ticks, milliseconds], args.timeout
    if command == "advance":
        if args.ticks <= 0:
            raise ValueError("advance needs a positive tick count")
        if args.timeout <= 0:
            raise ValueError("--timeout must be positive")
        milliseconds = int(args.timeout * 1000)
        if milliseconds <= 0:
            raise ValueError("--timeout is below one millisecond")
        return "advance", [args.ticks, milliseconds], args.timeout
    if command == "wait":
        if args.timeout <= 0:
            raise ValueError("--timeout must be positive")
        if args.max_ticks <= 0:
            raise ValueError("--max-ticks must be positive")
        milliseconds = int(args.timeout * 1000)
        if milliseconds <= 0:
            raise ValueError("--timeout is below one millisecond")
        return ("wait", [args.field, args.comparison, args.value,
                         args.max_ticks, milliseconds], args.timeout)
    if command in ("read", "watch"):
        if args.ea < 0:
            raise ValueError("guest effective address must not be negative")
        if args.size <= 0:
            raise ValueError("size must be positive")
        if command == "read":
            return "read", [args.ea, args.size], 30
        return "watch", [args.ea, args.size, args.label], 30
    if command in ("capture", "provenance", "trace"):
        output = (session / args.output).expanduser().resolve()
        if output == session or not output.is_relative_to(session):
            raise ValueError("output paths must remain inside the session directory")
        output.parent.mkdir(parents=True, exist_ok=True)
        if command == "trace":
            return "trace", [str(output), args.kinds], 30
        return command, [str(output)], 30
    raise ValueError(f"unknown command {command!r}")


def parser():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=EPILOG)
    ap.add_argument("--session", required=True, metavar="DIR",
                    help="session directory containing control.sock and metadata")
    ap.add_argument("--request-id",
                    help="stable id for reconciling a lost reply; never reuse "
                         "with different arguments")
    sub = ap.add_subparsers(dest="command", required=True, metavar="COMMAND")

    p = sub.add_parser("start", help="launch an already-built host, paused")
    p.add_argument("--binary", metavar="PATH",
                   help=f"host executable (default: {DEFAULT_BINARY})")
    p.add_argument("--backend", choices=("software", "clean", "cel"),
                   default="software",
                   help="software=headless capture backend (default); "
                        "clean/cel=windowed OpenGL backend")
    p.add_argument("--game-data", metavar="DIR",
                   help="folder containing driving/game; passed to the host")
    p.add_argument("--allow-debug", action="store_true",
                   help="run an unoptimized host anyway; stepping only, "
                        "timing claims are invalid")
    p.add_argument("--env", action="append", default=[], metavar="M2_NAME=VALUE",
                   help="explicit diagnostic environment override; inherited "
                        "M2_* variables are cleared")
    p.add_argument("--perf-file", metavar="PATH",
                   help="host performance report inside this session")
    p.add_argument("--width", type=int, metavar="N",
                   help="window/render width for a named graphics run")
    p.add_argument("--height", type=int, metavar="N",
                   help="window/render height for a named graphics run")
    p.add_argument("--startup-timeout", type=float, default=30,
                   metavar="SECONDS",
                   help="wall-clock limit for the paused status handshake "
                        "(default: 30)")

    for command in ("status", "pause", "quit", "graphics"):
        sub.add_parser(command)
    for command in ("tap", "hold", "release"):
        p = sub.add_parser(command)
        if command != "release":
            p.add_argument("button",
                           help="a/b/c/start/l/r/x/lookback/up/down/left/right/"
                                "none, or a numeric ControlPort mask")
        p.add_argument("--x", type=int, choices=range(256), metavar="0..255")
        p.add_argument("--y", type=int, choices=range(256), metavar="0..255")
        p.add_argument("--timeout", type=float, default=30, metavar="SECONDS",
                       help="wall-clock watchdog (default: 30, maximum: 120)")
        p.add_argument("--max-ticks", type=int, default=1800, metavar="N",
                       help="bounded platform work (default: 1800, maximum: 12000)")
    p = sub.add_parser(
        "advance",
        help="advance platform ticks; each tick services video, input and "
             "735 audio sample frames")
    p.add_argument("ticks", type=int)
    p.add_argument("--timeout", type=float, default=30, metavar="SECONDS",
                   help="wall-clock watchdog (default: 30, maximum: 120)")
    p = sub.add_parser("wait",
                       help="advance until a state or memory predicate holds")
    p.add_argument("field",
                   help="primary, secondary, guest_frame, completed_frames, "
                        "audio_frames, ticks, or u8:EA/u16:EA/u32:EA")
    p.add_argument("comparison", choices=("eq", "ne", "ge"))
    p.add_argument("value", type=lambda value: int(value, 0))
    p.add_argument("--timeout", type=float, default=120, metavar="SECONDS",
                   help="wall-clock watchdog (default: 120, maximum: 120)")
    p.add_argument("--max-ticks", type=int, default=1800, metavar="N",
                   help="bounded platform work (default: 1800, maximum: 12000)")
    p = sub.add_parser("read", help="read a bounded guest-memory range")
    p.add_argument("ea", type=lambda value: int(value, 0))
    p.add_argument("size", type=lambda value: int(value, 0))
    p = sub.add_parser("watch",
                       help="enable bounded write/copy provenance at a location")
    p.add_argument("ea", type=lambda value: int(value, 0))
    p.add_argument("size", type=lambda value: int(value, 0))
    p.add_argument("--label", default="diagnostic")
    for command in ("capture", "provenance", "trace"):
        p = sub.add_parser(command)
        p.add_argument("output",
                       help="new output path relative to the session directory")
        if command == "trace":
            p.add_argument("--kinds", required=True,
                           help="comma-separated trace event kinds: input, "
                                "guest_enter, guest_exit, state_transition, "
                                "selector_call, selector_return, asset_load, "
                                "graphics_request, audio_request, memory_fault, "
                                "indirect_target, unknown_behavior, "
                                "frame_complete, tracepoint, provenance, "
                                "shutdown")
    return ap


def main(argv=None):
    ap = parser()
    args = ap.parse_args(argv)
    session = Path(args.session).expanduser().resolve()
    request_id = args.request_id or uuid.uuid4().hex
    try:
        if args.command == "start":
            reply = start(args, session)
        else:
            if not session.is_dir():
                raise ValueError(f"session directory does not exist: {session}")
            verb, params, timeout = command_request(args, session)
            reply = request(session, verb, *params, timeout=timeout + 10,
                            request_id=request_id)
        print(json.dumps(reply, indent=2))
        return 0 if reply["ok"] else 1
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        outcome = getattr(exc, "outcome",
                          "startup_failed" if args.command == "start"
                          else "not_sent")
        print(json.dumps({"ok": False, "error": str(exc),
                          "session": str(session),
                          "request_id": request_id,
                          "outcome": outcome}))
        return 2


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--supervise":
        raise SystemExit(supervise(Path(sys.argv[2])))
    raise SystemExit(main())
