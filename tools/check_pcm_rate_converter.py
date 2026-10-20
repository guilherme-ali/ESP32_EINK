#!/usr/bin/env python3
"""Compila e testa o conversor PCM portavel, sem executar outras suites."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main():
    default = r"C:\Strawberry\c\bin\g++.exe" if os.name == "nt" else "g++"
    compiler = shutil.which(os.environ.get("CXX", default))
    if not compiler:
        raise RuntimeError("Compilador C++ host nao encontrado; configure CXX")
    with tempfile.TemporaryDirectory(prefix="pcm-rate-converter-") as directory:
        exe = Path(directory) / ("pcm_tests.exe" if os.name == "nt" else "pcm_tests")
        subprocess.run([
            compiler, "-std=c++11", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic",
            str(ROOT / "test/host/pcm_rate_converter_tests.cpp"), "-o", str(exe),
        ], cwd=ROOT, check=True, timeout=120)
        subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=120)


if __name__ == "__main__":
    main()
