#!/usr/bin/env python3
"""Confere o particionamento de um backup esptool sem exibir credenciais."""
import argparse
import hashlib
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backup", type=Path)
    args = parser.parse_args()
    flash = args.backup.read_bytes()
    if len(flash) != 8 * 1024 * 1024:
        raise SystemExit("Backup nao tem os 8 MiB esperados")
    partitions = {}
    for index in range(128):
        entry = flash[0x8000 + index * 32:0x8000 + (index + 1) * 32]
        magic = struct.unpack_from("<H", entry)[0]
        if magic in (0xFFFF, 0xEBEB):
            break
        if magic != 0x50AA:
            raise SystemExit("Tabela de particoes invalida")
        _, type_, subtype, offset, size, label, flags = struct.unpack("<HBBII16sI", entry)
        name = label.split(b"\0", 1)[0].decode("ascii")
        partitions[name] = (offset, size)
        digest = hashlib.sha256(flash[offset:offset + size]).hexdigest()
        print(f"{name}: offset=0x{offset:06x} size=0x{size:06x} SHA256={digest}")
    expected = {"nvs": (0x9000, 0x6000), "factory": (0x10000, 0x300000),
                "spiffs": (0x310000, 0x4C0000)}
    if any(partitions.get(name) != extent for name, extent in expected.items()):
        raise SystemExit("Particoes diferentes do projeto: nao gravar sem investigar")
    print("Backup completo e particionamento compativel: OK")


if __name__ == "__main__":
    main()
