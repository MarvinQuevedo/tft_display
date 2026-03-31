#!/usr/bin/env python3
"""
Terminal serial interactiva tipo Arduino Serial Monitor.

Uso:
  python3 serial_tools/serial_terminal.py --port /dev/cu.usbmodemXXXX --baud 115200
"""

import argparse
import sys
import threading
import time

import serial
from serial.tools import list_ports


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Monitor serial interactivo")
    parser.add_argument("--port", help="Puerto serie (ej. /dev/cu.usbmodem14101)")
    parser.add_argument("--baud", type=int, default=115200, help="Baudrate (default: 115200)")
    parser.add_argument(
        "--eol",
        choices=["lf", "crlf", "cr"],
        default="lf",
        help="Terminador de línea al enviar (default: lf)",
    )
    parser.add_argument("--timeout", type=float, default=0.1, help="Timeout de lectura en segundos")
    return parser.parse_args()


def list_arduino_ports() -> list[list_ports.ListPortInfo]:
    ports = list(list_ports.comports())
    if not ports:
        return []
    ranked: list[tuple[int, list_ports.ListPortInfo]] = []
    for p in ports:
        hay = f"{p.device} {p.description} {p.manufacturer or ''}".lower()
        score = 1
        if "arduino" in hay:
            score = 0
        elif "usbmodem" in hay or "wch" in hay or "cp210" in hay or "ch340" in hay:
            score = 0
        ranked.append((score, p))
    ranked.sort(key=lambda x: (x[0], x[1].device))
    return [p for _, p in ranked]


def choose_port_interactive(explicit_port: str | None) -> str | None:
    if explicit_port:
        return explicit_port
    ports = list_arduino_ports()
    if not ports:
        print("[error] no se detectaron puertos serie.")
        return None
    print("Puertos disponibles:")
    for i, p in enumerate(ports, start=1):
        desc = p.description or "sin descripcion"
        mf = f" | {p.manufacturer}" if p.manufacturer else ""
        print(f"  {i}) {p.device} - {desc}{mf}")
    while True:
        raw = input("Elige puerto [numero] (o 'q' para salir): ").strip().lower()
        if raw in ("q", "quit", "exit"):
            return None
        if raw.isdigit():
            idx = int(raw)
            if 1 <= idx <= len(ports):
                return ports[idx - 1].device
        print("Seleccion invalida.")


def eol_bytes(mode: str) -> bytes:
    if mode == "crlf":
        return b"\r\n"
    if mode == "cr":
        return b"\r"
    return b"\n"


def reader_loop(ser: serial.Serial, stop_event: threading.Event) -> None:
    while not stop_event.is_set():
        try:
            waiting = ser.in_waiting
            if waiting:
                data = ser.read(waiting)
                if data:
                    sys.stdout.write(data.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
            else:
                time.sleep(0.02)
        except serial.SerialException as exc:
            print(f"\n[error] lectura serial: {exc}")
            stop_event.set()
            return


def main() -> int:
    args = parse_args()
    line_end = eol_bytes(args.eol)
    port = choose_port_interactive(args.port)
    if not port:
        return 1

    try:
        ser = serial.Serial(port, args.baud, timeout=args.timeout)
    except serial.SerialException as exc:
        print(f"[error] no se pudo abrir {port}: {exc}")
        return 1

    stop_event = threading.Event()
    thread = threading.Thread(target=reader_loop, args=(ser, stop_event), daemon=True)
    thread.start()

    print(f"[ok] conectado a {port} @ {args.baud}")
    print("[tip] escribe comandos y Enter. Usa 'exit' o Ctrl+C para salir.\n")

    try:
        while not stop_event.is_set():
            try:
                line = input()
            except EOFError:
                break
            if line.strip().lower() == "exit":
                break
            payload = line.encode("utf-8", errors="replace") + line_end
            try:
                ser.write(payload)
                ser.flush()
            except serial.SerialException as exc:
                print(f"[error] escritura serial: {exc}")
                break
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        thread.join(timeout=0.3)
        if ser.is_open:
            ser.close()
        print("\n[ok] terminal cerrada")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
