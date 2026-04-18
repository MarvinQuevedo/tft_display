#!/usr/bin/env python3
"""
Convierte fotos JPG a arrays RGB565 para pantalla TFT ST7789V (240x320).
Genera tft_photos_esp32/photo_data.h con los datos en PROGMEM.

Uso: python3 tft_photos/convert_photos.py
"""

import os
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("ERROR: instala Pillow ->  pip3 install Pillow")

SCRIPT_DIR = Path(__file__).parent
PROJECT_ROOT = SCRIPT_DIR.parent
PHOTOS_DIR = SCRIPT_DIR
OUTPUT_DIR = PROJECT_ROOT / "tft_photos_esp32"
OUTPUT_H = OUTPUT_DIR / "photo_data.h"

TFT_W = 240
TFT_H = 320


def rgb888_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def center_crop_and_resize(img: Image.Image, target_w: int, target_h: int) -> Image.Image:
    """Recorta al centro manteniendo relación de aspecto, luego redimensiona."""
    src_w, src_h = img.size
    src_ratio = src_w / src_h
    tgt_ratio = target_w / target_h

    if src_ratio > tgt_ratio:
        # imagen más ancha: recortar lados
        new_w = int(src_h * tgt_ratio)
        left = (src_w - new_w) // 2
        img = img.crop((left, 0, left + new_w, src_h))
    else:
        # imagen más alta: recortar arriba/abajo
        new_h = int(src_w / tgt_ratio)
        top = (src_h - new_h) // 2
        img = img.crop((0, top, src_w, top + new_h))

    return img.resize((target_w, target_h), Image.LANCZOS)


def convert_image(path: Path) -> list[int]:
    """Retorna lista de valores uint16_t RGB565 (row-major)."""
    img = Image.open(path).convert("RGB")
    img = center_crop_and_resize(img, TFT_W, TFT_H)
    pixels = []
    for y in range(TFT_H):
        for x in range(TFT_W):
            r, g, b = img.getpixel((x, y))
            pixels.append(rgb888_to_rgb565(r, g, b))
    return pixels


def write_header(images: list[tuple[str, list[int]]]) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT_H, "w") as f:
        f.write("// Auto-generado por convert_photos.py — NO editar manualmente\n")
        f.write("#pragma once\n")
        f.write("#include <pgmspace.h>\n\n")
        f.write(f"#define PHOTO_W {TFT_W}\n")
        f.write(f"#define PHOTO_H {TFT_H}\n")
        f.write(f"#define PHOTO_COUNT {len(images)}\n\n")

        array_names = []
        for name, pixels in images:
            arr_name = f"photo_{name}"
            array_names.append(arr_name)
            total = len(pixels)
            f.write(f"// {name}  ({TFT_W}x{TFT_H}, {total} pixels, {total*2} bytes)\n")
            f.write(f"const uint16_t {arr_name}[] PROGMEM = {{\n")
            row_size = TFT_W
            for i in range(0, total, row_size):
                row = pixels[i : i + row_size]
                f.write("  " + ", ".join(f"0x{v:04X}" for v in row) + ",\n")
            f.write("};\n\n")

        f.write("const uint16_t* const PHOTOS[] PROGMEM = {\n")
        for n in array_names:
            f.write(f"  {n},\n")
        f.write("};\n")

    print(f"Header escrito: {OUTPUT_H}")


def main() -> None:
    jpgs = sorted(PHOTOS_DIR.glob("*.JPG")) + sorted(PHOTOS_DIR.glob("*.jpg"))
    if not jpgs:
        sys.exit(f"No se encontraron fotos en {PHOTOS_DIR}")

    print(f"Encontradas {len(jpgs)} fotos:")
    images: list[tuple[str, list[int]]] = []
    for i, path in enumerate(jpgs):
        short = f"img{i:02d}"
        print(f"  [{i}] {path.name}  -> array '{short}' ...")
        pixels = convert_image(path)
        images.append((short, pixels))
        size_kb = len(pixels) * 2 / 1024
        print(f"       {size_kb:.1f} KB RGB565")

    total_kb = sum(len(px) * 2 for _, px in images) / 1024
    print(f"\nTotal en flash: {total_kb:.1f} KB ({total_kb/1024:.2f} MB)")
    write_header(images)


if __name__ == "__main__":
    main()
