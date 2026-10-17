"""Standalone real-libopus codec tests; does not modify shared test runners.

Run: python test/host/voice_opus_check.py
Requires existing GCC/G++ (CC/CXX override); downloads no executables.
"""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("setup_opus", ROOT / "tools" / "setup_opus.py")
OPUS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OPUS)


def run(command: list[str], cwd: Path) -> None:
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, errors="replace", timeout=180)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="")
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {command[0]}")


def main() -> None:
    started = time.monotonic()
    run([sys.executable, str(ROOT / "test" / "host" / "voice_opus_setup_test.py")], ROOT)
    cc = shutil.which(os.environ.get("CC", "gcc"))
    cxx = shutil.which(os.environ.get("CXX", "g++"))
    ar = shutil.which(os.environ.get("AR", "ar"))
    if not cc or not cxx or not ar:
        raise RuntimeError("Existing GCC/G++/ar required; no toolchains are downloaded")
    # Keep all compiler output outside the accent-containing workspace path.
    build = Path(tempfile.gettempdir()) / "kilo" / "voice-opus-host"
    build.mkdir(parents=True, exist_ok=True)
    root = OPUS.prepare(build / "vendor")
    flags = ["-O2", "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections"]
    defines = [f"-D{name}" for name in OPUS.DEFINES]
    includes = [f"-I{folder}" for folder in OPUS.includes(root)]
    objects = []
    for name in OPUS.sources(root):
        obj = build / (name.replace("/", "_") + ".o")
        run([cc, "-std=c99", *flags, *defines, *includes,
             "-c", str(root / name), "-o", str(obj)], build)
        objects.append(str(obj))
    library = build / "libopus.a"
    run([ar, "rcs", str(library), *objects], build)
    host = ROOT / "test" / "host"
    # Copy only our own files for MinGW response-file/path portability.
    for name in ("voice_opus.cpp", "voice_opus.h"):
        destination = build / "audio" / name
        destination.parent.mkdir(exist_ok=True)
        shutil.copyfile(ROOT / "src" / "audio" / name, destination)
    shutil.copyfile(host / "voice_opus_tests.cpp", build / "voice_opus_tests.cpp")
    shutil.copyfile(host / "voice_opus_stubs" / "esp_heap_caps.h", build / "esp_heap_caps.h")
    exe = build / ("voice_opus_tests.exe" if os.name == "nt" else "voice_opus_tests")
    run([cxx, "-std=c++11", *flags, "-Wall", "-Wextra", "-Werror", "-DESP32",
         f"-I{build}", *includes, str(build / "audio" / "voice_opus.cpp"),
         str(build / "voice_opus_tests.cpp"), str(library), "-Wl,--gc-sections",
         "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free", "-o", str(exe)], build)
    run([str(exe)], build)
    print(f"VoiceOpus source+host build+tests: {time.monotonic() - started:.2f}s; license: {root / 'COPYING'}")


if __name__ == "__main__":
    main()
