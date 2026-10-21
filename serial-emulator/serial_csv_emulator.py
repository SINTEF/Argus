#!/usr/bin/env python3

import argparse
import os
import pty
import signal
import sys
import termios
import time
from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


SOURCE_TIMESTAMP_FORMAT = "%m/%d/%Y %H:%M:%S.%f"
OUTPUT_TIMESTAMP_FORMAT = "%d/%m/%Y %H:%M:%S"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Create a virtual serial device and stream CSV rows to it as "
            "ASCII comma-separated lines."
        )
    )
    parser.add_argument(
        "--csv",
        dest="csv_file",
        required=True,
        type=Path,
        help="Path to the CSV file to stream",
    )
    parser.add_argument(
        "--rate",
        dest="rate_ms",
        required=True,
        type=int,
        help="Delay in milliseconds between each streamed sample line",
    )
    timestamp_mode = parser.add_mutually_exclusive_group()
    timestamp_mode.add_argument(
        "--fix-time",
        action="store_true",
        help=(
            "Replace each CSV timestamp with the initial CSV timestamp plus "
            "elapsed wall time since streaming started"
        ),
    )
    timestamp_mode.add_argument(
        "--live",
        action="store_true",
        help="Replace each CSV timestamp with the current local timestamp",
    )
    parser.add_argument(
        "--wait",
        action="store_true",
        help="Wait until another process has opened the emulated serial device",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=None,
        help="Limit the output to N samples before closing",
    )
    parser.add_argument(
        "--loop",
        action="store_true",
        help=(
            "Loop the server; when the end of the logfile is reached, start "
            "from the beginning"
        ),
    )
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> int:
    if args.rate_ms < 0:
        print("--rate must be >= 0", file=sys.stderr)
        return 2

    if args.limit is not None and args.limit < 0:
        print("--limit must be >= 0", file=sys.stderr)
        return 2

    if not args.csv_file.is_file():
        print(f"CSV file not found: {args.csv_file}", file=sys.stderr)
        return 2

    return 0


def configure_slave_tty(fd: int) -> None:
    attrs = termios.tcgetattr(fd)

    attrs[0] = 0
    attrs[1] = 0
    attrs[2] |= termios.CREAD | termios.CLOCAL | termios.CS8
    attrs[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE)
    attrs[3] = 0

    for idx in (4, 5):
        attrs[idx] = termios.B115200

    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0

    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def is_open_by_another_process(device_path: str) -> bool:
    """Return true when another process has opened the virtual device."""
    target_path = os.path.realpath(device_path)
    current_pid = os.getpid()

    for proc_entry in Path("/proc").iterdir():
        if not proc_entry.name.isdigit() or int(proc_entry.name) == current_pid:
            continue

        fd_dir = proc_entry / "fd"
        try:
            fd_entries = list(fd_dir.iterdir())
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue

        for fd_entry in fd_entries:
            try:
                link_target = os.readlink(fd_entry)
            except (FileNotFoundError, PermissionError, ProcessLookupError):
                continue

            if link_target == target_path:
                return True

            if (
                link_target.startswith("/")
                and os.path.realpath(link_target) == target_path
            ):
                return True

    return False


def wait_for_device_open(device_path: str, should_continue: Callable[[], bool]) -> bool:
    while should_continue():
        if is_open_by_another_process(device_path):
            return True
        time.sleep(0.1)

    return False


def wait_for_reader_if_needed(
    wait_enabled: bool, device_path: str, should_continue: Callable[[], bool]
) -> bool:
    if not wait_enabled or is_open_by_another_process(device_path):
        return True

    print(
        f"Waiting for another process to open {device_path}",
        file=sys.stderr,
        flush=True,
    )
    return wait_for_device_open(device_path, should_continue)


def split_timestamp_field(line: str) -> tuple[str, str]:
    timestamp_text, separator, remainder = line.partition(",")
    if not separator:
        raise ValueError("CSV row does not contain any comma-separated fields")
    return timestamp_text, remainder


def parse_source_timestamp_ms(timestamp_text: str) -> int:
    dt = datetime.strptime(timestamp_text, SOURCE_TIMESTAMP_FORMAT).replace(
        tzinfo=timezone.utc
    )
    return int(dt.timestamp() * 1000)


