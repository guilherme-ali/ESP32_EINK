#!/usr/bin/env python3
"""Test the real voice worker/renderer/EPD with threaded RTOS and SDK boundary fakes."""
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
    with tempfile.TemporaryDirectory(prefix="listening-feedback-", dir=build) as temporary:
        for cdc in (0, 1):
            exe = Path(temporary) / (f"feedback_{cdc}.exe" if os.name == "nt" else f"feedback_{cdc}")
            subprocess.run([cxx, "-std=c++11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-Wframe-larger-than=768",
                            "-pthread", f"-DARDUINO_USB_CDC_ON_BOOT={cdc}",
                            "-Itest/host/listening_feedback_stubs", "-Itest/host/epaper_stubs",
                            "src/display/epaper.cpp", "src/ui/canvas.cpp", "src/ui/screens.cpp",
                            "src/ui/listening_feedback.cpp", "test/host/listening_feedback_tests.cpp",
                            "-o", str(exe.relative_to(ROOT))], cwd=ROOT, check=True, timeout=120)
            subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=45)


if __name__ == "__main__":
    main()
