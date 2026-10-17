"""Build official libopus as portable fixed-point C, without float/SIMD/DNN.

PIO post script, also importable by the codec-only host tests. Downloads source
only, verifies the release digest, and retains COPYING in the build cache.
Release/hash: https://opus-codec.org/release/stable/2024/04/12/libopus-1_5_2.html
"""
from __future__ import annotations

import hashlib
from pathlib import Path, PurePosixPath
import re
import tarfile
import urllib.request

VERSION = "1.5.2"
SHA256 = "65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1"
URL = f"https://downloads.xiph.org/releases/opus/opus-{VERSION}.tar.gz"
# Both the ESP32 newlib toolchain and the host C99 toolchain provide lrint[f].
# float_cast.h is included upstream even with the floating-point API disabled.
DEFINES = ("OPUS_BUILD", "FIXED_POINT", "DISABLE_FLOAT_API", "VAR_ARRAYS",
           "HAVE_LRINT", "HAVE_LRINTF")


def prepare(cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / f"opus-{VERSION}.tar.gz"
    if not archive.is_file():
        print(f"Opus: downloading official source {VERSION}")
        with urllib.request.urlopen(URL, timeout=60) as response:
            data = response.read(16 * 1024 * 1024 + 1)
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise RuntimeError("Opus source download SHA-256 mismatch")
        temporary = archive.with_suffix(".tmp")
        temporary.write_bytes(data)
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError(f"Opus cached archive SHA-256 mismatch: {archive}")

    # Verify cached sources against the authenticated archive too. Never trust
    # an extracted-tree marker or execute anything shipped in the archive.
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar.getmembers():
            name = PurePosixPath(member.name)
            if (not name.parts or name.parts[0] != f"opus-{VERSION}" or
                    name.is_absolute() or ".." in name.parts or "\\" in member.name or
                    not (member.isdir() or member.isfile())):
                raise RuntimeError(f"Unsafe Opus archive member: {member.name}")
            target = cache.joinpath(*name.parts)
            if target.is_symlink():
                raise RuntimeError(f"Symlink in Opus cache: {target}")
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            with tar.extractfile(member) as source:
                data = source.read()
            if not target.is_file() or target.read_bytes() != data:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
    return cache / f"opus-{VERSION}"


def sources(root: Path) -> list[str]:
    selected = []
    for manifest, groups in (
        ("opus_sources.mk", ("OPUS_SOURCES",)),
        ("celt_sources.mk", ("CELT_SOURCES",)),
        ("silk_sources.mk", ("SILK_SOURCES", "SILK_SOURCES_FIXED")),
    ):
        text = (root / manifest).read_text(encoding="utf-8").replace("\\\n", " ")
        for group in groups:
            match = re.search(rf"^{group}\s*=([^\n]+)", text, re.MULTILINE)
            if not match:
                raise RuntimeError(f"Missing Opus source group: {group}")
            selected.extend(match.group(1).split())
    if not selected or any(not (root / name).is_file() for name in selected):
        raise RuntimeError("Incomplete Opus source list")
    return selected


def includes(root: Path) -> list[str]:
    return [str(root / folder) for folder in ("include", "", "celt", "silk", "silk/fixed")]


def setup_pio(env, projenv) -> None:
    if env.IsCleanTarget():
        return
    if env.PioPlatform().name != "espressif32":
        raise RuntimeError("VoiceOpus is supported only on ESP32")
    root = prepare(Path(env.subst("$PROJECT_BUILD_DIR")) / "voice-opus")
    env.AppendUnique(CPPPATH=[str(root / "include")])
    projenv.AppendUnique(CPPPATH=[str(root / "include")])
    opus = env.Clone()
    opus.AppendUnique(CPPPATH=includes(root), CPPDEFINES=list(DEFINES))
    # C99 VLA scratch uses task stack, not malloc in encode/decode. The generic
    # manifests above intentionally exclude x86/AVX, ARM, float and DNN sources.
    opus.Append(CCFLAGS=["-O2", "-fno-strict-aliasing", "-fstack-usage"],
                CFLAGS=["-std=gnu99"])
    library = opus.BuildLibrary(
        env.subst("$BUILD_DIR/voice-opus"), str(root),
        src_filter="-<*> " + " ".join(f"+<{name}>" for name in sources(root)),
    )
    env.Prepend(LIBS=[library])
    print(f"Opus: {VERSION} fixed-point, float API disabled; license: {root / 'COPYING'}")


# SCons injects Import when this file runs as an extra_script. Importing the
# same preparation functions from a host test must not launch PlatformIO.
if "Import" in globals():
    Import("env", "projenv")
    setup_pio(env, projenv)
