#!/usr/bin/env python3
"""Reúne binários já compilados e bundle do gateway, sem ler configurações/segredos."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("C:/pio-builds/ESP32_EINK"))
    parser.add_argument("--apk", type=Path, default=Path(os.environ.get("LOCALAPPDATA", "")) /
                        "Temp/opencode/ideiarec-android-toolchain/build/app/outputs/apk/debug/app-debug.apk")
    args = parser.parse_args()
    files = {
        "firmware-battery.bin": args.build_dir / "battery/firmware.bin",
        "firmware-debug-usb.bin": args.build_dir / "esp32-s3-devkitc-1/firmware.bin",
        "IdeiaRec-debug.apk": args.apk,
        "gateway-Code.gs": ROOT / "gateway/dist/Code.gs",
        "gateway-appsscript.json": ROOT / "gateway/dist/appsscript.json",
        "libopus-COPYING.txt": args.build_dir / "voice-opus/opus-1.5.2/COPYING",
    }
    for path in files.values():
        if not path.is_file():
            raise SystemExit(f"Compile primeiro; artefato ausente: {path}")
    if not ROOT.is_dir():
        raise SystemExit("Raiz do projeto ausente")
    output = ROOT / "dist"
    output.mkdir(exist_ok=True)
    manifest = {"application_flash_offset": "0x10000", "files": {}}
    for name, source in files.items():
        target = output / name
        shutil.copy2(source, target)
        with target.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        manifest["files"][name] = {"bytes": target.stat().st_size, "sha256": digest}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    shutil.copy2(ROOT / "docs/ASSISTENTE_CELULAR.md", output / "CONFIGURACAO.md")
    print(f"Artefatos e hashes em: {output}")
    for name, info in manifest["files"].items():
        print(f"  {name}: {info['bytes']} bytes, SHA-256 {info['sha256']}")


if __name__ == "__main__":
    main()
