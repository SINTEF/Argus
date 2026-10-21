#!/usr/bin/env python3
"""Display serial CSV fields in place on a Linux terminal (standard library only)."""

import argparse
from contextlib import contextmanager
import curses
import errno
import os
from pathlib import Path
import select
import sys
import termios
import time
import tty

DEFAULT_DEVICE = "/dev/serial/by-id/usb-FTDI_TTL232R-3V3_FTENBB2G-if00-port0"
# Fallback when run from a checkout without an installed config.
REPO_CONFIG = Path(__file__).resolve().parent.parent / "config" / "sensor-fields.conf"


class DeviceClosed(Exception):
    """The serial device went away: end of an emulated stream, or unplugged."""


def default_config():
    # Same selection as SensorPayload: ARGUS_SENSOR_FIELDS, else the data directory.
    if os.environ.get("ARGUS_SENSOR_FIELDS"):
        return os.environ["ARGUS_SENSOR_FIELDS"]
    installed = Path(os.environ.get("ARGUS_DATA_DIR") or "/etc/argus") / "sensor-fields.conf"
    return str(installed if installed.exists() or not REPO_CONFIG.exists() else REPO_CONFIG)


def load_fields(path):
    fields = {}
    names = set()
    for number, line in enumerate(Path(path).read_text().splitlines(), 1):
        parts = line.split("#", 1)[0].split()
        if not parts:
            continue
        if (len(parts) != 3 or not parts[0].isascii() or not parts[0].isdigit()
                or parts[2] not in {"string", "int", "int64", "double"}):
            raise ValueError(f"{path}:{number}: expected: column name type")
        index, name, kind = int(parts[0]), parts[1], parts[2]
        if index in fields or name in names:
            raise ValueError(f"{path}:{number}: duplicate column or name")
        fields[index] = (name, kind)
        names.add(name)
    return fields


@contextmanager
def serial_port(device, baud):
    speed = getattr(termios, f"B{baud}", None)
    if speed is None:
        raise ValueError(f"Unsupported baud rate: {baud}")
    fd = os.open(device, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    original = None
    try:
        original = termios.tcgetattr(fd)
        tty.setraw(fd, termios.TCSANOW)
        attrs = termios.tcgetattr(fd)
        attrs[4] = attrs[5] = speed
        attrs[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE
                      | getattr(termios, "CRTSCTS", 0))
        attrs[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        # Start from current data, not rows that queued up before we opened.
        termios.tcflush(fd, termios.TCIFLUSH)
        yield fd
    finally:
        try:
            if original is not None:
                termios.tcsetattr(fd, termios.TCSANOW, original)
        except termios.error:
            pass  # The device is gone (closed pty, unplugged); nothing to restore.
        finally:
            os.close(fd)


class LineReader:
    """Keep fragmented rows until newline; bound memory for malformed streams."""

    def __init__(self):
        self.pending = bytearray()
        self.discarding = False
        self.dropped = 0

    def feed(self, chunk):
        rows = []
        for byte in chunk:
            if byte == 10:
                if not self.discarding:
                    rows.append(self.pending.rstrip(b"\r").decode("utf-8", "replace").split(","))
                self.pending.clear()
                self.discarding = False
            elif not self.discarding:
                if len(self.pending) >= 65536:
                    self.pending.clear()
                    self.discarding = True
                    self.dropped += 1
                else:
                    self.pending.append(byte)
        return rows


def display_lines(values, fields):
    # Include configured columns missing from this row, without inventing zeroes.
    indices = sorted(set(range(len(values))) | fields.keys())
    lines = []
    for index in indices:
        name, kind = fields.get(index, (f"field_{index}", "unknown"))
        value = values[index].strip() if index < len(values) else "<missing>"
        value = "".join(c if c.isprintable() else "?" for c in value)
        lines.append(f"{index:>3}  {name:<22} {kind:<7}  {value or '<empty>'}")
    return lines


def read_chunk(fd):
    try:
        chunk = os.read(fd, 4096)
    except OSError as error:
        # A pty reports EIO once the emulator closes it; USB serial on unplug.
        if error.errno == errno.EIO:
            raise DeviceClosed from error
        raise
    if not chunk:
        raise DeviceClosed
    return chunk


def run(fd, fields, args, screen=None):
    reader = LineReader()
    values = None
    samples = offset = 0
    last_seen = None
    closed = False
    if screen is not None:
        screen.nodelay(True)
        screen.keypad(True)
        try:
            curses.curs_set(0)
        except curses.error:
            pass
    while True:
        if closed:
            time.sleep(0.1)
        elif select.select([fd], [], [], 0.1)[0]:
            try:
                chunk = read_chunk(fd)
            except DeviceClosed:
                if screen is None:
                    raise
                # Keep the last sample on screen until the user quits.
                closed, chunk = True, b""
            for values in reader.feed(chunk):
                samples += 1
                last_seen = time.monotonic()
                if screen is None:
                    print(f"Sample {samples}")
                    print("\n".join(display_lines(values, fields)), flush=True)
                if args.count and samples >= args.count:
                    return
        if screen is None:
            continue
        key = screen.getch()
        if key in (ord("q"), ord("Q")):
            return
        height, width = screen.getmaxyx()
        page = max(1, height - 3)
        lines = display_lines(values, fields) if values is not None else ["Waiting for a complete sensor line..."]
        if key in (curses.KEY_DOWN, ord("j")):
            offset += 1
        elif key in (curses.KEY_UP, ord("k")):
            offset -= 1
        elif key == curses.KEY_NPAGE:
            offset += page
        elif key == curses.KEY_PPAGE:
            offset -= page
        offset = max(0, min(offset, max(0, len(lines) - page)))
        age = f"{time.monotonic() - last_seen:.1f}s" if last_seen is not None else "--"
        state = " | CLOSED: end of stream or device disconnected" if closed else ""
        header = f"Sensor | {args.baud} baud | samples={samples} age={age} dropped={reader.dropped}{state}"
        footer = f"q: quit  arrows/PgUp/PgDn: scroll  rows {offset + 1}-{min(offset + page, len(lines))}/{len(lines)}"
        screen.erase()
        for y, text in enumerate([header, "Idx  Field                  Type     Value"] + lines[offset:offset + page]):
            if y < height - 1:
                screen.addnstr(y, 0, text, max(0, width - 1))
        if height > 0:
            screen.addnstr(height - 1, 0, footer, max(0, width - 1))
        screen.refresh()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default=DEFAULT_DEVICE)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--config", default=default_config(),
                        help="Default: $ARGUS_SENSOR_FIELDS, else $ARGUS_DATA_DIR/sensor-fields.conf "
                             "(/etc/argus), else config/sensor-fields.conf in this repository")
    parser.add_argument("--plain", action="store_true", help="Print successive samples instead of refreshing in place")
    parser.add_argument("--count", type=int, default=0, help="Stop after N samples (0: unlimited)")
    args = parser.parse_args()
    if args.count < 0:
        parser.error("--count must be nonnegative")
    try:
        fields = load_fields(args.config)
        with serial_port(args.device, args.baud) as fd:
            if not args.plain and sys.stdout.isatty() and sys.stdin.isatty():
                curses.wrapper(lambda screen: run(fd, fields, args, screen))
            else:
                run(fd, fields, args)
    except KeyboardInterrupt:
        return 0
    except DeviceClosed:
        print(f"sensor_viewer: {args.device} closed (end of stream or device disconnected)",
              file=sys.stderr)
        return 1
    except (OSError, ValueError, curses.error, termios.error) as error:
        print(f"sensor_viewer: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
