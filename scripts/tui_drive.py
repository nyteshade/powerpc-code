#!/usr/bin/env python3
"""Drive the ppcode TUI inside a pty and dump what the screen looks like.

The TUI cannot be tested through a plain pipe -- ncurses needs a terminal, and
ppcode refuses to start interactively without one. This spawns it on a real
pty, feeds a scripted sequence of keystrokes, and prints the raw output so a
human (or an agent) can check the rendering.

  ./tui_drive.py ./build/ppcode -- '/help\\r' 2.0 '\\x04'

Arguments after -- alternate between things to send and seconds to wait. A
bare number is a delay; anything else is sent as keystrokes (with \\x and \\r
escapes interpreted).
"""

import os
import pty
import re
import select
import sys
import time

DEFAULT_ENV = {
    "TERM": "xterm-color",
    "LINES": "40",
    "COLUMNS": "100",
}


def drive(argv, steps, settle=1.5):
    env = dict(os.environ)
    env.update(DEFAULT_ENV)

    pid, fd = pty.fork()
    if pid == 0:
        os.execvpe(argv[0], argv, env)
        os._exit(127)

    # Match the window size we advertised, or ncurses assumes 24x80.
    try:
        import fcntl
        import struct
        import termios
        fcntl.ioctl(fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", int(DEFAULT_ENV["LINES"]),
                                int(DEFAULT_ENV["COLUMNS"]), 0, 0))
    except Exception:
        pass

    out = []

    def pump(seconds):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.1)
            if fd in r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    return False
                if not data:
                    return False
                out.append(data)
        return True

    # Snapshot after every step. The exit sequence ends with a screen clear,
    # so rendering only the final buffer would show a blank terminal -- the
    # interesting states are the intermediate ones.
    snapshots = []

    pump(settle)
    snapshots.append(("startup", b"".join(out)))

    for step in steps:
        try:
            delay = float(step)
            pump(delay)
            snapshots.append((f"after {delay}s", b"".join(out)))
            continue
        except ValueError:
            pass
        keys = step.encode().decode("unicode_escape").encode("latin-1")
        os.write(fd, keys)
        pump(0.6)
        snapshots.append((f"sent {step!r}", b"".join(out)))

    pump(settle)
    snapshots.append(("final", b"".join(out)))

    try:
        os.close(fd)
    except OSError:
        pass
    try:
        _, status = os.waitpid(pid, os.WNOHANG)
    except ChildProcessError:
        status = 0
    return snapshots, status


def render(raw):
    """Collapse a stream of terminal output into the final visible screen.

    This is a deliberately small ANSI interpreter: enough to place text via
    cursor-positioning and erase lines, which is all ncurses uses here.
    """
    rows, cols = int(DEFAULT_ENV["LINES"]), int(DEFAULT_ENV["COLUMNS"])
    grid = [[" "] * cols for _ in range(rows)]
    cy = cx = 0
    i = 0
    text = raw.decode("utf-8", "replace")
    while i < len(text):
        c = text[i]
        if c == "\x1b":
            m = re.match(r"\x1b\[([0-9;?]*)([A-Za-z])", text[i:])
            if not m:
                # Two-character sequences (\x1b7 save, \x1b8 restore, \x1b=,
                # \x1b>) and charset selects (\x1b)0) -- skip, don't print.
                nxt = text[i + 1] if i + 1 < len(text) else ""
                i += 3 if nxt in "()#" else 2
                continue
            params, cmd = m.group(1), m.group(2)
            nums = [int(p) for p in params.split(";") if p.isdigit()]
            if cmd == "H":
                cy = (nums[0] - 1) if len(nums) > 0 else 0
                cx = (nums[1] - 1) if len(nums) > 1 else 0
            elif cmd == "K":
                for x in range(cx, cols):
                    grid[cy][x] = " "
            elif cmd == "J":
                for y in range(rows):
                    for x in range(cols):
                        grid[y][x] = " "
                cy = cx = 0
            elif cmd in "ABCD":
                n = nums[0] if nums else 1
                if cmd == "A": cy -= n
                elif cmd == "B": cy += n
                elif cmd == "C": cx += n
                elif cmd == "D": cx -= n
            i += m.end()
            continue
        if c == "\r":
            cx = 0
        elif c == "\n":
            cy += 1
            cx = 0
        elif c == "\b":
            cx = max(0, cx - 1)
        elif c >= " ":
            if 0 <= cy < rows and 0 <= cx < cols:
                grid[cy][cx] = c
            cx += 1
            if cx >= cols:
                cx = 0
                cy += 1
        cy = max(0, min(cy, rows - 1))
        cx = max(0, min(cx, cols - 1))
        i += 1
    return "\n".join("".join(row).rstrip() for row in grid)


def main():
    if "--" not in sys.argv:
        print(__doc__)
        return 2
    split = sys.argv.index("--")
    argv = sys.argv[1:split]
    steps = sys.argv[split + 1:]
    only_last = "--last" in argv
    argv = [a for a in argv if a != "--last"]

    snapshots, _ = drive(argv, steps)
    if only_last:
        snapshots = snapshots[-2:-1] or snapshots[-1:]
    for label, raw in snapshots:
        print("=" * 100)
        print(f"--- {label} ---")
        print(render(raw))
    print("=" * 100)
    return 0


if __name__ == "__main__":
    sys.exit(main())
