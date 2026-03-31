#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("Missing dependency: pyserial")
    print("Install with: pip install pyserial")
    sys.exit(1)


# LED en pin 6 (no usar 13: es SCK del TFT). Cambia BLINK_PIN si usas otro GPIO.
BLINK_PIN = 2

PROGRAM_LINES = [
    "eperase",
    "bcclr",
    'bcstr("encendiendo")',
    'bcstr("apagando")',
    f"bc(high,{BLINK_PIN})",
    "bcx(5,2,0)",
    "bc(sleep,10)",
    f"bc(low,{BLINK_PIN})",
    "bcx(5,2,1)",
    "bc(sleep,10)",
    "bc(goto,0)",
    "bcsave",
    "run(0)",
]


def read_response(
    ser: serial.Serial,
    *,
    idle_s: float,
    max_total_s: float,
    first_byte_timeout_s: float,
) -> str:
    """Lee todo lo que envía el dispositivo hasta un tramo idle_s sin bytes (máx. max_total_s)."""
    t0 = time.time()
    abs_deadline = t0 + max_total_s
    chunks: list[str] = []
    last_data: float | None = None
    while time.time() < abs_deadline:
        waiting = ser.in_waiting
        if waiting:
            chunks.append(ser.read(waiting).decode(errors="replace"))
            last_data = time.time()
        else:
            if chunks and last_data is not None and (time.time() - last_data >= idle_s):
                break
            if not chunks and (time.time() - t0 >= first_byte_timeout_s):
                break
            time.sleep(0.012)
    return "".join(chunks)


def main() -> int:
    parser = argparse.ArgumentParser(description="Send TFT bytecode test program via serial")
    parser.add_argument("--port", required=True, help="Serial port, e.g. /dev/cu.usbmodem1101")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--line-delay", type=float, default=0.12, help="Delay between lines in seconds")
    parser.add_argument(
        "--read-idle",
        type=float,
        default=0.08,
        help="Tras el último byte, esperar este silencio (s) antes de considerar la respuesta completa",
    )
    parser.add_argument(
        "--read-max",
        type=float,
        default=3.0,
        help="Tiempo máximo total (s) acumulando respuesta por cada línea enviada",
    )
    parser.add_argument(
        "--read-first",
        type=float,
        default=1.5,
        help="Si no llega ningún byte en este tiempo (s), se sigue (respuesta vacía)",
    )
    parser.add_argument("--initial-wait", type=float, default=1.8, help="Wait after opening port (seconds)")
    args = parser.parse_args()

    print(f"Opening {args.port} @ {args.baud}...")
    with serial.Serial(args.port, args.baud, timeout=0.05) as ser:
        time.sleep(args.initial_wait)
        ser.reset_input_buffer()

        for line in PROGRAM_LINES:
            payload = (line + "\n").encode()
            ser.write(payload)
            ser.flush()
            print(f"> {line}")

            resp = read_response(
                ser,
                idle_s=args.read_idle,
                max_total_s=args.read_max,
                first_byte_timeout_s=args.read_first,
            )
            if resp:
                for part in resp.splitlines(keepends=True):
                    print(f"< {part}", end="" if part.endswith("\n") else "\n")
            else:
                print("< (sin respuesta)")

            time.sleep(args.line_delay)

        print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
