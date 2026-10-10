#!/usr/bin/env python3
"""Regressao host de finalizacao: LittleFS upstream v2.9.3 real, NOR em memoria.

Executa somente littlefs_recording_tests.cpp, sem PlatformIO ou hardware.
Documentacao consultada: README.md, DESIGN.md e lfs.h oficiais da tag v2.9.3.
Cache, objetos, bytecode e executavel ficam em test/host/.build/ (ignorado).
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import py_compile
import shutil
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "test" / "host"
BUILD = HOST / ".build"
TAG = "v2.9.3"
UPSTREAM = f"https://raw.githubusercontent.com/littlefs-project/littlefs/{TAG}/"
VENDOR = BUILD / f"littlefs-{TAG}"
SUITE = HOST / "littlefs_recording_tests.cpp"
# Pin por conteudo, inclusive para caches existentes: nao aceita fake, outra
# release ou alteracao posterior da tag. Os sources nao sao editados.
SHA256 = {
    "lfs.c": "4a221443c1936be4d41d8f400cd2fd055695196acadef35fcb227eb7e90f6db5",
    "lfs.h": "37cefbc6983f19ada481bda913199d917cbb8c82d53a43dca369d62f669c64e6",
    "lfs_util.c": "f2fbde533670560434bd9f5a547174cc7c5a4670a02c47b4bd85180dced8b2ec",
    "lfs_util.h": "03e912a6e9894c9d10c61f5da22b89ebe0bb778af67972d7b67a5f160731bf72",
}


def run(command: list[str], *, timeout: int = 120) -> None:
    result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=timeout)
    for data in (result.stdout, result.stderr):
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = data.decode("oem" if os.name == "nt" else "utf-8", errors="replace")
        if text.strip():
            print(text.replace("\r\n", "\n").strip(), flush=True)
    if result.returncode:
        raise RuntimeError(f"comando falhou (exit {result.returncode}): {command[0]}")


def sources() -> Path:
    VENDOR.mkdir(parents=True, exist_ok=True)
    for name, expected in SHA256.items():
        path = VENDOR / name
        if path.is_file():
            data = path.read_bytes()
        else:
            with urllib.request.urlopen(UPSTREAM + name, timeout=30) as response:
                data = response.read()
        actual = hashlib.sha256(data).hexdigest()
        if actual != expected:
            raise RuntimeError(f"SHA-256 upstream divergente: {name}: {actual}")
        if not path.is_file():
            path.write_bytes(data)
        print(f"  {name}: SHA-256 {actual}", flush=True)
    print(f"LittleFS: upstream oficial {TAG}, quatro hashes verificados; cache {VENDOR}", flush=True)
    return VENDOR


def lint() -> None:
    # Lint restrito aos dois arquivos desta regressao, inclusive untracked.
    for path in (SUITE, Path(__file__).resolve()):
        text = path.read_text(encoding="utf-8")
        if not text.endswith("\n"):
            raise RuntimeError(f"newline final ausente: {path}")
        for line_number, line in enumerate(text.splitlines(), 1):
            if line.rstrip() != line:
                raise RuntimeError(f"whitespace final: {path}:{line_number}")
    py_compile.compile(str(Path(__file__).resolve()),
                       cfile=str(BUILD / "check_littlefs_recording.pyc"), doraise=True)
    print("Lint local: whitespace + sintaxe Python OK; C/C++ com -Wall -Wextra -Werror", flush=True)


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    default_cxx = r"C:\Strawberry\c\bin\g++.exe" if os.name == "nt" else "g++"
    parser.add_argument("--cxx", default=os.environ.get("CXX", default_cxx))
    args = parser.parse_args()
    cxx = shutil.which(args.cxx)
    if not cxx:
        raise RuntimeError(f"compilador host nao encontrado: {args.cxx}")
    # Recusa gerar cache/objetos se a pasta deixou de ser ignorada pelo git.
    run(["git", "check-ignore", "-q", "test/host/.build/littlefs-v2.9.3/lfs.c"])
    BUILD.mkdir(parents=True, exist_ok=True)
    lint()
    upstream = sources()
    flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-DLFS_NO_MALLOC",
             "-DLFS_NO_DEBUG", "-DLFS_NO_WARN", "-DLFS_NO_ERROR", "-I" + str(upstream)]
    objects = []
    for name in ("lfs", "lfs_util"):
        obj = VENDOR / f"{name}.o"
        # Compila upstream como C99 real, nao como C++ e nao como stub.
        run([cxx, "-x", "c", "-std=c99", *flags, "-c", str(upstream / f"{name}.c"),
             "-o", str(obj)])
        objects.append(str(obj))
    executable = BUILD / ("littlefs_recording_tests" + (".exe" if os.name == "nt" else ""))
    run([cxx, "-std=c++17", "-Wpedantic", *flags, str(SUITE), *objects, "-o", str(executable)])
    print("Build LittleFS recording: OK (upstream C99 + suite C++17)", flush=True)
    run([str(executable)], timeout=120)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired, py_compile.PyCompileError) as error:
        print(f"LittleFS checker: FALHOU: {error}", file=sys.stderr)
        sys.exit(1)
