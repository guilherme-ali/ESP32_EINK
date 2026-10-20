"""Testa apenas parser/veredito reais do self-test USB, sem PIO ou hardware."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    default = r"C:\Strawberry\c\bin\g++.exe" if os.name == "nt" else "g++"
    compiler = shutil.which(os.environ.get("CXX", default))
    if not compiler:
        raise RuntimeError("Compilador C++ host nao encontrado; configure CXX")
    subprocess.run([
        compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
        "-DARDUINO_USB_CDC_ON_BOOT=0", "src/audio/power_selftest.cpp",
    ], cwd=ROOT, check=True, timeout=30)
    with tempfile.TemporaryDirectory(prefix="power-selftest-") as temporary:
        binary = Path(temporary) / ("parser.exe" if os.name == "nt" else "parser")
        subprocess.run([
            compiler, "-std=c++17", "-O1", "-flto", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections", "-DARDUINO_USB_CDC_ON_BOOT=1",
            "-Itest/host/power_selftest_stubs", "-Itest/host/stubs",
            "test/host/power_selftest_tests.cpp", "-Wl,--gc-sections", "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=120)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30)


if __name__ == "__main__":
    main()
