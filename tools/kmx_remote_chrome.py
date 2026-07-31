#!/usr/bin/env python3
"""Own one Chrome pixel-pane session on an SSH host.

This process is intentionally the remote SSH command.  It verifies that no
other Chrome is running, starts kmx-serve with a private profile/display/input
helper, and tears down that exact session when the SSH channel goes away.
"""

import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--token", required=True)
    parser.add_argument("--width", required=True, type=int)
    parser.add_argument("--height", required=True, type=int)
    parser.add_argument("--fps", type=int, default=10)
    parser.add_argument("--url", default="about:blank")
    configured_home = os.environ.get("KILIX_HOME")
    parser.add_argument(
        "--kilix-home",
        default=configured_home,
        required=not configured_home,
        help="Kilix source root (or set KILIX_HOME)",
    )
    parser.add_argument("--user-data-dir")
    parser.add_argument(
        "--profile-number",
        type=int,
        help="0 selects Default; N selects the directory 'Profile N'",
    )
    return parser.parse_args()


def process_is_live(pid):
    try:
        state = next(
            line.split()[1]
            for line in Path(f"/proc/{pid}/status").read_text().splitlines()
            if line.startswith("State:")
        )
    except (FileNotFoundError, PermissionError, ProcessLookupError, StopIteration):
        return False
    return state not in {"X", "Z"}


def processes_matching(predicate):
    matches = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            comm = (entry / "comm").read_text().strip()
            state = next(
                line.split()[1]
                for line in (entry / "status").read_text().splitlines()
                if line.startswith("State:")
            )
            argv = [
                value.decode("utf-8", "replace")
                for value in (entry / "cmdline").read_bytes().split(b"\0")
                if value
            ]
        except (
            FileNotFoundError,
            PermissionError,
            ProcessLookupError,
            StopIteration,
        ):
            continue
        if state == "Z":
            continue
        if predicate(comm, argv):
            matches.append(int(entry.name))
    return matches


