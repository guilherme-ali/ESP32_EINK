#!/usr/bin/env python3
"""Build isolado Windows: Gradle oficial fixado e ZIP validado por SHA-256 oficial."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import tarfile
import urllib.request
import zipfile

VERSION = "8.13"
BASE = f"https://services.gradle.org/distributions/gradle-{VERSION}-bin.zip"
SHA256 = "20f1b1176237254a6fc204d8434196fa11a4cfb387567519c61556e8710aed78"
ROOT = Path(__file__).resolve().parent
OPUS_VERSION = "1.5.2"
OPUS_SHA256 = "65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1"


def prepare_opus(cache):
    """Official Xiph release; verify both download and extracted files each build."""
    archive = cache / f"opus-{OPUS_VERSION}.tar.gz"
    if not archive.exists():
        with urllib.request.urlopen(f"https://downloads.xiph.org/releases/opus/opus-{OPUS_VERSION}.tar.gz", timeout=60) as response:
            data = response.read(16 * 1024 * 1024 + 1)
        if hashlib.sha256(data).hexdigest() != OPUS_SHA256:
            raise SystemExit("SHA-256 libopus divergente")
        archive.write_bytes(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != OPUS_SHA256:
        raise SystemExit("SHA-256 libopus em cache divergente")
    with tarfile.open(archive, "r:gz") as bundle:
        for member in bundle.getmembers():
            parts = Path(member.name).parts
            if (not parts or parts[0] != f"opus-{OPUS_VERSION}" or ".." in parts
                    or "\\" in member.name or Path(member.name).is_absolute()
                    or not (member.isdir() or member.isfile())):
                raise SystemExit("Caminho inseguro no arquivo libopus")
            target = cache.joinpath(*parts)
            if target.is_symlink():
                raise SystemExit("Symlink no cache libopus")
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                with bundle.extractfile(member) as source:
                    data = source.read()
                if not target.is_file() or target.read_bytes() != data:
                    target.write_bytes(data)
    print(f"libopus {OPUS_VERSION} SHA-256 verificado: {OPUS_SHA256}", flush=True)
    return cache / f"opus-{OPUS_VERSION}"


def prepare_host_codec(cache, opus, work):
    """Execute the same JNI wrapper against authenticated libopus on the host JVM."""
    compiler = shutil.which("gcc")
    javac = shutil.which("javac")
    if not compiler or not javac or os.name != "nt":
        raise SystemExit("Testes JNI Windows exigem gcc e javac no PATH")
    java = Path(javac).resolve().parent.parent
    native = cache / "voice-jni-host"
    native.mkdir(exist_ok=True)
    sources = []
    for manifest, groups in (("opus_sources.mk", ("OPUS_SOURCES",)),
                             ("celt_sources.mk", ("CELT_SOURCES",)),
                             ("silk_sources.mk", ("SILK_SOURCES", "SILK_SOURCES_FIXED"))):
        text = (opus / manifest).read_text(encoding="utf-8").replace("\\\n", " ")
        for group in groups:
            match = re.search(rf"^{group}\s*=([^\n]+)", text, re.MULTILINE)
            if not match:
                raise SystemExit(f"Grupo libopus ausente: {group}")
            sources.extend(str(opus / source) for source in match.group(1).split())
    command = [compiler, "-shared", "-O2", "-std=gnu99", "-fno-strict-aliasing",
               "-DOPUS_BUILD", "-DFIXED_POINT", "-DDISABLE_FLOAT_API", "-DVAR_ARRAYS",
               "-DHAVE_LRINT", "-DHAVE_LRINTF", "-static-libgcc"]
    for include in (java / "include", java / "include/win32", opus / "include", opus,
                    opus / "celt", opus / "silk", opus / "silk/fixed"):
        command.extend(["-I", str(include)])
    command.extend([str(work / "app/src/main/cpp/voice_opus.c"), *sources, "-lm", "-o", str(native / "voice_opus.dll")])
    print("Compilando JNI/libopus real para testes JVM Windows", flush=True)
    subprocess.run(command, check=True, timeout=180)
    return native


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tasks", nargs="*", default=["assembleDebug", "testDebugUnitTest", "lintDebug"])
    args = parser.parse_args()
    parent = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "Temp" / "opencode"
    if not parent.is_dir():
        raise SystemExit(f"Pasta temporária preaprovada não existe: {parent}")
    cache = parent / "ideiarec-android-toolchain"
    cache.mkdir(exist_ok=True)
    opus = prepare_opus(cache)
    archive = cache / f"gradle-{VERSION}-bin.zip"
    expected = urllib.request.urlopen(BASE + ".sha256", timeout=60).read().decode("ascii").strip()
    if expected != SHA256:
        raise SystemExit("Checksum oficial diferente do SHA-256 fixado no projeto")
    actual = None
    if archive.exists():
        with archive.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual != expected:
        partial = archive.with_suffix(".download")
        urllib.request.urlretrieve(BASE, partial)
        with partial.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != expected:
            partial.unlink(missing_ok=True)
            raise SystemExit("SHA-256 Gradle divergente")
        partial.replace(archive)
    print(f"Gradle {VERSION} SHA-256 verificado: {expected}", flush=True)
    home = cache / f"gradle-{VERSION}"
    # Extrai novamente o ZIP validado: não confia em executável anteriormente extraído.
    if home.exists():
        shutil.rmtree(home)
    with zipfile.ZipFile(archive) as bundle:
        bundle.extractall(cache)
    sdk = Path(os.environ.get("ANDROID_HOME", str(Path.home() / "AppData/Local/Android/Sdk")))
    if not (sdk / "platforms/android-36/android.jar").is_file():
        raise SystemExit(f"SDK 36 ausente: {sdk}")
    env = dict(os.environ, ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk),
               GRADLE_USER_HOME=str(cache / "user-home"))
    # Cópia de fontes evita AGP/aapt em caminho com Unicode/Google Drive. Saídas só no temp.
    work = cache / "source"
    if work.exists():
        shutil.rmtree(work)
    shutil.copytree(ROOT, work, ignore=shutil.ignore_patterns(".gradle", "build", "local.properties", "__pycache__"))
    assets = work / "app/src/main/assets"
    assets.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(opus / "COPYING", assets / "libopus-COPYING.txt")
    native = prepare_host_codec(cache, opus, work) if any("test" in task.lower() for task in args.tasks) else None
    command = [str(home / "bin/gradle.bat"), "--no-daemon", "--console=plain", "--stacktrace",
               f"-PexternalBuildDir={cache / 'build'}", f"-PopusSourceDir={opus.as_posix()}", *args.tasks]
    if native:
        command.insert(4, f"-PvoiceHostDir={native.as_posix()}")
    print("Executando:", subprocess.list2cmdline(command), flush=True)
    raise SystemExit(subprocess.call(command, cwd=work, env=env))


if __name__ == "__main__":
    main()