def format_timestamp_field(epoch_ms: int, *, local_time: bool = False) -> str:
    seconds = epoch_ms // 1000
    milliseconds = epoch_ms % 1000

    if local_time:
        dt = datetime.fromtimestamp(seconds).astimezone()
    else:
        dt = datetime.fromtimestamp(seconds, tz=timezone.utc)

    return f"{dt.strftime(OUTPUT_TIMESTAMP_FORMAT)}.{milliseconds:03d}"


def get_current_local_epoch_ms() -> int:
    return int(datetime.now().astimezone().timestamp() * 1000)


@dataclass
class TimestampConverter:
    live: bool
    fix_time: bool
    fixed_base_epoch_ms: int | None = None
    fixed_stream_start_ns: int | None = None

    def convert(self, line: str) -> str:
        timestamp_text, remainder = split_timestamp_field(line)
        epoch_ms = self._get_epoch_ms(timestamp_text)
        return f"{format_timestamp_field(epoch_ms, local_time=self.live)},{remainder}"

    def _get_epoch_ms(self, timestamp_text: str) -> int:
        if self.live:
            return get_current_local_epoch_ms()

        if self.fix_time:
            return self._get_fixed_epoch_ms(timestamp_text)

        return parse_source_timestamp_ms(timestamp_text)

    def _get_fixed_epoch_ms(self, timestamp_text: str) -> int:
        if self.fixed_base_epoch_ms is None:
            self.fixed_base_epoch_ms = parse_source_timestamp_ms(timestamp_text)
            self.fixed_stream_start_ns = time.monotonic_ns()

        if self.fixed_stream_start_ns is None:
            raise RuntimeError("fixed timestamp state was not initialized")

        elapsed_ms = (time.monotonic_ns() - self.fixed_stream_start_ns) // 1_000_000
        return self.fixed_base_epoch_ms + elapsed_ms


def get_timestamp_converter(args: argparse.Namespace) -> TimestampConverter:
    converter = getattr(args, "_timestamp_converter", None)
    if converter is None:
        converter = TimestampConverter(args.live, args.fix_time)
        args._timestamp_converter = converter

    return converter


def convert_timestamp(line: str, args: argparse.Namespace) -> str:
    return get_timestamp_converter(args).convert(line)


def main() -> int:
    args = parse_args()

    validation_result = validate_args(args)
    if validation_result != 0:
        return validation_result

    master_fd, slave_fd = pty.openpty()
    slave_path = os.ttyname(slave_fd)
    configure_slave_tty(slave_fd)

    running = True

    def stop_stream(_signum: int, _frame: object) -> None:
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop_stream)
    signal.signal(signal.SIGTERM, stop_stream)

    interval_s = args.rate_ms / 1000.0

    print(f"Emulated serial port: {slave_path}", flush=True)

    rows_sent = 0

    try:
        if not wait_for_reader_if_needed(args.wait, slave_path, lambda: running):
            return 130

        print(
            f"Streaming data from {args.csv_file} every {args.rate_ms} ms",
            file=sys.stderr,
            flush=True,
        )

        with args.csv_file.open("r", encoding="ascii", newline="") as csv_file:
            while running:
                csv_file.seek(0)
                next(csv_file, None)
                rows_sent_this_pass = 0
                for raw_line in csv_file:
                    if not running:
                        break
                    if args.limit is not None and rows_sent >= args.limit:
                        break

                    line = raw_line.rstrip("\r\n")
                    if not line:
                        continue

                    if not wait_for_reader_if_needed(
                        args.wait, slave_path, lambda: running
                    ):
                        return 130

                    converted_line = convert_timestamp(line, args)

                    payload = f"{converted_line}\n".encode("ascii", errors="strict")
                    os.write(master_fd, payload)
                    rows_sent += 1
                    rows_sent_this_pass += 1
                    if rows_sent % 10 == 0:
                        print(f"Sent {rows_sent} rows")
                    if interval_s > 0:
                        time.sleep(interval_s)

                if not running or (
                    args.limit is not None and rows_sent >= args.limit
                ):
                    break
                if not args.loop:
                    break
                if rows_sent_this_pass == 0:
                    break

    except UnicodeDecodeError as exc:
        print(f"CSV file must contain ASCII data only: {exc}", file=sys.stderr)
        return 2
    except ValueError as exc:
        print(f"Invalid CSV timestamp format: {exc}", file=sys.stderr)
        return 2
    finally:
        os.close(master_fd)
        os.close(slave_fd)

    print(f"Stopped after sending {rows_sent} rows", file=sys.stderr, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
