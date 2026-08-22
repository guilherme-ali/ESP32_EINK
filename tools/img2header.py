#!/usr/bin/env python3
"""Converte uma imagem para um bitmap 1bpp 200x200 pronto para o
e-paper (mesmo layout de buffer usado em src/display/epaper.cpp:
MSB primeiro, bit 1 = branco, bit 0 = preto, 25 bytes por linha).

Uso:
    python tools/img2header.py entrada.jpg NOME_DA_VARIAVEL > saida.h
    python tools/img2header.py entrada.jpg NOME_DA_VARIAVEL --out saida.h

A imagem e redimensionada preenchendo os 200x200 (crop central) e
convertida para 1-bit com dithering Floyd-Steinberg - bom tanto para
fotos quanto para ilustracoes/line art (que ja sao quase binarias, o
dithering nao degrada nesse caso).
"""
import sys
import argparse
from PIL import Image, ImageOps

WIDTH = 200
HEIGHT = 200


def to_epaper_bytes(img: Image.Image) -> bytes:
    img = ImageOps.fit(img.convert("L"), (WIDTH, HEIGHT), Image.LANCZOS)
    img = img.convert("1", dither=Image.FLOYDSTEINBERG)  # 0=preto, 255=branco

    buf = bytearray(WIDTH * HEIGHT // 8)
    px = img.load()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if px[x, y]:  # branco
                buf[y * (WIDTH // 8) + (x >> 3)] |= (0x80 >> (x & 7))
    return bytes(buf)


def emit_header(name: str, data: bytes, out):
    out.write(f"// Gerado por tools/img2header.py - {WIDTH}x{HEIGHT} 1bpp\n")
    out.write(f"static const uint8_t {name}[{len(data)}] PROGMEM = {{\n")
    for i in range(0, len(data), 16):
        row = ", ".join(f"0x{b:02X}" for b in data[i:i + 16])
        out.write(f"  {row},\n")
    out.write("};\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", help="Arquivo de imagem de entrada")
    ap.add_argument("name", help="Nome da variavel C (ex: WALLPAPER_MOUNTAINS)")
    ap.add_argument("--out", help="Arquivo de saida (default: stdout)")
    args = ap.parse_args()

    img = Image.open(args.image)
    data = to_epaper_bytes(img)

    if args.out:
        with open(args.out, "w") as f:
            emit_header(args.name, data, f)
        print(f"Escrito {args.out} ({len(data)} bytes)", file=sys.stderr)
    else:
        emit_header(args.name, data, sys.stdout)


if __name__ == "__main__":
    main()
