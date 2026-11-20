#!/usr/bin/env python3
"""Save one completed AI Board capture as JSON over the UART0 console."""

import argparse
import json
import os
import sys
import time
from pathlib import Path
from urllib.parse import urlencode

import serial

CAPTURE_PREFIX = b"CAPTURE JSON "
ERROR_PREFIX = b"CAPTURE ERROR "
MAX_LINE_BYTES = 65536


def channel(value):
    token = value.upper().removeprefix("Y")
    if token not in "01234567" or len(token) != 1:
        raise argparse.ArgumentTypeError("use a Y channel from 0 through 7")
    return int(token)


def read_capture(connection, timeout):
    deadline = time.monotonic() + timeout
    pending = bytearray()
    while time.monotonic() < deadline:
        pending.extend(connection.read(min(connection.in_waiting or 1, 4096)))
        while (end := pending.find(b"\n")) >= 0:
            line = pending[:end].rstrip(b"\r")
            del pending[:end + 1]
            if line.startswith(ERROR_PREFIX):
                raise ValueError(line[len(ERROR_PREFIX):].decode(errors="replace"))
            if line.startswith(CAPTURE_PREFIX):
                payload = line[len(CAPTURE_PREFIX):]
                result = json.loads(payload)
                if not isinstance(result, dict) or not isinstance(result.get("channels"), list):
                    raise ValueError("console returned an invalid capture document")
                return bytes(payload)
        if len(pending) > MAX_LINE_BYTES:
            raise ValueError("console capture response exceeded the line limit")
    raise ValueError("no complete capture response received before timeout")


def release_on_close(connection):
    if os.name != "posix":
        return
    import termios

    settings = termios.tcgetattr(connection.fileno())
    settings[2] &= ~termios.HUPCL
    termios.tcsetattr(connection.fileno(), termios.TCSANOW, settings)


def capture(port, form, timeout):
    connection = serial.Serial(baudrate=115200, timeout=0.2, write_timeout=1, exclusive=True)
    connection.port = port
    connection.dtr = False
    connection.rts = False
    connection.open()
    with connection:
        release_on_close(connection)
        connection.dtr = False
        connection.rts = False
        time.sleep(0.3)
        connection.write(f"capture {urlencode(form)}\n".encode("ascii"))
        return read_capture(connection, timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="UART0 console serial port")
    parser.add_argument("channels", nargs="+", type=channel, metavar="Y",
                        help="one to four distinct Y channels in A/B/C/D order")
    parser.add_argument("--ms", type=int, default=150, metavar="MS", help="capture window, 10–500 ms")
    parser.add_argument("--baud", type=int, default=0, help="UART decoding hint, 0 for auto")
    parser.add_argument("--trigger", choices=("manual", "rising", "falling"), default="manual")
    parser.add_argument("--trigger-channel", choices="ABCD", default="A")
    parser.add_argument("--wait-ms", type=int, default=5000, help="edge trigger wait, 100–5000 ms")
    parser.add_argument("--output", "-o", default="-", metavar="FILE", help="capture JSON path, or - for stdout")
    args = parser.parse_args()
    if len(args.channels) > 4 or len(set(args.channels)) != len(args.channels):
        parser.error("specify one to four distinct Y channels")
    if not 10 <= args.ms <= 500 or args.baud != 0 and not 1200 <= args.baud <= 115200:
        parser.error("capture window must be 10–500 ms; baud must be 0 or 1200–115200")

    channels = args.channels + [-1] * (4 - len(args.channels))
    form = dict(zip("abcd", channels))
    form.update(ms=args.ms, baud=args.baud, trigger=args.trigger)
    timeout = args.ms / 1000 + 5
    if args.trigger != "manual":
        trigger_index = "ABCD".index(args.trigger_channel)
        if trigger_index >= len(args.channels) or not 100 <= args.wait_ms <= 5000:
            parser.error("choose a captured trigger channel and a wait of 100–5000 ms")
        form.update(trigger_channel=trigger_index, wait_ms=args.wait_ms)
        timeout += args.wait_ms / 1000

    try:
        result = capture(args.port, form, timeout) + b"\n"
        if args.output == "-":
            sys.stdout.buffer.write(result)
        else:
            Path(args.output).write_bytes(result)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Capture failed: {error}\n")


if __name__ == "__main__":
    main()
