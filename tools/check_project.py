#!/usr/bin/env python3
"""Whitespace + executable tests of production C++ on the host (no PlatformIO)."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "test" / "host"
BUILD = HOST / ".build"
SOURCES = [ROOT / "src" / "net" / name for name in
           ("http_client.cpp", "json_utils.cpp", "gemini_client.cpp", "stt.cpp",
            "settings.cpp", "gdrive.cpp")]
SOURCES.append(ROOT / "src" / "storage" / "note_files.cpp")
SOURCES.extend(ROOT / "src" / "sync" / name for name in ("telemetry.cpp", "progress_model.cpp"))
CJSON_TAG = "v1.7.17"  # Same version as the installed Arduino ESP32 SDK header.
CJSON_ORIGIN = f"https://raw.githubusercontent.com/DaveGamble/cJSON/{CJSON_TAG}/"


def run(command: list[str], *, timeout: int = 120, diagnostics: bool = False,
        cwd: Path = ROOT) -> bool:
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout)
    def decode(data: bytes) -> str:
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = data.decode("oem" if os.name == "nt" else "utf-8", errors="replace")
        return text.replace("\r\n", "\n")
    stdout, stderr = decode(result.stdout), decode(result.stderr)
    if result.returncode:
        print((stdout + stderr).strip())
    elif stdout.strip():
        print(stdout.strip())
    if result.returncode == 0 and diagnostics and stderr.strip():
        print(stderr.strip())
    return result.returncode == 0


def find_cjson(explicit: str | None) -> Path:
    """Use official local sources first; prebuilt ESP32 libraries cannot link on x86."""
    if explicit:
        folder = Path(explicit).resolve()
        if not all((folder / name).is_file() for name in ("cJSON.c", "cJSON.h")):
            raise RuntimeError("--cjson-dir requer cJSON.c e cJSON.h oficiais")
        return folder
    platformio = Path.home() / ".platformio" / "packages"
    candidates = [HOST / "vendor" / "cJSON"]
    for base in (ROOT / ".pio", ROOT / "sdk", platformio / "framework-espidf",
                 platformio / "framework-arduinoespressif32"):
        if base.is_dir():
            candidates.extend(path.parent for path in base.rglob("cJSON.c"))
    candidates.append(BUILD / CJSON_TAG)
    for folder in candidates:
        if all((folder / name).is_file() for name in ("cJSON.c", "cJSON.h")):
            print(f"cJSON: {folder}")
            return folder
    folder = BUILD / CJSON_TAG
    folder.mkdir(parents=True, exist_ok=True)
    for name in ("cJSON.c", "cJSON.h"):
        with urllib.request.urlopen(CJSON_ORIGIN + name, timeout=30) as response:
            data = response.read()
        if b"Copyright (c) 2009-2017 Dave Gamble and cJSON contributors" not in data:
            raise RuntimeError(f"conteúdo inesperado do upstream oficial: {name}")
        (folder / name).write_bytes(data)
    print(f"cJSON: upstream oficial {CJSON_TAG} (cache local)")
    return folder


def main() -> int:
    # Windows redirected stdout may default to cp1252; diagnostics contain
    # Unicode paths (and replacement characters from external tools).
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", r"C:\Strawberry\c\bin\g++.exe"))
    parser.add_argument("--cjson-dir", default=os.environ.get("CJSON_DIR"))
    parser.add_argument("--cppcheck", action="store_true", help="lint opcional, se disponível")
    args = parser.parse_args()
    ok = run(["git", "-c", "core.safecrlf=false", "diff", "--check"])
    print(f"Whitespace: {'OK' if ok else 'FALHOU'}")
    BUILD.mkdir(parents=True, exist_ok=True)
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise RuntimeError(f"compilador host não encontrado: {args.cxx}")
    cjson = find_cjson(args.cjson_dir)
    production = list((ROOT / "src").rglob("*.cpp")) + list((ROOT / "src").rglob("*.h"))
    snapshot = {path: hashlib.sha256(path.read_bytes()).digest() for path in production}
    flags = ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra",
             "-ffunction-sections", "-fdata-sections", "-DCJSON_HIDE_SYMBOLS",
             "-I" + str(HOST / "stubs"), "-I" + str(cjson), "-I" + str(ROOT / "src" / "net")]
    # Compile the real cJSON implementation as C, not as an imitation/parser stub.
    obj = BUILD / "cJSON.o"
    compiled = run([cxx, "-x", "c", "-std=c89", "-O1", "-DCJSON_HIDE_SYMBOLS",
                    "-ffunction-sections", "-fdata-sections", "-c", str(cjson / "cJSON.c"),
                    "-o", str(obj)])
    for suite in ("host_tests", "storage_tests", "gemini_tests"):
        exe = BUILD / (suite + (".exe" if os.name == "nt" else ""))
        suite_compiled = compiled and run([
            cxx, *flags, *(str(path) for path in SOURCES), str(HOST / "fakes.cpp"),
            str(HOST / (suite + ".cpp")), str(obj), "-Wl,--gc-sections", "-o", str(exe)],
            diagnostics=True)
        print(f"Host build ({suite}): {'OK' if suite_compiled else 'FALHOU'}")
        if suite_compiled:
            if suite == "gemini_tests":
                listed = subprocess.run([str(exe), "--list"], capture_output=True, timeout=10, check=True)
                names = listed.stdout.decode("utf-8").splitlines()
                passed = sum(run([str(exe), str(index)], timeout=30) for index in range(len(names)))
                print(f"Gemini integration (processos isolados): {passed}/{len(names)} OK")
                ok = bool(names) and passed == len(names) and ok
            else:
                ok = run([str(exe)], timeout=30) and ok
        else:
            ok = False
    # Prova de portabilidade: nenhum include do Arduino/SDK nem mocks no modelo.
    portable = BUILD / ("progress_tests" + (".exe" if os.name == "nt" else ""))
    portable_ok = run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                       str(ROOT / "src" / "sync" / "progress_model.cpp"),
                       str(HOST / "progress_tests.cpp"), "-o", str(portable)], diagnostics=True)
    print(f"Host build (progress_tests, sem SDK): {'OK' if portable_ok else 'FALHOU'}")
    ok = (run([str(portable)], timeout=30) if portable_ok else False) and ok
    # There was no existing project linter/test runner; this lint is opt-in.
    if args.cppcheck:
        lint = shutil.which("cppcheck")
        if lint:
            # Relocated MinGW packages retain a build-time FILESDIR. The
            # official configs are installed under share/Cppcheck/cfg; cwd is a
            # supported search location, without editing the installation.
            config = Path(lint).resolve().parent.parent / "share" / "Cppcheck" / "cfg"
            lint_cwd = config if (config / "std.cfg").is_file() else ROOT
            lint_ok = run([lint, "--enable=warning,performance,portability", "--error-exitcode=1",
                           "--std=c++17", "--inline-suppr", "--quiet", "-DCJSON_HIDE_SYMBOLS",
                           "--suppress=normalCheckLevelMaxBranches",
                           "-I" + str(HOST / "stubs"), "-I" + str(cjson),
                           *(str(path) for path in SOURCES)], cwd=lint_cwd,
                          diagnostics=True)
            print(f"cppcheck: {'OK' if lint_ok else 'FALHOU'}")
            ok = lint_ok and ok
        else:
            print("cppcheck: indisponível (opcional)")
    current_production = set((ROOT / "src").rglob("*.cpp")) | set((ROOT / "src").rglob("*.h"))
    if current_production != set(snapshot) or any(
            not path.is_file() or hashlib.sha256(path.read_bytes()).digest() != digest
            for path, digest in snapshot.items()):
        print("Host: produção mudou durante a execução; rode o checker novamente")
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"Checker: FALHOU: {error}", file=sys.stderr)
        sys.exit(1)