def terminate_pids(pids, timeout=4.0):
    pids = {pid for pid in pids if pid != os.getpid()}
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            pass
    deadline = time.monotonic() + timeout
    while pids and time.monotonic() < deadline:
        pids = {pid for pid in pids if process_is_live(pid)}
        if pids:
            time.sleep(0.1)
    for pid in pids:
        try:
            os.kill(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
    deadline = time.monotonic() + 1.0
    while pids and time.monotonic() < deadline:
        pids = {pid for pid in pids if process_is_live(pid)}
        if pids:
            time.sleep(0.02)
    return pids


def resolve_profile_configuration(args):
    ephemeral_profile = args.user_data_dir is None
    if ephemeral_profile:
        user_data_dir = Path(f"/tmp/kmx-remote-chrome-{args.token}")
        if user_data_dir.is_symlink() or (
            user_data_dir.exists() and not user_data_dir.is_dir()
        ):
            raise SystemExit(
                f"remote Chrome: unsafe temporary profile path: {user_data_dir}"
            )
    else:
        user_data_dir = Path(args.user_data_dir).expanduser().resolve()
        if not user_data_dir.is_dir():
            raise SystemExit(
                f"remote Chrome: user-data directory does not exist: {user_data_dir}"
            )

    profile_directory = None
    if args.profile_number is not None:
        if args.profile_number < 0:
            raise SystemExit("remote Chrome: profile number cannot be negative")
        profile_directory = (
            "Default" if args.profile_number == 0
            else f"Profile {args.profile_number}"
        )
        selected_profile = user_data_dir / profile_directory
        if not selected_profile.is_dir():
            raise SystemExit(
                f"remote Chrome: profile does not exist: {selected_profile}"
            )
    return ephemeral_profile, user_data_dir, profile_directory


def resolve_kilix_home(value):
    kilix_home = Path(value).expanduser().resolve()
    required_modules = (
        kilix_home / "config" / "apprun.py",
        kilix_home / "config" / "xinject.py",
    )
    if not kilix_home.is_dir() or not all(path.is_file() for path in required_modules):
        raise SystemExit(
            "remote Chrome: Kilix source root is missing required input modules: "
            f"{kilix_home}"
        )
    return kilix_home


def validate_arguments(args):
    if not (1024 <= args.port <= 65535):
        raise SystemExit("remote Chrome: invalid port")
    if not re.fullmatch(r"[0-9a-fA-F]{16,32}", args.token):
        raise SystemExit("remote Chrome: token must be 16-32 hexadecimal characters")
    if args.width < 320 or args.height < 200:
        raise SystemExit("remote Chrome: framebuffer is too small")
    if not (1 <= args.fps <= 60):
        raise SystemExit("remote Chrome: fps must be between 1 and 60")


def profile_state_snapshot(local_state):
    if not local_state.is_file():
        return None
    with local_state.open(encoding="utf-8") as stream:
        document = json.load(stream)
    profile = document.get("profile", {})
    missing = object()
    snapshot = {}
    for key in ("last_used", "last_active_profiles"):
        value = profile.get(key, missing)
        snapshot[key] = (value is not missing, value if value is not missing else None)
    return snapshot


def restore_profile_state(local_state, snapshot):
    if snapshot is None or not local_state.is_file():
        return
    with local_state.open(encoding="utf-8") as stream:
        document = json.load(stream)
    profile = document.setdefault("profile", {})
    changed = False
    for key, (existed, value) in snapshot.items():
        if existed:
            if profile.get(key) != value:
                profile[key] = value
                changed = True
        elif key in profile:
            del profile[key]
            changed = True
    if not changed:
        return
    mode = local_state.stat().st_mode & 0o777
    temporary = local_state.with_name(
        f".{local_state.name}.kmx-{os.getpid()}.tmp"
    )
    try:
        with temporary.open("x", encoding="utf-8") as stream:
            json.dump(document, stream, ensure_ascii=False, separators=(",", ":"))
        os.chmod(temporary, mode)
        os.replace(temporary, local_state)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def main():
    args = parse_args()
    validate_arguments(args)

    ephemeral_profile, user_data_dir, profile_directory = (
        resolve_profile_configuration(args)
    )
    kilix_home = resolve_kilix_home(args.kilix_home)

    source = Path(__file__).resolve().parent.parent
    serve = source / "build" / "kmx-serve"
    input_helper = source / "tools" / "kmx_pixel_input.py"
    chrome = shutil.which("google-chrome")
    if not serve.is_file() or not os.access(serve, os.X_OK):
        raise SystemExit(f"remote Chrome: missing executable {serve}")
    if not input_helper.is_file():
        raise SystemExit(f"remote Chrome: missing helper {input_helper}")
    if not chrome:
        raise SystemExit("remote Chrome: google-chrome is not installed")

    # Validate every path and profile choice before reaching this destructive
    # boundary. Chrome's process tree
    # uses `chrome` for the browser and renderer processes, so clear all of it
    # and prove it reached zero before creating the private profile.
    existing = processes_matching(lambda comm, _argv: comm == "chrome")
    print(f"remote Chrome: pre-existing chrome processes={len(existing)}", flush=True)
    unresolved = terminate_pids(existing)
    remaining = processes_matching(lambda comm, _argv: comm == "chrome")
    remaining = sorted(set(remaining) | set(unresolved))
    if remaining:
        raise SystemExit(
            "remote Chrome: could not stop pre-existing processes: "
            + ",".join(str(pid) for pid in remaining)
        )
    print("remote Chrome: chrome processes after cleanup=0", flush=True)

    if ephemeral_profile:
        if user_data_dir.exists():
            shutil.rmtree(user_data_dir)

    local_state = user_data_dir / "Local State"
    saved_profile_state = (
        profile_state_snapshot(local_state) if not ephemeral_profile else None
    )

    chrome_argv = [
        chrome,
        f"--user-data-dir={user_data_dir}",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-background-networking",
        "--disable-component-update",
        "--disable-dev-shm-usage",
        "--disable-features=Translate",
        "--window-position=0,0",
        f"--window-size={args.width},{args.height}",
    ]
    if profile_directory:
        chrome_argv.append(f"--profile-directory={profile_directory}")
    chrome_argv.append(args.url)
    chrome_command = "exec " + shlex.join(chrome_argv)
    input_command = shlex.join(
        [
            sys.executable,
            str(input_helper),
            str(args.width),
            str(args.height),
            "--kilix-home",
            str(kilix_home),
        ]
    )
    command = [
        str(serve),
        "--socket",
        f"127.0.0.1:{args.port}",
        "--no-tls",
        "--token",
        args.token,
        "--pixel-pane",
        chrome_command,
        "--pixel-size",
        f"{args.width}x{args.height}",
        "--pixel-fps",
        str(args.fps),
        "--input-command",
        input_command,
    ]

    stopping = False

    def request_stop(_signal, _frame):
        nonlocal stopping
        stopping = True

    for signal_number in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(signal_number, request_stop)

    # sshd normally signals its command when the channel closes, but a killed
    # local ssh client can instead leave that command reparented and alive.
    # Linux's parent-death signal closes that lifecycle gap; the explicit
    # parent check covers the small race between reading the parent and prctl.
    parent_pid = os.getppid()
    try:
        ctypes.CDLL(None).prctl(1, signal.SIGTERM)
    except (AttributeError, OSError):
        pass
    if os.getppid() != parent_pid:
        stopping = True

    server = subprocess.Popen(command)
    try:
        while server.poll() is None and not stopping:
            if os.getppid() != parent_pid:
                stopping = True
                continue
            time.sleep(0.1)
    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=8)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
        profile_argument = f"--user-data-dir={user_data_dir}"
        owned = processes_matching(
            lambda comm, argv: comm == "chrome" and profile_argument in argv
        )
        unresolved = terminate_pids(owned)
        remaining = processes_matching(
            lambda comm, argv: comm == "chrome" and profile_argument in argv
        )
        remaining = sorted(set(remaining) | set(unresolved))
        if remaining:
            raise SystemExit(
                "remote Chrome: could not stop session processes: "
                + ",".join(str(pid) for pid in remaining)
            )
        if ephemeral_profile and user_data_dir.exists():
            shutil.rmtree(user_data_dir)
        elif not ephemeral_profile:
            restore_profile_state(local_state, saved_profile_state)
    return server.returncode if server.returncode and not stopping else 0


if __name__ == "__main__":
    raise SystemExit(main())
