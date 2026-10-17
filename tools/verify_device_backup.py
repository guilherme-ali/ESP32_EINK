#!/usr/bin/env python3
"""Confere imagem integral e partições sem mostrar credenciais nem áudio."""
import argparse
import hashlib
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--firmware", type=Path)
    args = parser.parse_args()
    image = args.image.read_bytes()
    if len(image) != 8 * 1024 * 1024:
        raise SystemExit(f"Backup incompleto: {len(image)} bytes; esperados 8388608")
    partitions = {}
    for offset in range(0x8000, 0x9000, 32):
        magic, kind, subtype, start, size, label, _ = struct.unpack_from("<HBBII16sI", image, offset)
        if magic in (0xFFFF, 0xEBEB):
            break
        if magic != 0x50AA or start + size > len(image):
            raise SystemExit("Tabela de partições inválida")
        name = label.split(b"\0", 1)[0].decode("ascii", errors="strict")
        partitions[name] = (kind, subtype, start, size)
        print(f"{name}: offset=0x{start:x} tamanho=0x{size:x}")
    expected = {"nvs": (1, 2, 0x9000, 0x6000), "factory": (0, 0, 0x10000, 3 * 1024 * 1024),
                "spiffs": (1, 0x82, 0x310000, 4864 * 1024)}
    for name, value in expected.items():
        if partitions.get(name) != value:
            raise SystemExit(f"Partição {name} diferente da configuração esperada; não gravar")
    if args.firmware:
        size = args.firmware.stat().st_size
        if not 0 < size <= expected["factory"][3]:
            raise SystemExit("Firmware não cabe na partição factory")
        print(f"Aplicação: {size} bytes; escrita restrita a 0x10000..0x{0x10000 + size:x}")
    print(f"Backup 8 MiB íntegro em tamanho; SHA-256: {hashlib.sha256(image).hexdigest()}")
    print("O hash identifica esta leitura; a comparação independente é feita pelo verify-flash/esptool.")


if __name__ == "__main__":
    main()
