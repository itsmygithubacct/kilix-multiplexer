#!/usr/bin/env python3
"""Checks for pixel-input dispatch and explicit Kilix discovery."""

import importlib.util
import os
from pathlib import Path
import sys
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parent.parent / "tools" / "kmx_pixel_input.py"
SPEC = importlib.util.spec_from_file_location("kmx_pixel_input", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class Injector:
    def __init__(self):
        self.calls = []

    def key(self, key, event_type):
        self.calls.append(("key", key, event_type))

    def paste(self, text):
        self.calls.append(("paste", text))

    def mouse(self, event, box):
        self.calls.append(("mouse", event, box))


injector = Injector()
MODULE.dispatch_event(injector, {"kind": "key", "key": "a", "event": 1}, 640, 480)
MODULE.dispatch_event(injector, {"kind": "key", "key": "a", "event": 2}, 640, 480)
MODULE.dispatch_event(injector, {"kind": "key", "key": "a", "event": 3}, 640, 480)
MODULE.dispatch_event(injector, {"kind": "paste", "text": "hello"}, 640, 480)
mouse = {"kind": "mouse", "x": 12, "y": 34, "b": 0, "press": True}
MODULE.dispatch_event(injector, mouse, 640, 480)
assert injector.calls == [
    ("key", "a", 1),
    ("key", "a", 3),
    ("paste", "hello"),
    ("mouse", mouse, (0, 0, 640, 480)),
]

environment = dict(os.environ)
environment.pop("KILIX_HOME", None)
with (
    mock.patch.dict(os.environ, environment, clear=True),
    mock.patch.object(sys, "argv", ["kmx-pixel-input", "640", "480"]),
    mock.patch("sys.stderr"),
):
    try:
        MODULE.parse_args()
    except SystemExit as error:
        assert error.code == 2
    else:
        raise AssertionError("missing Kilix source root was accepted")

with (
    mock.patch.dict(os.environ, {"KILIX_HOME": "portable-kilix"}, clear=False),
    mock.patch.object(sys, "argv", ["kmx-pixel-input", "640", "480"]),
):
    args = MODULE.parse_args()
    assert args.kilix_home == "portable-kilix"

print("all pixel input helper checks passed")
