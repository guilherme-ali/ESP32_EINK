#!/usr/bin/env python3
"""Build isolado Windows: Gradle oficial fixado e ZIP validado por SHA-256 oficial."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

VERSION = "8.13"
BASE = f"https://services.gradle.org/distributions/gradle-{VERSION}-bin.zip"
SHA256 = "20f1b1176237254a6fc204d8434196fa11a4cfb387567519c61556e8710aed78"
ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tasks", nargs="*", default=["assembleDebug", "testDebugUnitTest", "lintDebug"])
    args = parser.parse_args()
    parent = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "Temp" / "opencode"
    if not parent.is_dir():
        raise SystemExit(f"Pasta temporária preaprovada não existe: {parent}")
    cache = parent / "ideiarec-android-toolchain"
    cache.mkdir(exist_ok=True)
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
    command = [str(home / "bin/gradle.bat"), "--no-daemon", "--console=plain", "--stacktrace",
               f"-PexternalBuildDir={cache / 'build'}", *args.tasks]
    print("Executando:", subprocess.list2cmdline(command), flush=True)
    raise SystemExit(subprocess.call(command, cwd=work, env=env))


if __name__ == "__main__":
    main()
