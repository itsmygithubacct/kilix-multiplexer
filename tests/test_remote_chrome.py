#!/usr/bin/env python3
"""Unit checks for the remote Chrome supervisor's destructive boundaries."""

import argparse
import importlib.util
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parent.parent / "tools" / "kmx_remote_chrome.py"
SPEC = importlib.util.spec_from_file_location("kmx_remote_chrome", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def arguments(**changes):
    values = {
        "port": 47800,
        "token": "a" * 32,
        "width": 1280,
        "height": 720,
        "fps": 10,
        "url": "about:blank",
        "kilix_home": "kilix-source",
        "user_data_dir": None,
        "profile_number": None,
    }
    values.update(changes)
    return argparse.Namespace(**values)


MODULE.validate_arguments(arguments())
for bad_token in ("short", "z" * 32, "a/../../unsafe-path"):
    try:
        MODULE.validate_arguments(arguments(token=bad_token))
    except SystemExit:
        pass
    else:
        raise AssertionError(f"unsafe token was accepted: {bad_token!r}")


with tempfile.TemporaryDirectory() as directory:
    data = Path(directory)
    kilix_home = data / "kilix-source"
    (kilix_home / "config").mkdir(parents=True)
    for module in ("apprun.py", "xinject.py"):
        (kilix_home / "config" / module).write_text("", encoding="utf-8")
    assert MODULE.resolve_kilix_home(kilix_home) == kilix_home.resolve()

    (data / "Profile 2").mkdir()
    ephemeral, selected, profile = MODULE.resolve_profile_configuration(
        arguments(user_data_dir=str(data), profile_number=2)
    )
    assert not ephemeral
    assert selected == data.resolve()
    assert profile == "Profile 2"

    bad = arguments(user_data_dir=str(data), profile_number=3)
    with (
        mock.patch.object(MODULE, "parse_args", return_value=bad),
        mock.patch.object(
            MODULE,
            "processes_matching",
            side_effect=AssertionError("Chrome cleanup ran before profile validation"),
        ),
    ):
        try:
            MODULE.main()
        except SystemExit as error:
            assert "profile does not exist" in str(error)
        else:
            raise AssertionError("missing profile was accepted")

    bad = arguments(user_data_dir=str(data), kilix_home=str(data / "missing"))
    with (
        mock.patch.object(MODULE, "parse_args", return_value=bad),
        mock.patch.object(
            MODULE,
            "processes_matching",
            side_effect=AssertionError("Chrome cleanup ran before Kilix validation"),
        ),
    ):
        try:
            MODULE.main()
        except SystemExit as error:
            assert "missing required input modules" in str(error)
        else:
            raise AssertionError("invalid Kilix source root was accepted")

    state = data / "Local State"
    state.write_text(
        json.dumps(
            {
                "profile": {
                    "last_used": "Default",
                    "last_active_profiles": ["Default"],
                },
                "unrelated": {"keep": True},
            }
        ),
        encoding="utf-8",
    )
    snapshot = MODULE.profile_state_snapshot(state)
    document = json.loads(state.read_text(encoding="utf-8"))
    document["profile"]["last_used"] = "Profile 2"
    document["profile"]["last_active_profiles"] = ["Profile 2"]
    document["unrelated"]["new"] = 1
    state.write_text(json.dumps(document), encoding="utf-8")
    MODULE.restore_profile_state(state, snapshot)
    restored = json.loads(state.read_text(encoding="utf-8"))
    assert restored["profile"]["last_used"] == "Default"
    assert restored["profile"]["last_active_profiles"] == ["Default"]
    assert restored["unrelated"] == {"keep": True, "new": 1}


child = subprocess.Popen(
    [
        sys.executable,
        "-c",
        "import signal,sys,time; "
        "signal.signal(signal.SIGTERM, lambda *_: None); "
        "print('ready', flush=True); time.sleep(30)",
    ],
    stdout=subprocess.PIPE,
    text=True,
)
try:
    assert child.stdout.readline().strip() == "ready"
    remaining = MODULE.terminate_pids({child.pid}, timeout=0.05)
    assert not remaining, f"process still live after forced cleanup: {remaining}"
    child.wait(timeout=2)
    assert child.returncode == -signal.SIGKILL
finally:
    if child.poll() is None:
        child.kill()
        child.wait()

print("all remote Chrome supervisor checks passed")
