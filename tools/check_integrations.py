#!/usr/bin/env python3
"""Verifica integrações BLE, configurações, voz, gateway e opcionalmente Android."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "test" / "host"
BUILD = HOST / ".build"


def rel(path):
    try:
        return Path(path).resolve().relative_to(ROOT)
    except Exception:
        return Path(path)


def run(command, cwd=ROOT):
    rendered = [str(rel(x)) if isinstance(x, Path) and cwd == ROOT else str(x) for x in command]
    subprocess.run(rendered, cwd=cwd, check=True, timeout=600)


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--android", action="store_true", help="Inclui assembleDebug, testes e lint Android")
    args = parser.parse_args()
    cxx = shutil.which(os.environ.get("CXX", r"C:\Strawberry\c\bin\g++.exe" if os.name == "nt" else "g++"))
    if not cxx:
        raise RuntimeError("Compilador C++ host não encontrado")
    if not HOST.is_dir():
        raise RuntimeError("Diretório de testes não encontrado")
    BUILD.mkdir(exist_ok=True)
    flags = ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
    suffix = ".exe" if os.name == "nt" else ""
    def suite(name, sources, extra=()):
        exe = BUILD / (name + suffix)
        run([cxx, *flags, *extra, *sources, "-o", exe])
        run([exe])
    suite("relay_protocol_tests", [ROOT / "src/connectivity/relay_protocol.cpp", HOST / "relay_protocol_tests.cpp"])
    suite("network_route_tests", [ROOT / "src/net/network_client.cpp", ROOT / "src/net/http_client.cpp",
          HOST / "network_route_tests.cpp"], ["-DARDUINO_ARCH_ESP32", "-ffunction-sections", "-fdata-sections",
          "-Itest/host/network_route_tests_stubs", "-Itest/host/stubs", "-Wl,--gc-sections"])
    suite("network_route_tests_host", [HOST / "network_route_tests_host.cpp"],
          ["-Itest/host/network_route_tests_stubs", "-Itest/host/stubs"])
    suite("network_route_policy_tests", [ROOT / "src/connectivity/network_route.cpp",
          HOST / "network_route_policy_tests.cpp"], ["-DARDUINO_ARCH_ESP32", "-ffunction-sections",
          "-fdata-sections", "-Itest/host/network_route_tests_stubs", "-Itest/host/stubs", "-Wl,--gc-sections"])
    suite("connectivity_settings_tests", [ROOT / "src/net/settings.cpp", HOST / "connectivity_settings_tests.cpp"],
          ["-Itest/host/stubs"])
    suite("button_tests", [ROOT / "src/board/buttons.cpp", ROOT / "src/board/multi_button.c", HOST / "button_tests.cpp"],
          ["-Itest/host/button_tests_stubs"])
    run([sys.executable, HOST / "assistant_check.py"])
    run([sys.executable, HOST / "voice_opus_check.py"])
    benchmark_venv = ROOT / "tools/.venv-benchmark/Scripts/python.exe"
    if benchmark_venv.is_file():
        run([benchmark_venv, ROOT / "tools/benchmark_voice_test.py"])
    elif subprocess.run([sys.executable, "-c", "import numpy"], cwd=ROOT, check=False,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        run([sys.executable, ROOT / "tools/benchmark_voice_test.py"])
    else:
        print("SKIP: testes analiticos do benchmark requerem tools/requirements-benchmark.txt", flush=True)
    npm = shutil.which("npm")
    if not npm:
        raise RuntimeError("Node/npm necessário para verificar o gateway")
    for target in ("lint", "test", "build"):
        run([npm, "run", target], ROOT / "gateway")
    if args.android:
        run([sys.executable, ROOT / "companion-android/build_android.py", "assembleDebug", "testDebugUnitTest", "lintDebug"])
    run(["git", "-c", "core.safecrlf=false", "diff", "--check"])
    print("Integrações: verificações concluídas.")


if __name__ == "__main__":
    main()
