#!/usr/bin/env python3
"""Gera uma fonte bitmap PROPORCIONAL (ASCII 0x20-0x7E) a partir de uma TTF,
no formato que src/ui/canvas.h/.cpp espera (struct Font/FontGlyph).

Ao contrario de tools/img2header.py (imagem -> bitmap fixo), aqui cada glifo
tem sua propria largura (o avanco de cursor da fonte de origem) e sua propria
matriz de bits, coluna a coluna: bit 0 = topo da coluna, MSB pra baixo nao -
usa (1 << row), ou seja o bit do row N mora sempre no byte (row >> 3) da
coluna. bytesPerCol = ceil(altura / 8).

Uso (sem argumentos, gera as 3 fontes padrao do firmware em src/ui/fonts.h):
    python tools/font2header.py

Uso avancado (uma fonte especifica em outro arquivo):
    python tools/font2header.py --ttf tools/fonts/OpenSans-Bold.ttf \
        --size 11 --name FONT_EMPHASIS --out algum.h
"""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FIRST_CHAR = 0x20
LAST_CHAR = 0xFF
# Pixel do glifo (fundo preto, texto branco) conta como tinta se
# brilho >= THRESHOLD. Mais baixo = mais generoso com bordas
# anti-aliased = traco mais grosso/solido, menos serrilhado no 1-bit
# sem antialiasing real do e-paper.
THRESHOLD = 70

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FONTS = [
    # (nome C, arquivo ttf, tamanho em pt, comentario)
    # 7pt (altura 11px) testado na placa e achado pequeno demais para
    # leitura confortavel; 10pt (altura 14px) melhorou mas ainda
    # serrilhado - 11pt (altura 16px) com THRESHOLD mais baixo (ver
    # acima) e o que ficou na placa.
    ("FONT_BODY", "OpenSans-Regular.ttf", 11, "corpo de texto"),
    ("FONT_EMPHASIS", "OpenSans-Bold.ttf", 11, "titulos, destaque, selecao"),
    ("FONT_CLOCK", "OpenSans-Regular.ttf", 29, "relogio da tela inicial"),
]


def render_glyph(font: ImageFont.FreeTypeFont, ch: str, height: int):
    adv = max(1, round(font.getlength(ch)))
    img = Image.new("L", (adv, height), 0)
    draw = ImageDraw.Draw(img)
    draw.text((0, 0), ch, font=font, fill=255)
    return adv, img


def pack_columns(img: Image.Image, width: int, height: int, bytes_per_col: int):
    px = img.load()
    cols = []
    for x in range(width):
        col = bytearray(bytes_per_col)
        for y in range(height):
            if px[x, y] >= THRESHOLD:
                col[y >> 3] |= (1 << (y & 7))
        cols.append(bytes(col))
    return cols


def emit_font(name: str, ttf_path: Path, size_pt: int, comment: str, out):
    font = ImageFont.truetype(str(ttf_path), size_pt)
    ascent, descent = font.getmetrics()
    height = ascent + descent
    bytes_per_col = (height + 7) // 8

    glyphs = []  # (char, width, [col_bytes...])
    for code in range(FIRST_CHAR, LAST_CHAR + 1):
        if 0x80 <= code <= 0x9F:
            width = 1
            cols = [bytes(bytes_per_col)]
        else:
            ch = chr(code)
            width, img = render_glyph(font, ch, height)
            cols = pack_columns(img, width, height, bytes_per_col)
        glyphs.append((code, width, cols))

    out.write(f"// {name}: {ttf_path.name} @ {size_pt}pt -> altura {height}px "
              f"({comment}). Gerado por tools/font2header.py, nao editar a mao.\n")

    for code, width, cols in glyphs:
        var = f"{name}_BM_{code:02X}"
        flat = bytearray()
        for c in cols:
            flat.extend(c)
        if flat:
            hex_bytes = ", ".join(f"0x{b:02X}" for b in flat)
            out.write(f"static const uint8_t {var}[] = {{ {hex_bytes} }};\n")
        else:
            out.write(f"static const uint8_t {var}[] = {{ 0 }}; // largura 0 (nao deveria ocorrer)\n")

    out.write(f"static const FontGlyph {name}_GLYPHS[FONT_GLYPH_COUNT] = {{\n")
    for code, width, cols in glyphs:
        var = f"{name}_BM_{code:02X}"
        label = chr(code) if (0x20 <= code <= 0x7E or 0xA0 <= code <= 0xFF) else f"0x{code:02X}"
        out.write(f"  {{ {width}, {var} }}, // 0x{code:02X} '{label}'\n")
    out.write("};\n")
    out.write(f"static const Font {name} = {{ {height}, {bytes_per_col}, {name}_GLYPHS }};\n\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ttf", help="Fonte TTF de entrada (modo fonte unica)")
    ap.add_argument("--size", type=int, help="Tamanho em pt (modo fonte unica)")
    ap.add_argument("--name", help="Nome C da fonte, ex: FONT_BODY (modo fonte unica)")
    ap.add_argument("--out", help="Arquivo de saida (default: stdout no modo fonte unica; "
                                   "src/ui/fonts.h no modo padrao)")
    args = ap.parse_args()

    single_mode = args.ttf or args.size or args.name
    if single_mode:
        if not (args.ttf and args.size and args.name):
            ap.error("modo fonte unica exige --ttf, --size e --name juntos")
        out = open(args.out, "w") if args.out else sys.stdout
        try:
            out.write("#pragma once\n#include \"fonts.h\"\n\n")
            emit_font(args.name, Path(args.ttf), args.size, "fonte avulsa", out)
        finally:
            if args.out:
                out.close()
                print(f"Escrito {args.out}", file=sys.stderr)
        return

    out_path = Path(args.out) if args.out else REPO_ROOT / "src" / "ui" / "fonts.h"
    fonts_dir = REPO_ROOT / "tools" / "fonts"
    with open(out_path, "w") as out:
        out.write("#pragma once\n#include <Arduino.h>\n\n")
        out.write("// Fontes bitmap proporcionais (ASCII 0x20-0x7E), geradas por\n"
                   "// tools/font2header.py a partir da Open Sans (Apache 2.0, ver\n"
                   "// tools/fonts/LICENSE.txt). Nao editar a mao - rode o script de novo.\n\n")
        out.write("struct FontGlyph {\n  uint8_t width;\n  const uint8_t *bitmap;\n};\n\n")
        out.write("struct Font {\n  uint8_t height;\n  uint8_t bytesPerCol;\n  const FontGlyph *glyphs;\n};\n\n")
        out.write(f"#define FONT_FIRST_CHAR 0x{FIRST_CHAR:02X}\n")
        out.write(f"#define FONT_LAST_CHAR 0x{LAST_CHAR:02X}\n")
        out.write("#define FONT_GLYPH_COUNT (FONT_LAST_CHAR - FONT_FIRST_CHAR + 1)\n\n")

        for name, ttf_file, size_pt, comment in DEFAULT_FONTS:
            emit_font(name, fonts_dir / ttf_file, size_pt, comment, out)

    print(f"Escrito {out_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
