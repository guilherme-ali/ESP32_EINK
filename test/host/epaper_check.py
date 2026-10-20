#!/usr/bin/env python3
"""Compile the production e-paper driver and Canvas with isolated boundary fakes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "test/host"


def main():
    cxx = shutil.which(os.environ.get("CXX", r"C:\Strawberry\c\bin\g++.exe" if os.name == "nt" else "g++"))
    if not cxx:
        raise RuntimeError("Host C++ compiler not found")
    build = HOST / ".build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="epaper-", dir=build) as temporary:
        exe = Path(temporary) / ("epaper_tests.exe" if os.name == "nt" else "epaper_tests")
        subprocess.run([cxx, "-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-Itest/host/epaper_stubs", "src/display/epaper.cpp", "src/ui/canvas.cpp",
                        "test/host/epaper_tests.cpp", "-o", str(exe.relative_to(ROOT))], cwd=ROOT, check=True, timeout=120)
        subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=30)


if __name__ == "__main__":
    main()
