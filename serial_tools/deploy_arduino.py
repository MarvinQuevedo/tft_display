#!/usr/bin/env python3
"""
Deploy interactivo con arduino-cli:
- Detecta placas conectadas
- Te deja elegir puerto/board
- Compila y sube el sketch
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


def run_cmd(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, capture_output=True, text=True)


def load_boards() -> list[dict[str, Any]]:
    cp = run_cmd(["arduino-cli", "board", "list", "--format", "json"])
    if cp.returncode != 0:
        print("[error] no se pudo ejecutar 'arduino-cli board list'")
        if cp.stderr:
            print(cp.stderr.strip())
        return []
    try:
        data = json.loads(cp.stdout)
    except json.JSONDecodeError:
        print("[error] salida JSON invalida de arduino-cli")
        return []

    if isinstance(data, dict):
        boards = data.get("detected_ports", [])
    elif isinstance(data, list):
        boards = data
    else:
        boards = []

    out: list[dict[str, Any]] = []
    for item in boards:
        port = item.get("port", {}) if isinstance(item, dict) else {}
        addr = port.get("address") or item.get("address") or ""
        proto_label = port.get("protocol_label") or ""
        matching = item.get("matching_boards") or []
        fqbn = ""
        name = ""
        if matching and isinstance(matching, list) and isinstance(matching[0], dict):
            fqbn = matching[0].get("fqbn", "") or ""
            name = matching[0].get("name", "") or ""
        out.append(
            {
                "address": addr,
                "protocol": proto_label,
                "fqbn": fqbn,
                "name": name,
            }
        )
    return [x for x in out if x.get("address")]


def choose_board(boards: list[dict[str, Any]], explicit_port: str | None, explicit_fqbn: str | None) -> dict[str, str] | None:
    if explicit_port and explicit_fqbn:
        return {"address": explicit_port, "fqbn": explicit_fqbn}

    if not boards:
        print("[error] no se detectaron placas. Conecta el Arduino y reintenta.")
        return None

    print("Arduinos/placas detectadas:")
    for i, b in enumerate(boards, start=1):
        name = b["name"] or "unknown"
        fqbn = b["fqbn"] or "sin fqbn"
        proto = b["protocol"] or "n/a"
        print(f"  {i}) {b['address']} | {name} | {fqbn} | {proto}")

    if explicit_port and not explicit_fqbn:
        for b in boards:
            if b["address"] == explicit_port and b["fqbn"]:
                return {"address": explicit_port, "fqbn": b["fqbn"]}
        print("[error] pasaste --port pero no se encontro fqbn automatico para ese puerto.")
        return None

    while True:
        raw = input("Elige placa [numero] (o 'q' para salir): ").strip().lower()
        if raw in ("q", "quit", "exit"):
            return None
        if raw.isdigit():
            idx = int(raw)
            if 1 <= idx <= len(boards):
                chosen = boards[idx - 1]
                fqbn = chosen["fqbn"] or ask_fqbn_interactive(None)
                if not fqbn:
                    continue
                return {"address": chosen["address"], "fqbn": fqbn}
        print("Seleccion invalida.")


def ask_fqbn_interactive(explicit_fqbn: str | None) -> str | None:
    if explicit_fqbn:
        return explicit_fqbn

    defaults = [
        ("arduino:avr:uno", "Arduino Uno"),
        ("arduino:avr:nano", "Arduino Nano (ATmega328P)"),
        ("arduino:avr:mega", "Arduino Mega 2560"),
    ]
    print("\nNo se detecto FQBN automaticamente.")
    print("Elige tipo de placa:")
    for i, (fqbn, name) in enumerate(defaults, start=1):
        print(f"  {i}) {name} ({fqbn})")
    print("  m) Escribir FQBN manual")

    while True:
        raw = input("Opcion [1-3/m] (o 'q' para salir): ").strip().lower()
        if raw in ("q", "quit", "exit"):
            return None
        if raw in ("1", "2", "3"):
            return defaults[int(raw) - 1][0]
        if raw in ("m", "manual"):
            fqbn = input("Escribe FQBN (ej. arduino:avr:uno): ").strip()
            if fqbn:
                return fqbn
            print("FQBN vacio.")
            continue
        print("Seleccion invalida.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Deploy interactivo para Arduino")
    parser.add_argument("--sketch", default="tft_display_uno", help="Carpeta del sketch")
    parser.add_argument("--port", help="Puerto serie opcional")
    parser.add_argument("--fqbn", help="FQBN opcional (ej: arduino:avr:uno)")
    args = parser.parse_args()

    sketch = Path(args.sketch).resolve()
    if not sketch.exists():
        print(f"[error] sketch no existe: {sketch}")
        return 1

    boards = load_boards()
    selected = choose_board(boards, args.port, args.fqbn)
    if not selected:
        return 1

    port = selected["address"]
    fqbn = selected["fqbn"]
    print(f"\nCompilando {sketch} con {fqbn} ...")
    cp = run_cmd(["arduino-cli", "compile", "--fqbn", fqbn, str(sketch)])
    if cp.returncode != 0:
        print("[error] fallo compilacion")
        print(cp.stdout.strip())
        print(cp.stderr.strip())
        return 1
    print("[ok] compilacion correcta")

    print(f"Subiendo a {port} ...")
    up = run_cmd(["arduino-cli", "upload", "-p", port, "--fqbn", fqbn, str(sketch)])
    if up.returncode != 0:
        print("[error] fallo upload")
        print(up.stdout.strip())
        print(up.stderr.strip())
        return 1
    print("[ok] upload completado")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
