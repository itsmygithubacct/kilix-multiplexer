#!/usr/bin/env python3
"""Translate Kilix terminal input into XTest events for a pixel pane.

kmx-serve starts this helper with DISPLAY set to the private Xvfb used by
--pixel-pane.  stdin is the authenticated control client's raw input stream:
kitty CSI-u keyboard events, bracketed paste, and SGR pixel mouse events.

The helper also keeps the largest top-level X window fitted to the framebuffer.
That matters on a display without a window manager: applications are otherwise
free to map at a default size, while the captured root window is much larger.
"""

import argparse
import os
import select
import sys
import time


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("width", type=int)
    parser.add_argument("height", type=int)
    parser.add_argument(
        "--kilix-home",
        default=os.environ.get("KILIX_HOME", "kilix"),
    )
    return parser.parse_args()


def largest_top_level(xd):
    from Xlib import X

    candidates = []
    for window in xd.screen().root.query_tree().children:
        try:
            attributes = window.get_attributes()
            geometry = window.get_geometry()
        except Exception:
            continue
        if attributes.map_state != X.IsViewable:
            continue
        candidates.append((geometry.width * geometry.height, window))
    return max(candidates, default=(0, None), key=lambda item: item[0])[1]


def fit_window(xd, window, width, height, focus=False):
    from Xlib import X

    if window is None:
        return False
    try:
        geometry = window.get_geometry()
        if (
            geometry.x != 0
            or geometry.y != 0
            or geometry.width != width
            or geometry.height != height
            or geometry.border_width != 0
        ):
            window.configure(
                x=0,
                y=0,
                width=width,
                height=height,
                border_width=0,
                stack_mode=X.Above,
            )
        if focus:
            window.set_input_focus(X.RevertToParent, X.CurrentTime)
        xd.sync()
        return True
    except Exception:
        return False


def write_pipe(fd, data):
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        view = view[written:]


def main():
    args = parse_args()
    if args.width < 1 or args.height < 1:
        return 2

    config_dir = os.path.join(os.path.abspath(args.kilix_home), "config")
    sys.path.insert(0, config_dir)
    try:
        from Xlib import display
        from apprun import RunTerm
        from xinject import Injector
    except (ImportError, ModuleNotFoundError) as error:
        print(f"kmx pixel input: {error}", file=sys.stderr)
        return 1

    xd = display.Display(os.environ.get("DISPLAY"))
    injector = Injector(xd, args.width, args.height)

    # RunTerm's parser reads from a descriptor.  A small non-blocking pipe lets
    # this helper feed each network chunk through the exact parser used by
    # `kilix run`, including sequences split across chunks.
    read_fd, write_fd = os.pipe()
    os.set_blocking(read_fd, False)
    term = RunTerm.__new__(RunTerm)
    term.fd = read_fd
    term.inbuf = b""

    target = None
    focused = False
    refit_until = time.monotonic() + 15.0
    stdin_fd = sys.stdin.fileno()
    try:
        while True:
            now = time.monotonic()
            if target is None or now < refit_until:
                candidate = largest_top_level(xd)
                if candidate is not None:
                    target = candidate
                    focused = fit_window(
                        xd, target, args.width, args.height, focus=not focused
                    ) or focused

            readable, _, _ = select.select([stdin_fd], [], [], 0.1)
            if not readable:
                continue
            chunk = os.read(stdin_fd, 4096)
            if not chunk:
                break
            write_pipe(write_fd, chunk)
            for event in term.read_input():
                kind = event.get("kind")
                if kind == "key":
                    event_type = event.get("event", 1)
                    if event_type != 2:
                        injector.key(event["key"], event_type)
                elif kind == "paste":
                    injector.paste(event.get("text", ""))
                elif kind == "mouse":
                    injector.mouse(event, (0, 0, args.width, args.height))
    finally:
        injector.release_all()
        os.close(write_fd)
        os.close(read_fd)
        try:
            xd.close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
