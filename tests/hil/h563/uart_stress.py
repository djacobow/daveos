#!/usr/bin/env python3
"""Exercise the STM32 console over UART; requires pyserial and connected hardware."""
import argparse
from pathlib import Path
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", help="ST-LINK UART device, preferably /dev/serial/by-id/...")
    parser.add_argument("--baud", type=int, default=1_000_000)
    parser.add_argument("--repeat", type=int, default=10)
    parser.add_argument("--output", type=Path, default=Path("build/hardware/uart-stress"))
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    failures = []
    with serial.Serial(args.device, args.baud, timeout=.01) as port:
        for repetition in range(1, args.repeat + 1):
            with (args.output / f"{repetition:02}.log").open("wb") as log:
                def collect(seconds):
                    deadline = time.monotonic() + seconds
                    data = bytearray()
                    while time.monotonic() < deadline:
                        data.extend(port.read(8192))
                    log.write(data)
                    log.flush()
                    return bytes(data)

                def send(payload, seconds):
                    port.write(payload)
                    port.flush()
                    return collect(seconds)

                def counters(phase):
                    data = send(b"board stats\r", .5)
                    match = re.search(rb"TX DMA: [^\r\n]* (\d+) dropped frames, (\d+) errors", data)
                    if not match:
                        raise RuntimeError(f"{phase}: missing UART statistics")
                    dropped, errors = map(int, match.groups())
                    print(f"run {repetition} {phase}: dropped={dropped}, errors={errors}", flush=True)
                    if dropped or errors:
                        failures.append((repetition, phase, "transmit counters", dropped, errors))

                # Start every repetition from a software reset, without a debugger.
                send(b"\rboard reset\r", 5)
                port.reset_input_buffer()
                counters("baseline")
                completed = 0
                for label, payload, count, rounds in [
                    ("short", b"board button\r", 1, 50),
                    ("long", b"board button".ljust(256) + b"\r", 1, 50),
                    ("16 short", b"board button\r" * 16, 16, 20),
                    ("16 long", (b"board button".ljust(256) + b"\r") * 16, 16, 10),
                ]:
                    for attempt in range(rounds):
                        data = send(payload, .15 if count == 1 else .3)
                        # Allow host scheduling/USB latency without mistaking it
                        # for lost bytes; still require exactly the expected replies.
                        if data.count(b"BTN1:") < count:
                            data += collect(1)
                        found = data.count(b"BTN1:")
                        if found != count or b"Dropped " in data:
                            failures.append((repetition, label, attempt, "replies", found, count))
                        completed += found
                    counters(label)
                data = send(b"x" * 300 + b"\rboard button\r", .5)
                if b"command line too long" not in data or data.count(b"BTN1:") != 1:
                    failures.append((repetition, "overlength recovery"))
                counters("final")
                print(f"run {repetition}: {completed}/580 burst replies", flush=True)
    for failure in failures:
        print("FAIL:", failure)
    print(f"{args.repeat} repetitions; {len(failures)} failed checks; logs: {args.output}")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
