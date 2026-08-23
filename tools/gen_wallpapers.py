#!/usr/bin/env python3
"""Gera wallpapers 200x200 1-bit em estilo line-art (sem dithering de
foto), coerente com o sistema visual v4 do firmware (pilulas
arredondadas, tracos finos, muito espaco em branco). Renderiza em 4x
(800x800) com anti-aliasing e reduz por LANCZOS antes de binarizar, pra
sair com bordas suaves em vez de ruido de Floyd-Steinberg. As PNGs
resultantes viram src/ui/wallpapers.h via tools/img2header.py.

Uso:
    python tools/gen_wallpapers.py <dir_saida>
    python tools/img2header.py <dir_saida>/WALLPAPER_X.png WALLPAPER_X >> src/ui/wallpapers.h
"""
import math
import random
import sys
from pathlib import Path

from PIL import Image, ImageDraw

SS = 4  # supersampling
S = 200 * SS


def finalize(img: Image.Image) -> Image.Image:
    img = img.resize((200, 200), Image.LANCZOS)
    return img.point(lambda p: 255 if p >= 140 else 0).convert("1")


def gen_mountains() -> Image.Image:
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)

    # sol/lua: circulo fino no ceu
    d.ellipse([S * 0.68, S * 0.16, S * 0.68 + S * 0.14, S * 0.16 + S * 0.14],
              outline=0, width=3 * SS)

    random.seed(7)
    ridges = [
        (0.92, 0.55),
        (0.78, 0.42),
        (0.62, 0.30),
    ]
    for base_y, peak_y in ridges:
        pts = [(-10 * SS, S * base_y)]
        n = 7
        for i in range(1, n):
            x = S * i / (n - 1)
            y = S * (peak_y + random.uniform(-0.05, 0.05)) if i % 2 else S * base_y
            pts.append((x, y))
        pts.append((S + 10 * SS, S * base_y))
        d.line(pts, fill=0, width=3 * SS, joint="curve")

    return finalize(img)


def gen_topo() -> Image.Image:
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    cx, cy = S * 0.5, S * 0.6

    # forma unica (soma de 2 harmonicos com fase fixa) reaproveitada em
    # todo anel, so escalada pelo raio - assim os aneis ficam paralelos
    # como curvas de nivel de verdade, sem se cruzar.
    def shape(a):
        return 1.0 + 0.10 * math.sin(a * 2 + 0.7) + 0.05 * math.sin(a * 5 - 1.1)

    angles = [t / 90 * 2 * math.pi for t in range(91)]
    for r in range(int(S * 0.09), int(S * 0.60), int(S * 0.065)):
        pts = [(cx + r * shape(a) * math.cos(a), cy + r * shape(a) * math.sin(a) * 0.85)
               for a in angles]
        d.line(pts, fill=0, width=3 * SS, joint="curve")
    return finalize(img)


def gen_stars() -> Image.Image:
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    random.seed(11)
    pts = []
    for _ in range(28):
        x = random.uniform(S * 0.08, S * 0.92)
        y = random.uniform(S * 0.08, S * 0.85)
        pts.append((x, y))
        r = random.choice([3, 3, 4, 6]) * SS
        d.ellipse([x - r, y - r, x + r, y + r], fill=0)

    # conecta alguns pontos proximos em uma constelacao sutil
    random.shuffle(pts)
    for i in range(0, min(10, len(pts) - 1), 2):
        d.line([pts[i], pts[i + 1]], fill=0, width=int(1.5 * SS))
    return finalize(img)


def gen_waves() -> Image.Image:
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    for row in range(9):
        y0 = S * (0.18 + row * 0.085)
        amp = S * 0.028
        pts = [(x, y0 + amp * math.sin(x / S * 3 * math.pi + row * 0.6))
               for x in range(0, S + 1, 6)]
        d.line(pts, fill=0, width=3 * SS, joint="curve")
    return finalize(img)


def gen_dots() -> Image.Image:
    img = Image.new("L", (S, S), 255)
    d = ImageDraw.Draw(img)
    step = S // 11
    for row in range(11):
        for col in range(11):
            x = step * col + step // 2
            y = step * row + step // 2
            dist = math.hypot(x - S * 0.5, y - S * 0.42) / S
            r = max(1.5, 7 - dist * 11) * SS
            d.ellipse([x - r, y - r, x + r, y + r], fill=0)
    return finalize(img)


GENERATORS = {
    "WALLPAPER_MOUNTAINS": gen_mountains,
    "WALLPAPER_TOPO": gen_topo,
    "WALLPAPER_STARS": gen_stars,
    "WALLPAPER_WAVES": gen_waves,
    "WALLPAPER_DOTS": gen_dots,
}


def main():
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, fn in GENERATORS.items():
        img = fn()
        path = out_dir / f"{name}.png"
        img.save(path)
        print(f"gerado {path}")


if __name__ == "__main__":
    main()
